#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

/*
 * This device is part of the mount driver, devmnt.c
 *
 * The devtab implemented here is used to defer calls that
 * will be performed by the mount driver and issue them in
 * a single round trip.
 *
 * Entry points for 9p ops assume the usage made by the
 * nameop routine, trying to batch as many ops at a time as
 * feasible. Beware.
 *
 * This can be used only with 9P2000.ix severs,
 * 9P2000 server requests are never deferred.
 * The first three functions are used by namec to switch.
 *
 * We could make the mount driver initialize a later chan when its
 * walk is called, and add a walked() function to force evaluation, but,
 * that would mean that people calling walk by hand on the mount driver
 * would be mistaken. Thus, better to expose the lazy evaluation on the caller.
 * 
 */

extern void	mntfree(Mntrpc*);
extern int	mntabort(Mntrpc*);
extern Mntrpc*	mntclunked(Mntrpc *r);
extern Mntrpc*	mntclunking(Mntrpc *prev, Chan *c, int t);
extern Mntrpc*	mntwalked(Mntrpc *r, Walkqid **wqp);
extern Mntrpc*	mntwalking(Chan *c, char **name, int nname);
extern Mntrpc*	mntwstated(Mntrpc *r);
extern Mntrpc*	mntwstating(Mntrpc *prev, Chan *c, uchar *dp, long n);
extern Mntrpc*	mntopencreated(Mntrpc *r, Chan **cp);
extern Mntrpc*	mntopencreating(Mntrpc *prev, int type, Chan *c, char *name, int omode, ulong perm);
extern Mntrpc*	mntstated(Mntrpc *r, uchar *dp, long *np);
extern Mntrpc*	mntstating(Mntrpc *prev, Chan *c);
extern Chan*	clwalk(Chan *c, Path *p);


int nolater;

static int
canwalklater(Chan *c)
{
	Mnt *mnt;

	if(c->mchan == nil || c->dev == nil || c->dev->dc != 'M' || nolater)
		return 0;
	mnt = c->mchan->mux;
	if(mnt == nil)
		return 0;
	/* could force the chan to be CCACHE, but asking for 9pix suffices */
	return mnt->sharedtags;
}

Chan*
walklater(Chan *c, Path *p)
{
	Chan *nc;

	if(!canwalklater(c))
		return nil;

	DBG("walk later %N → %N\n", c->path, p);
	nc = devattach(L'λ', "");
	pathclose(nc->path);
	nc->path = p;
	incref(p);
	nc->lchan = c;
	incref(c);
	nc->ismtpt = c->ismtpt;
	nc->flag |= c->flag&CCACHE;
	return nc;
}

Chan*
walked(Chan *c)
{
	Chan *nc;

	if(c->lchan == nil)		/* not a 'λ' chan or already walked */
		return c;
	DBG("walked %N\n", c->path);
	nc = clwalk(c->lchan, c->path);
	cclose(c->lchan);		/* usually a decref */
	c->lchan = nil;
	pathclose(nc->path);
	nc->path = c->path;
	incref(c->path);
	cclose(c);
	return nc;
}

static Chan*
lattach(char *)
{
	panic("lattach");
	return nil;
}

static Walkqid*
lwalk(Chan *, Chan *, char **, int)
{
	panic("lwalk");
	return nil;
}

static Chan*
walkit(Chan *c)
{
	Chan *nc;

	if(c->lchan->path == c->path)
		return c->lchan;

	nc = clwalk(c->lchan, c->path);
	cclose(c->lchan);
	c->lchan = nc;
	pathclose(nc->path);
	nc->path = c->path;
	incref(c->path);
	return nc;
}

static long
lrwstat(Chan *c, uchar *dp, long nd, long isw)
{
	Mntrpc *r0, *r;
	Path *p;
	Walkqid *wq;

	p = c->path;
	r0 = mntwalking(c->lchan, p->els+p->nres, p->nels - p->nres);
	if(waserror()){
		mntabort(r0);
		nexterror();
	}
	if(isw)
		r = mntwstating(r0, r0->wq->clone, dp, nd);
	else
		r = mntstating(r0, r0->wq->clone);
	mntclunking(r, r0->wq->clone, Tclunk);

	r = mntwalked(r0, &wq);
	if(wq == nil || wq->nqid < p->nels-p->nres)
		error(up->errstr);

	if(isw)
		r = mntwstated(r);
	else
		r = mntstated(r, dp, &nd);
	mntclunked(r);
	poperror();
	mntfree(r0);
	cclose(c->lchan);	/* usually just a decref */
	c->lchan = nil;

	return nd;
}

/*
 * walk, stat, and clunk.
 */
static long
lstat(Chan *c, uchar *dp, long nd)
{
	if(c->lchan == nil)
		panic("devlater: called too late pc %#p", getcallerpc(&c));
	DBG("lstat %N", c->path);
	return lrwstat(c, dp, nd, 0);
}

/*
 * walk, wstat, and clunk.
 */
static long
lwstat(Chan *c, uchar *dp, long nd)
{
	if(c->lchan == nil)
		panic("devlater: called too late pc %#p", getcallerpc(&c));
	DBG("lwstat %N", c->path);
	return lrwstat(c, dp, nd, 1);
}

/*
 * walk and remove.
 */
static void
lremove(Chan *c)
{
	Mntrpc *r0, *r;
	Path *p;
	Walkqid *wq;

	if(c->lchan == nil)
		panic("devlater: called too late pc %#p", getcallerpc(&c));
	p = c->path;
	DBG("lremove %N", p);
	r0 = mntwalking(c->lchan, p->els+p->nres, p->nels - p->nres);
	if(waserror()){
		mntabort(r0);
		nexterror();
	}
	mntclunking(r0, r0->wq->clone, Tremove);

	r = mntwalked(r0, &wq);
	if(wq == nil || wq->nqid < p->nels-p->nres)
		error(up->errstr);
	mntclunked(r);
	poperror();
	mntfree(r0);
	cclose(c->lchan);		/* usually just a decref */
	c->lchan = nil;
}

static Chan*
lopenncreate(int type, Chan *c, char *name, int omode, int perm)
{
	Chan *nc;
	Mntrpc *r0, *r;
	Path *p;
	Walkqid *wq;

	if(c->lchan == nil)
		panic("devlater: called too late pc %#p", getcallerpc(&c));
	p = c->path;
	DBG("lopenncreate %N", p);
	r0 = mntwalking(c->lchan, p->els+p->nres, p->nels - p->nres);
	if(waserror()){
		mntabort(r0);
		nexterror();
	}
	mntopencreating(r0, type, r0->wq->clone, name, omode, perm);
	r = mntwalked(r0, &wq);
	if(wq == nil || wq->nqid < p->nels-p->nres)
		error(up->errstr);
	mntopencreated(r, &nc);
	assert(nc == r0->wq->clone);
	r0->wq->clone = nil;

	poperror();
	mntfree(r0);
	cclose(c->lchan);	/* usually just a decref */
	c->lchan = nil;
	cclose(c);
	return nc;
}

/*
 * walk and open
 *
 */
static Chan*
lopen(Chan *c, int omode)
{
	return lopenncreate(Topen, c, nil, omode, 0);
}

static void
lcreate(Chan *, char *, int, int)
{
	panic("lcreate");
}

/*
 * walk and create with OTRUNC.
 *
 * (In 9pix create with OTRUNC means create and, if exists, walk
 * and open OTRUNC.)
 */
static Chan*
lncreate(Chan *c, char *name, int omode, int perm)
{
	return lopenncreate(Tcreate, c, name, omode|OTRUNC, perm);
}

static void
lclose(Chan *c)
{
	if(c->lchan != nil){
		cclose(c->lchan);
		c->lchan = nil;
	}
}

/*
 * All other requests should never be called, because namec
 * would call one of the previous requests before giving the
 * Chan to its caller.
 * The code after the call to panic is what we could do if we
 * wanted them to work.
 */

static long
lwrite(Chan *c, void *a, long n, vlong off)
{
	Chan *nc;

	panic("lwrite");
	nc = walkit(c);
	return nc->dev->write(nc, a, n, off);
}

/*
 * This one could be accepted and used from sysexec to
 * walk, open, and read a few bytes.
 */
static long
lread(Chan *c, void *a, long n, vlong off)
{
	Chan *nc;

	panic("lread");
	nc = walkit(c);
	return nc->dev->read(nc, a, n, off);
}

static Block*
lbread(Chan *c, long n, vlong off)
{
	Chan *nc;

	panic("lbread");
	nc = walkit(c);
	return nc->dev->bread(nc, n, off);
}

static long
lbwrite(Chan *c, Block *bp, vlong off)
{
	Chan *nc;

	panic("lbwrite");
	nc = walkit(c);
	return nc->dev->bwrite(nc, bp, off);
}

static void
laterinit(void)
{
	if(getconf("*nolater"))
		nolater = 1;
}

Dev laterdevtab = {
	L'λ',
	"later",

	devreset,
	laterinit,
	devshutdown,
	lattach,
	lwalk,
	lstat,
	lopen,
	lcreate,
	lclose,
	lread,
	lbread,
	lwrite,
	lbwrite,
	lremove,
	lwstat,
	nil,		/* power */
	nil,		/* config */
	lncreate,
};
