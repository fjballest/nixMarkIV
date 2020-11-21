#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"
#include	"ureg.h"

/*
 * 9P2000.ix RPCs can share the same tag if they make a group.
 * They are linked through Mntrpc.tagnext in such case, and Mntrpc.owntag
 * indicates if deallocation of the RPC implies a deallocation of the tag.
 *
 * References are managed as follows:
 * The channel to the server - a network connection or pipe - has one
 * reference for every Chan open on the server.  The server channel has
 * c->mux set to the Mnt used for muxing control to that server.  Mnts
 * have no reference count; they go away when c goes away.
 * Each channel derived from the mount point has mchan set to c,
 * and increfs/decrefs mchan to manage references on the server
 * connection.
 */

#define VERSION9PIX VERSION9P ".ix"

enum
{
	TAGSHIFT = 5,		/* ulong has to be 32 bits */
	TAGMASK = (1<<TAGSHIFT)-1,
	NMASK = (64*1024)>>TAGSHIFT,

	MAXDATA = 8192,
	MAXRPC = IOHDRSZ+MAXDATA,
	NRPCS = 0,		/* rpcs kept in free list; 0: unlimited */
};

struct Mntalloc
{
	Lock;
	Mnt*	list;		/* Mount devices in use */
	Mnt*	mntfree;	/* Free list */
	Mntrpc*	rpcfreehd;
	Mntrpc*	rpcfreetl;
	int	nrpcfree;
	int	nrpcused;
	uint	id;
	ulong	tagmask[NMASK];
}mntalloc;

int	mntabort(Mntrpc*);
Mnt*	mntchk(Chan*);
void	mntdirfix(uchar*, Chan*);
Mntrpc*	mntflushalloc(Mntrpc*, ulong);
void	mntfree(Mntrpc*);
void	mntgate(Mnt*);
void	mntpntfree(Mnt*);
void	mntqrm(Mnt*, Mntrpc*, int);
Mntrpc*	mntralloc(Mntrpc*, Chan*, ulong, int);
int	mntrpcread(Mnt*, Mntrpc*);
void	mountmux(Mnt*, Mntrpc*);
void	mountrpcreq(Mnt*, Mntrpc*);
void	mountrpcrep(Mntrpc*);
int	rpcattn(void*);
Chan*	mntchan(void);

char	Esbadstat[] = "invalid directory entry received from server";
char	Enoversion[] = "version not established for mount channel";

void (*mntstats)(int, Chan*, uvlong, ulong);
#pragma	varargck	type	"G"	Fcall*

static char*
mntsummary(char *s, char *e, void*)
{
	return seprint(s, e, "%d/%d rpcs\n",
		mntalloc.nrpcused, mntalloc.nrpcused+mntalloc.nrpcfree);
}

#define	QIDFMT	"(%.16llux %lud %x)"
static int
kfcallfmt(Fmt *fmt)
{
	Fcall *f;
	int fid, type, tag, i;
	char buf[512];
	char *p, *e;
	Qid *q;

	e = buf+sizeof(buf);
	f = va_arg(fmt->args, Fcall*);
	type = f->type;
	fid = f->fid;
	tag = f->tag;
	switch(type){
	case Tversion:	/* 100 */
		seprint(buf, e, "Tversion tag %ud msize %ud version '%s'", tag, f->msize, f->version);
		break;
	case Rversion:
		seprint(buf, e, "Rversion tag %ud msize %ud version '%s'", tag, f->msize, f->version);
		break;
	case Tauth:	/* 102 */
		seprint(buf, e, "Tauth tag %ud afid %d uname %s aname %s", tag,
			f->afid, f->uname, f->aname);
		break;
	case Rauth:
		seprint(buf, e, "Rauth tag %ud qid " QIDFMT, tag,
			f->aqid.path, f->aqid.vers, f->aqid.type);
		break;
	case Tattach:	/* 104 */
		seprint(buf, e, "Tattach tag %ud fid %d afid %d uname %s aname %s", tag,
			fid, f->afid, f->uname, f->aname);
		break;
	case Rattach:
		seprint(buf, e, "Rattach tag %ud qid " QIDFMT, tag,
			f->qid.path, f->qid.vers, f->qid.type);
		break;
	case Rerror:	/* 107; 106 (Terror) illegal */
		seprint(buf, e, "Rerror tag %ud ename %s", tag, f->ename);
		break;
	case Tflush:	/* 108 */
		seprint(buf, e, "Tflush tag %ud oldtag %ud", tag, f->oldtag);
		break;
	case Rflush:
		seprint(buf, e, "Rflush tag %ud", tag);
		break;
	case Twalk:	/* 110 */
		p = seprint(buf, e, "Twalk tag %ud fid %d newfid %d nwname %d ", tag, fid, f->newfid, f->nwname);
		if(f->nwname <= MAXWELEM)
			for(i=0; i<f->nwname; i++)
				p = seprint(p, e, "%d:%s ", i, f->wname[i]);
		break;
	case Rwalk:
		p = seprint(buf, e, "Rwalk tag %ud nwqid %ud ", tag, f->nwqid);
		if(f->nwqid <= MAXWELEM)
			for(i=0; i<f->nwqid; i++){
				q = &f->wqid[i];
				p = seprint(p, e, "%d:" QIDFMT " ", i,
					q->path, q->vers, q->type);
			}
		break;
	case Topen:	/* 112 */
		seprint(buf, e, "Topen tag %ud fid %ud mode %d", tag, fid, f->mode);
		break;
	case Ropen:
		seprint(buf, e, "Ropen tag %ud qid " QIDFMT " iounit %ud ", tag,
			f->qid.path, f->qid.vers, f->qid.type, f->iounit);
		break;
	case Tcreate:	/* 114 */
		seprint(buf, e, "Tcreate tag %ud fid %ud name %s perm %ulx mode %d", tag, fid, f->name, (ulong)f->perm, f->mode);
		break;
	case Rcreate:
		seprint(buf, e, "Rcreate tag %ud qid " QIDFMT " iounit %ud ", tag,
			f->qid.path, f->qid.vers, f->qid.type, f->iounit);
		break;
	case Tread:	/* 116 */
		seprint(buf, e, "Tread tag %ud fid %d offset %lld count %ud",
			tag, fid, f->offset, f->count);
		break;
	case Rread:
		seprint(buf, e, "Rread tag %ud count %ud ", tag, f->count);
		break;
	case Twrite:	/* 118 */
		seprint(buf, e, "Twrite tag %ud fid %d offset %lld count %ud ",
			tag, fid, f->offset, f->count);
		break;
	case Rwrite:
		seprint(buf, e, "Rwrite tag %ud count %ud", tag, f->count);
		break;
	case Tclunk:	/* 120 */
		seprint(buf, e, "Tclunk tag %ud fid %ud", tag, fid);
		break;
	case Rclunk:
		seprint(buf, e, "Rclunk tag %ud", tag);
		break;
	case Tremove:	/* 122 */
		seprint(buf, e, "Tremove tag %ud fid %ud", tag, fid);
		break;
	case Rremove:
		seprint(buf, e, "Rremove tag %ud", tag);
		break;
	case Tstat:	/* 124 */
		seprint(buf, e, "Tstat tag %ud fid %ud", tag, fid);
		break;
	case Rstat:
		seprint(buf, e, "Rstat tag %ud stat(%d bytes)", tag, f->nstat);
		break;
	case Twstat:	/* 126 */
		seprint(buf, e, "Twstat tag %ud fid %ud stat(%d bytes)", tag, fid, f->nstat);
		break;
	case Rwstat:
		seprint(buf, e, "Rwstat tag %ud", tag);
		break;
	default:
		seprint(buf, e,  "unknown type %d", type);
	}
	return fmtstrcpy(fmt, buf);
}

static void
devmntdump(void*)
{
	Mnt *mnt;
	Proc *p;
	Chan *c;

	for(mnt = mntalloc.list; mnt != nil; mnt = mnt->list){
		c = mnt->c;
		p = mnt->rip;
		print("mnt %N rip %d\n", c->path, p?p->pid:0);
	}
}

static void
mntreset(void)
{
	mntalloc.id = 1;
	mntalloc.tagmask[0] = 1;			/* don't allow 0 as a tag */
	mntalloc.tagmask[NMASK-1] = 0x80000000UL;	/* don't allow NOTAG */
	/* We can't install %M since eipfmt does and is used in the kernel [sape] */
	/* Then we can't install %F either, use one skipping stats [nemo] */
	fmtinstall('G', kfcallfmt);
	fmtinstall('D', dirfmt);

	addsummary(mntsummary, nil);
	addttescape('M', devmntdump, nil);
	cinit();
}

/*
 * Version is not multiplexed: message sent only once per connection.
 */
usize
mntversion(Chan *c, u32int msize, char *version, usize returnlen)
{
	Mntrpc *mntr;
	Fcall fc;
	Fcall *f;
	uchar *msg;
	Mnt *mnt;
	char *v;
	long l, n;
	usize k;
	vlong oo;
	char buf[128];

	/* make sure no one else does this until we've established ourselves */
	qlock(&c->umqlock);
	if(waserror()){
		qunlock(&c->umqlock);
		nexterror();
	}

	/* defaults */
	if(msize == 0)
		msize = MAXRPC;
	if(msize > c->iounit && c->iounit != 0)
		msize = c->iounit;
	v = version;
	if(v == nil || v[0] == '\0')
		v = VERSION9PIX;

	if(strncmp(v, VERSION9P, strlen(VERSION9P)) != 0)
		error("bad 9P version specification");

	mnt = c->mux;

	if(mnt != nil){
		qunlock(&c->umqlock);
		poperror();

		strecpy(buf, buf+sizeof buf, mnt->version);
		k = strlen(buf);
		if(strncmp(buf, v, k) != 0){
			snprint(buf, sizeof buf, "incompatible 9P versions %s %s", mnt->version, v);
			error(buf);
		}
		if(returnlen != 0){
			if(returnlen < k)
				error(Eshort);
			memmove(version, buf, k);
		}
		return k;
	}

	mntr = mntralloc(nil, nil, MAXRPC, -1);
	msg = mntr->rpc;
	memset(&fc, 0, sizeof fc);
	f = &fc;
	if(waserror()){
		mntfree(mntr);
		nexterror();
	}
	f->type = Tversion;
	f->tag = NOTAG;
	f->msize = msize;
	f->version = v;

	k = convS2M(f, msg, MAXRPC);
	if(k == 0)
		error("bad fversion conversion on send");

	lock(c);
	oo = c->offset;
	c->offset += k;
	unlock(c);

	l = c->dev->write(c, msg, k, oo);

	if(l < k){
		lock(c);
		c->offset -= k - l;
		unlock(c);
		error("short write in fversion");
	}

	/* message sent; receive and decode reply */
	n = c->dev->read(c, msg, MAXRPC, c->offset);
	if(n <= 0)
		error("EOF receiving fversion reply");

	lock(c);
	c->offset += n;
	unlock(c);

	l = convM2S(msg, n, f);
	if(l != n)
		error("bad fversion conversion on reply");
	if(f->type != Rversion){
		if(f->type == Rerror){
			/* too drastic: panic by now to spot bugs in 9P2000.ix */
			if(strstr(f->ename, "fid") && strstr(f->ename, "in use"))
				panic("mntversion: %s", f->ename);
			error(f->ename);
		}
		error("unexpected reply type in fversion");
	}
	if(f->msize > msize)
		error("server tries to increase msize in fversion");
	if(f->msize<256 || f->msize>1024*1024)
		error("nonsense value of msize in fversion");
	k = strlen(f->version);
	if(strncmp(f->version, v, k) != 0){
		if(strcmp(v, VERSION9PIX) == 0 && strcmp(f->version, VERSION9P) == 0)
			f->version = VERSION9P;
		else
			error("bad 9P version returned from server");
	}

	/* now build Mnt associated with this connection */
	lock(&mntalloc);
	mnt = mntalloc.mntfree;
	if(mnt != nil)
		mntalloc.mntfree = mnt->list;
	else {
		mnt = malloc(sizeof(Mnt));
		if(mnt == nil) {
			unlock(&mntalloc);
			exhausted("mount devices");
		}
	}
	mnt->list = mntalloc.list;
	mntalloc.list = mnt;
	mnt->version = nil;
	kstrdup(&mnt->version, f->version);
	mnt->sharedtags = strcmp(mnt->version, VERSION9PIX) == 0;
	mnt->id = mntalloc.id++;
	/*
	 * We an arbitrary high limit, we don't want to limit
	 * the flow at which we send requests.
	 */
	mnt->q = qopen(0x7fffffff, 0, nil, nil);
	mnt->msize = f->msize;
	unlock(&mntalloc);

	if(returnlen != 0){
		if(returnlen < k)
			error(Eshort);
		memmove(version, f->version, k);
	}

	poperror();	/* r */
	mntfree(mntr);

	lock(mnt);
	mnt->queuehd = nil;
	mnt->queuetl = nil;
	mnt->rip = nil;

	c->flag |= CMSG;
	c->mux = mnt;
	mnt->c = c;
	unlock(mnt);

	poperror();	/* c */
	qunlock(&c->umqlock);

	return k;
}

Chan*
mntauth(Chan *c, char *spec)
{
	Mnt *mnt;
	Mntrpc *r;

	mnt = c->mux;

	if(mnt == nil){
		mntversion(c, MAXRPC, VERSION9PIX, 0);
		mnt = c->mux;
		if(mnt == nil)
			error(Enoversion);
	}

	c = mntchan();
	if(waserror()) {
		/* Close must not be called since it will
		 * call mnt recursively
		 */
		chanfree(c);
		nexterror();
	}

	r = mntralloc(nil, nil, mnt->msize, mnt->sharedtags);

	if(waserror()) {
		mntabort(r);
		nexterror();
	}

	r->request.type = Tauth;
	r->request.afid = c->fid;
	r->request.uname = up->user;
	r->request.aname = spec;
	mountrpcreq(mnt, r);
	mountrpcrep(r);

	c->qid = r->reply.aqid;
	c->mchan = mnt->c;
	incref(mnt->c);
	c->mqid = c->qid;
	c->mode = ORDWR;
	c->flag &= ~CCACHE;	/* safety */
	if(c->iounit == 0 || c->iounit > mnt->msize-IOHDRSZ)
		c->iounit = mnt->msize-IOHDRSZ;
	poperror();	/* r */
	mntfree(r);

	poperror();	/* c */

	return c;

}

static Chan*
mntattach(char *muxattach)
{
	Mnt *mnt;
	Chan *c;
	Mntrpc *r;
	struct bogus{
		Chan	*chan;
		Chan	*authchan;
		char	*spec;
		int	flags;
	}bogus;

	bogus = *((struct bogus *)muxattach);
	c = bogus.chan;

	mnt = c->mux;

	if(mnt == nil){
		mntversion(c, 0, nil, 0);
		mnt = c->mux;
		if(mnt == nil)
			error(Enoversion);
	}

	c = mntchan();
	if(waserror()) {
		/* Close must not be called since it will
		 * call mnt recursively
		 */
		chanfree(c);
		nexterror();
	}

	r = mntralloc(nil, nil, mnt->msize, mnt->sharedtags);

	if(waserror()) {
		mntabort(r);
		nexterror();
	}

	r->request.type = Tattach;
	r->request.fid = c->fid;
	if(bogus.authchan == nil)
		r->request.afid = NOFID;
	else
		r->request.afid = bogus.authchan->fid;
	r->request.uname = up->user;
	r->request.aname = bogus.spec;
	mountrpcreq(mnt, r);
	mountrpcrep(r);

	c->qid = r->reply.qid;
	c->mchan = mnt->c;
	incref(mnt->c);
	c->mqid = c->qid;
	pathclose(c->path);
	incref(mnt->c->path);
	c->path = mnt->c->path;
	poperror();	/* r */
	mntfree(r);

	poperror();	/* c */

	if(bogus.flags&MCACHE)
		c->flag |= CCACHE;
	return c;
}

Chan*
mntchan(void)
{
	Chan *c;

	c = devattach('M', 0);
	lock(&mntalloc);
	c->devno = mntalloc.id++;
	unlock(&mntalloc);

	if(c->mchan)
		panic("mntchan non-nil %#p", c->mchan);
	return c;
}

/* issue the call and link after prev, or free r */
static Mntrpc*
mntreqing(Mnt *mnt, Mntrpc *r, Mntrpc *prev)
{
	if(waserror()){
		mntfree(r);
		nexterror();
	}
	mountrpcreq(mnt, r);
	r->sendpc = getcallerpc(&mnt);
	poperror();
	if(prev != nil)
		prev->tagnext = r;
	return r;
}

Mntrpc*
mntwalking(Chan *c, char **name, int nname)
{
	Mntrpc *r;
	Mnt *mnt;
	Chan *nc;
	Walkqid *wq;

	/*
	 * BUG: We are not walking multiple times if
	 * this holds. The fix should be in clwalk.
	 */
	if(nname > MAXWELEM)
		error("devmnt: too many name elements");

	mnt = mntchk(c);
	r = mntralloc(nil, c, mnt->msize, mnt->sharedtags);
	wq = newwq(nname);
	r->wq = wq;
	nc = devclone(c);
	wq->clone = nc;
	/*
	 * Until the other side accepts this fid, we can't mntclose it.
	 * Therefore set type to 0 for now; rootclose is known to be safe.
	 * The process reading the reply, whichever it is, will move
	 * r->wq->clone->dev back to r->c->dev when Rwalk is received,
	 * so cclose() on the chan is correct at any time.
	 */
	nc->dev = nil;
	nc->flag |= c->flag&CCACHE;
	wq->nqid = nname;
	wq->clone->mchan = c->mchan;
	incref(c->mchan);

	r->request.type = Twalk;
	r->request.fid = c->fid;
	r->request.newfid = nc->fid;
	r->request.nwname = nname;
	memmove(r->request.wname, name, nname*sizeof(char*));
	return mntreqing(mnt, r, nil);
}

Mntrpc*
mntwalked(Mntrpc *r, Walkqid **wqp)
{
	Walkqid *wq;
	int i, nname;
	Chan *c;

	c = r->c;
	wq = r->wq;
	nname = wq->nqid;
	wq->nqid = 0;
	mountrpcrep(r);
	if(r->reply.nwqid > nname)
		error("too many QIDs returned by walk");
	if(r->reply.nwqid < nname){
		if(r->reply.nwqid == 0){
			/* nc won't be free until mntfree(r) */
			wq = nil;
			goto Return;
		}
	}
	/*
	 * BUG?
	 * mountmux sets the clone type, but it can happen
	 * that this is different from c->type. Why?
	 */
	wq->clone->dev = c->dev;
	wq->clone->devno = c->devno;
	if(r->reply.nwqid > 0)
		wq->clone->qid = r->reply.wqid[r->reply.nwqid-1];
	wq->nqid = r->reply.nwqid;
	for(i=0; i < wq->nqid; i++)
		wq->qid[i] = r->reply.wqid[i];

    Return:
	*wqp = wq;
	return r->tagnext;
}

static Walkqid*
mntwalk(Chan *c, Chan *nc, char **name, int nname)
{
	Mntrpc *r;
	Walkqid *wq;

	if(nc != nil)
		panic("mntwalk: not supported; since lib9p can, so do I");
	r = mntwalking(c, name, nname);
	if(waserror()){
		mntabort(r);
		nexterror();
	}
	mntwalked(r, &wq);
	poperror();
	if(wq != nil)
		r->wq = nil; /* don't free it on mntfree */
	mntfree(r);

	return wq;
}

Mntrpc*
mntstating(Mntrpc *prev, Chan *c)
{
	Mnt *mnt;
	Mntrpc *r;

	mnt = mntchk(c);
	r = mntralloc(prev, c, mnt->msize, mnt->sharedtags);
	r->request.type = Tstat;
	r->request.fid = c->fid;
	return mntreqing(mnt, r, prev);
}

Mntrpc*
mntstated(Mntrpc *r, uchar *dp, long *np)
{
	Chan *c;
	int n;

	n = *np;
	c = r->c;
	mountrpcrep(r);

	if(r->reply.nstat > n){
		n = BIT16SZ;
		PBIT16((uchar*)dp, r->reply.nstat-2);
	}else{
		n = r->reply.nstat;
		memmove(dp, r->reply.stat, n);
		validstat(dp, n);
		mntdirfix(dp, c);
	}
	*np = n;
	return r->tagnext;
}

static long
mntstat(Chan *c, uchar *dp, long n)
{
	Mntrpc *r;

	if(n < BIT16SZ)
		error(Eshortstat);
	r = mntstating(nil, c);
	if(waserror()){
		mntabort(r);
		nexterror();
	}
	mntstated(r, dp, &n);
	poperror();
	mntfree(r);
	return n;
}

Mntrpc*
mntopencreating(Mntrpc *prev, int type, Chan *c, char *name, int omode, ulong perm)
{
	Mnt *mnt;
	Mntrpc *r;

	mnt = mntchk(c);
	r = mntralloc(prev, c, mnt->msize, mnt->sharedtags);
	r->request.type = type;
	r->request.fid = c->fid;
	r->request.mode = omode;
	if(type == Tcreate){
		r->request.perm = perm;
		r->request.name = name;
	}
	return mntreqing(mnt, r, prev);
}

Mntrpc*
mntopencreated(Mntrpc *r, Chan **cp)
{
	Mnt *mnt;
	Chan *c;

	mountrpcrep(r);
	c = r->c;
	mnt = mntchk(c);
	c->qid = r->reply.qid;
	c->offset = 0;
	c->mode = openmode(r->request.mode);
	c->iounit = r->reply.iounit;
	if(c->iounit == 0 || c->iounit > mnt->msize-IOHDRSZ)
		c->iounit = mnt->msize-IOHDRSZ;
	c->flag |= COPEN;

	/*
	 * TODO: check out if it's a script. If it is,
	 * then sysexec is going to discard the file after it
	 * reads the #! line. Using the cache in this case is ok.
	 * If it's not a script, then clear the cache flag because
	 * the file must be cached by the segment cache.
	 */
	if(r->request.mode == OEXEC)
		c->flag &= ~CCACHE;

	copen(c);
	*cp = c;
	return r->tagnext;
}

static Chan*
mntopencreate(int type, Chan *c, char *name, int omode, ulong perm)
{
	Mntrpc *r;

	r = mntopencreating(nil, type, c, name, omode, perm);
	if(waserror()){
		mntabort(r);
		nexterror();
	}
	mntopencreated(r, &c);
	poperror();
	mntfree(r);
	return c;
}

static Chan*
mntopen(Chan *c, int omode)
{
	return mntopencreate(Topen, c, nil, omode, 0);
}

static void
mntcreate(Chan *c, char *name, int omode, int perm)
{
	if(omode&OTRUNC)
		panic("create called with OTRUNC");
	mntopencreate(Tcreate, c, name, omode, perm);
}

static Chan*
mntncreate(Chan *c, char *name, int omode, int perm)
{
	return mntopencreate(Tcreate, c, name, omode, perm);
}

Mntrpc*
mntclunking(Mntrpc *prev, Chan *c, int t)
{
	Mnt *mnt;
	Mntrpc *r;

	mnt = mntchk(c);
	r = mntralloc(prev, c, mnt->msize, mnt->sharedtags);
	r->request.type = t;
	r->request.fid = c->fid;
	if(t == Tremove)
		cremove(c);
	return mntreqing(mnt, r, prev);
}

Mntrpc*
mntclunked(Mntrpc *r)
{
	mountrpcrep(r);
	return r->tagnext;
}

static void
mntclunk(Chan *c, int t)
{
	Mntrpc *r;

	r = mntclunking(nil, c, t);
	if(waserror()){
		mntabort(r);
		nexterror();
	}
	mntclunked(r);
	poperror();
	mntfree(r);
}

void
muxclose(Mnt *mnt)
{
	Mntrpc *r;

	lock(mnt);
	while(mnt->queuehd != nil){
		r = mnt->queuehd;
		if(r->tagnext != nil)
			panic("muxclose: next");
		r->tagnext = nil;
		mntqrm(mnt, r, 1);
		mntfree(r);
	}
	unlock(mnt);
	mnt->id = 0;
	free(mnt->version);
	mnt->version = nil;
	mntpntfree(mnt);
}

void
mntpntfree(Mnt *mnt)
{
	Mnt *f, **l;
	Queue *q;

	lock(&mntalloc);
	l = &mntalloc.list;
	for(f = *l; f != nil; f = f->list) {
		if(f == mnt) {
			*l = mnt->list;
			break;
		}
		l = &f->list;
	}
	mnt->list = mntalloc.mntfree;
	mntalloc.mntfree = mnt;
	q = mnt->q;
	unlock(&mntalloc);

	qfree(q);
}

/* the name mntclose is now part of the std mount table interface */
static void
xmntclose(Chan *c)
{
	mntclunk(c, Tclunk);
}

static void
mntremove(Chan *c)
{
	mntclunk(c, Tremove);
}

Mntrpc*
mntwstating(Mntrpc *prev, Chan *c, uchar *dp, long n)
{
	Mnt *mnt;
	Mntrpc *r;

	mnt = mntchk(c);
	r = mntralloc(prev, c, mnt->msize, mnt->sharedtags);
	r->request.type = Twstat;
	r->request.fid = c->fid;
	r->request.nstat = n;
	r->request.stat = dp;
	return mntreqing(mnt, r, prev);
}

Mntrpc*
mntwstated(Mntrpc *r)
{
	mountrpcrep(r);
	return r->tagnext;
}

static long
mntwstat(Chan *c, uchar *dp, long n)
{
	Mntrpc *r;

	r = mntwstating(nil, c, dp, n);
	if(waserror()){
		mntabort(r);
		nexterror();
	}
	mntwstated(r);
	poperror();
	mntfree(r);
	return n;
}

Mntrpc*
mntrdwring(Mntrpc *prev, int type, Chan *c, void *buf, long n, vlong off)
{
	Mnt *mnt;
 	Mntrpc *r;

	if(n > c->iounit)
		n = c->iounit;
	mnt = mntchk(c);

	r = mntralloc(prev, c, mnt->msize, mnt->sharedtags);
	r->request.type = type;
	r->request.fid = c->fid;
	r->request.offset = off;
	r->request.data = buf;
	r->request.count = n;
	return mntreqing(mnt, r, prev);
}

Mntrpc*
mntrdwred(Mntrpc *r, long *np)
{
	long n;

	mountrpcrep(r);
	n = r->request.count;
	if(r->reply.count < n)
		n = r->reply.count;
	if(r->request.type == Tread && r->request.data != nil)
		r->b = bl2mem((uchar*)r->request.data, r->b, n);
	if(np != nil)
		*np = n;
	return r->tagnext;
}

static long
mntrdwr(int type, Chan *c, void *buf, long n, vlong off)
{
	Mntrpc *r;

	r = mntrdwring(nil, type, c, buf, n, off);
	if(waserror()){
		mntabort(r);
		nexterror();
	}
	mntrdwred(r, &n);
	poperror();
	mntfree(r);
	return n;
}

/*
 * This should issue a single request, but 9's devmnt
 * tries to do a nreadn and stops only when less data
 * than asked for has been returned. We must do the
 * same or fix acme, libauth, etc.
 */
static long
mntread(Chan *c, void *buf, long n, vlong off)
{
	long nr, nreq, tot;
	char *p;

	nr = cread(c, buf, n, off);
	if(nr >= 0)
		return nr;
	p = buf;
	tot = 0;
	do{
		nreq = n-tot;
		if(nreq > c->iounit)
			nreq = c->iounit;
		nr = mntrdwr(Tread, c, p+tot, nreq, off+tot);
		if(nr <= 0)
			break;
		tot += nr;
	}while(tot < n && nr == nreq && up->nnote == 0);
	return tot;
}

static long
mntwrite(Chan *c, void *buf, long n, vlong off)
{
	long tot, nw;
	uchar *p;

	tot = cwrite(c, buf, n, off);
	if(tot >= 0)
		return tot;
	p = buf;
	for(tot = 0; tot < n; tot += nw){
		nw = mntrdwr(Twrite, c, p+tot, n-tot, off+tot);
		if(nw != n-tot)
			break;
	}
	return tot;
}

void
mountrpcreq(Mnt *mnt, Mntrpc *r)
{
	int n;

	r->reply.tag = 0;
	r->reply.type = Tmax;	/* can't ever be a valid message type */
	lock(mnt);
	if(r->next != nil || r->prev != nil || mnt->queuehd == r)
		panic("mountrpcreq: double send rpc %#p pc=%#p sendpc=%#p ",
			r, getcallerpc(&mnt), r->sendpc);
	r->sendpc = getcallerpc(&mnt);
	DBG("send pid %d %G\n", up->pid, &r->request);
	r->pid = up->pid;
	r->m = mnt;
	if(mnt->queuetl == nil)
		mnt->queuehd = r;
	else {
		mnt->queuetl->next = r;
		r->prev = mnt->queuetl;
	}
	mnt->queuetl = r;
	unlock(mnt);

	/* Transmit a file system rpc */
	if(mnt->msize == 0)
		panic("msize");
	n = convS2M(&r->request, r->rpc, mnt->msize);
	if(n < 0)
		panic("bad message type in mountrpcreq");
	if(waserror()){
		DBG("send err tag %d\n", r->request.tag);
		mntqrm(mnt, r, 0);
		nexterror();
	}
	if(mnt->c->dev->write(mnt->c, r->rpc, n, 0) != n)
		error(Emountrpc);
	r->stime = fastticks(nil);
	r->reqlen = n;
	poperror();
}

void
mountrpcrep(Mntrpc *r)
{
	Path *sn, *cn;
	int t;
	Mnt *mnt;

	mnt = r->m;
	while(waserror()) {
		if(mnt->rip == up)
			mntgate(mnt);
		nexterror();
	}

	/* Gate readers onto the mount point one at a time */
	for(;;) {
		lock(mnt);
		if(mnt->rip == nil)
			break;
		unlock(mnt);
		sleep(&r->r, rpcattn, r);
		if(r->done)
			goto Done;
	}
	DBG("mountrpcrep: rip %d\n", up->pid);
	mnt->rip = up;
	USED(&mnt->rip);
	unlock(mnt);

	/*
	 * Callers assume upon errors or returns that the rpc
	 * is no longer in the queue; make it so.
	 */
	if(waserror()){
		if(r->done == 0)
			mntqrm(mnt, r, 0);
		nexterror();
	}
	while(r->done == 0) {
		if(mntrpcread(mnt, r) < 0)
			error(Emountrpc);
		mountmux(mnt, r);
	}
	poperror();
	mntgate(mnt);
Done:
	/* If there's another with the same tag, pass the tag ownership to it */
	if(r->tagnext != nil && r->owntag && r->tagnext->request.tag == r->request.tag){
		r->tagnext->owntag = 1;
		r->owntag = 0;
	}
	poperror();

	DBG("recv pid %d %G\n", up->pid, &r->reply);
	t = r->reply.type;
	switch(t) {
	case Rerror:
		/* too drastic: panic by now to spot bugs in 9P2000.ix */
		if(strstr(r->reply.ename, "fid") && strstr(r->reply.ename, "in use"))
			panic("mountrpc: %s", r->reply.ename);
		error(r->reply.ename);
	case Rflush:
		if(r->request.type != Tflush)
			error(Eintr);
		break;
	default:
		if(t == r->request.type+1)
			break;
		sn = mnt->c->path;
		cn = nil;
		if(r->c != nil)
			cn = r->c->path;
		print("mnt: proc %s %d: mismatch from %N %N "
			"rep %#p tag %d fid %d T%d R%d rp %d\n",
			up->text, up->pid, sn, cn, 
			r, r->request.tag, r->request.fid, r->request.type,
			r->reply.type, r->reply.tag);
		error(Emountrpc);
	}
}

static int
doread(Mnt *mnt, int len)
{
	Block *b;

	while(qlen(mnt->q) < len){
		b = mnt->c->dev->bread(mnt->c, mnt->msize, 0);
		if(b == nil)
			return -1;
		if(blocklen(b) == 0){
			freeblist(b);
			return -1;
		}
		qaddlist(mnt->q, b);
	}
	return 0;
}

int
mntrpcread(Mnt *mnt, Mntrpc *r)
{
	int i, t, len, hlen;
	Block *b, **l, *nb;

	r->reply.type = 0;
	r->reply.tag = 0;

	/* read at least length, type, and tag and pullup to a single block */
	if(doread(mnt, BIT32SZ+BIT8SZ+BIT16SZ) < 0)
		return -1;
	nb = pullupqueue(mnt->q, BIT32SZ+BIT8SZ+BIT16SZ);

	/* read in the rest of the message, avoid ridiculous (for now) message sizes */
	len = GBIT32(nb->rp);
	if(len > mnt->msize){
		DBG("mntrpcread discard\n");
		qdiscard(mnt->q, qlen(mnt->q));
		return -1;
	}
	if(doread(mnt, len) < 0){
		DBG("mntrpcread -1\n");
		return -1;
	}
	/* pullup the header (i.e. everything except data) */
	t = nb->rp[BIT32SZ];
	switch(t){
	case Rread:
		hlen = BIT32SZ+BIT8SZ+BIT16SZ+BIT32SZ;
		break;
	default:
		hlen = len;
		break;
	}
	nb = pullupqueue(mnt->q, hlen);

	if(convM2S(nb->rp, len, &r->reply) <= 0){
		/* bad message, dump it */
		print("mntrpcread: convM2S failed\n");
		qdiscard(mnt->q, len);
		return -1;
	}
	/* hang the data off of the fcall struct */
	freeblist(r->b);
	r->b = nil;
	l = &r->b;
	do {
		b = qremove(mnt->q);
		if(hlen > 0){
			b->rp += hlen;
			len -= hlen;
			hlen = 0;
		}
		i = BLEN(b);
		if(i <= len){
			len -= i;
			*l = b;
			l = &(b->next);
		} else {
			/* split block and put unused bit back */
			nb = allocb(i-len);
			memmove(nb->wp, b->rp+len, i-len);
			b->wp = b->rp+len;
			nb->wp += i-len;
			qputback(mnt->q, nb);
			*l = b;
			return 0;
		}
	}while(len > 0);

	return 0;
}

void
mntgate(Mnt *mnt)
{
	Mntrpc *q;

	lock(mnt);
	mnt->rip = nil;
	for(q = mnt->queuehd; q != nil; q = q->next)
		if(wakeup(&q->r))
			break;
	unlock(mnt);
}

void
mountmux(Mnt *mnt, Mntrpc *r)
{
	Mntrpc *q;

	DBG("mux pid %d %G\n",up->pid, &r->reply);
	lock(mnt);
	for(q = mnt->queuehd; q != nil; q = q->next)
		if(q->request.tag == r->reply.tag) {		/* reply to a message */
			if(q != r) {
				/*
				 * Completed someone else.
				 * Trade pointers to receive buffer.
				 */
				q->reply = r->reply;
				freeblist(q->b);
				q->b = r->b;
				r->b = nil;
			}
			mntqrm(mnt, q, 1);
			/*
			 * Later would be late, if we are flushing and the
			 * walking proc gets interrupted and killed before
			 * setting the clone type.
			 */
			if(q->request.type == Twalk && q->reply.type == Rwalk &&
			   q->reply.nwqid == q->request.nwname &&
			   q->wq != nil && q->wq->clone != nil){
				q->wq->clone->dev = q->c->dev;
				q->wq->clone->devno = q->c->devno;
			}
			/* Same reasoning, for clunk: move chan out of devmnt.
			 * Removing this is ok: you might get 'no such fid' while re-clunking.
			 */
			if(q->request.type == Tremove || q->request.type == Tclunk){
				q->c->dev = nil;
				q->c->devno = 0;
			}
			if(q != r)
				wakeup(&q->r);
			unlock(mnt);
			if(mntstats != nil)
				(*mntstats)(q->request.type,
					mnt->c, q->stime,
					q->reqlen + r->replen);
			return;
		}
	unlock(mnt);
	print("mux pid %d: unexpected reply tag %ud; type %d fid %ud\n",
		up->pid, r->reply.tag, r->reply.type, r->request.fid);
}

/*
 * Create a new flush request and chain the previous
 * requests from it
 */
Mntrpc*
mntflushalloc(Mntrpc *r, ulong iounit)
{
	Mntrpc *fr;

	fr = mntralloc(nil, nil, iounit, 0);

	fr->request.type = Tflush;
	if(r->request.type == Tflush)
		fr->request.oldtag = r->request.oldtag;
	else
		fr->request.oldtag = r->request.tag;
	fr->flushed = r;

	return fr;
}

int
alloctag(void)
{
	int i, j;
	ulong v;

	for(i = 0; i < NMASK; i++){
		v = mntalloc.tagmask[i];
		if(v == ~0UL)
			continue;
		for(j = 0; j < 1<<TAGSHIFT; j++)
			if((v & (1<<j)) == 0){
				mntalloc.tagmask[i] |= 1<<j;
				return (i<<TAGSHIFT) + j;
			}
	}
	panic("nemo improved the mount driver");
	return NOTAG;
}

void
freetag(int t)
{
	mntalloc.tagmask[t>>TAGSHIFT] &= ~(1<<(t&TAGMASK));
}

Mntrpc*
mntralloc(Mntrpc *prev, Chan *c, ulong msize, int sharetags)
{
	Mntrpc *new;

	lock(&mntalloc);
	new = mntalloc.rpcfreehd;
	if(new == nil){
		unlock(&mntalloc);
		new = malloc(sizeof(Mntrpc));
		if(new == nil)
			exhausted("mount rpc header");
		/*
		 * The header is split from the data buffer as
		 * mountmux may swap the buffer with another header.
		 */
		new->rpc = mallocz(MAX(msize, MAXRPC), 0);
		if(new->rpc == nil){
			free(new);
			exhausted("mount rpc buffer");
		}
		new->rpclen = MAX(msize, MAXRPC);
		new->b = nil;
		lock(&mntalloc);
	}else{
		mntalloc.rpcfreehd = new->next;
		if(mntalloc.rpcfreehd == nil)
			mntalloc.rpcfreetl = nil;
		new->next = nil;
		mntalloc.nrpcfree--;
		if(new->rpclen < msize){
			free(new->rpc);
			new->rpc = mallocz(MAX(msize, MAXRPC), 0);
			if(new->rpc == nil){
				free(new);
				unlock(&mntalloc);
				exhausted("mount rpc buffer");
			}
			new->rpclen = MAX(msize, MAXRPC);
		}
	}
	assert(new->b == nil);
	if(sharetags < 0)
		new->owntag = 0;
	else if(prev != nil && sharetags){
		if(new->owntag != 0 && new->request.tag != prev->request.tag)
			freetag(new->request.tag);
		new->request.tag = prev->request.tag;
		new->owntag = 0;
	}else if(new->owntag == 0){
		new->owntag = 1;
		new->request.tag = alloctag();
	}
	mntalloc.nrpcused++;
	unlock(&mntalloc);
	new->c = c;
	new->done = 0;
	new->flushed = nil;
	new->request.type = 0;
	new->reply.type = 0;
	if(new->tagnext != nil || new->wq != nil || new->next != nil || new->prev != nil)
		panic("mntralloc: tagnext %#p wq %#p next %#p prev %#p",
			new->tagnext, new->wq, new->next, new->prev);
	setmalloctag(new, getcallerpc(&prev));
	//DBG("mntalloc %#p ~ %d+%d\n", new, mntalloc.nrpcused, mntalloc.nrpcfree);
	return new;
}

void
mntfree(Mntrpc *r)
{
	Mntrpc *tn;

	if(r == nil)
		return;

	//DBG("mntfree %#p ~ %d+%d\n", r, mntalloc.nrpcused, mntalloc.nrpcfree);
	if(r->request.type == 69 && r->reply.type == 69)
		panic("mntfree: double free: pc %#p alloc pc %#p ",
			getcallerpc(&r), (uintptr)getmalloctag(r));
	if(r->next != nil || r->prev != nil)
		panic("mntfree: pid %d r %#p %G: next %#p prev %#p",
			up->pid, r, &r->request, r->next, r->prev);

	r->request.type = 69;		/* poison */
	r->reply.type = 69;		/* poison */
	if(r->wq != nil){
		wqfree(r->wq);
		r->wq = nil;
	}
	if(r->b != nil)
		freeblist(r->b);
	r->b = nil;
	while(r->tagnext != nil){
		tn = r->tagnext;
		r->tagnext = tn->tagnext;
		tn->tagnext = nil;
		mntfree(tn);
	}

	lock(&mntalloc);
	if(NRPCS != 0 && mntalloc.nrpcfree >= NRPCS){
		free(r->rpc);
		if(r->owntag != 0)
			freetag(r->request.tag);
		free(r);
	}else{
		if(mntalloc.rpcfreetl == nil)
			mntalloc.rpcfreehd = r;
		else
			mntalloc.rpcfreetl->next = r;
		mntalloc.rpcfreetl = r;
		mntalloc.nrpcfree++;
	}
	mntalloc.nrpcused--;
	unlock(&mntalloc);
}

/*
 * A series of rpcs starting at r0 raised an error.
 * If it's Eintr we must send a flush for all different tags
 * found in the series and then just release all the rpcs and flushes.
 * 
 * Otherwise, we must wait for any RPC not yet done, ignoring errors.
 * The usual case here is an Rerror from a request that makes us abort
 * a full series of requests.
 *
 * Either way, all rpcs are set free.
 * 
 */
int
mntabort(Mntrpc *r0)
{
	Mnt *mnt;
	Mntrpc *r, *fr;
	char ebuf[ERRMAX];
	int notdone, nr;

	nr = 0;
	if(r0 == nil)
		return 0;
	mnt = r0->m;
	if(m == nil)
		panic("mntabort");

	kstrcpy(ebuf, up->errstr, ERRMAX);

	if(strcmp(up->errstr, Eintr) != 0){
		for(r = r0; r != nil; r = r->tagnext){
			nr++;
			if(r->done || r->pid != up->pid)
				continue;
			if(!waserror()){
				DBG("mntabort: pid %d wait for %G\n",
						r->pid, &r->request);
				mountrpcrep(r);
				poperror();
			}
		}
		kstrcpy(up->errstr, ebuf, ERRMAX);
		mntfree(r0);
		return nr;
	}
	/* interrupted: send flushes for any alive tags we used */
	for(r = r0; r != nil; r = r->tagnext){
		nr++;
		if(r->pid != up->pid)
			DBG("mntabort: PIDS: up %d rpid %d\n", up->pid, r->pid);

		/* don't skip !r->owntag; if they share a tag they would
		 * respond to flushes with an error. But otherwise,
		 * we might skip a flush for a request that shared the tag
		 * of one being retired.
		 */
		if(r->done || r->pid != up->pid)
			continue;
		DBG("mntabort: pid %d flush %G\n", r->pid, &r->request);

		fr = mntflushalloc(r, mnt->msize);
		if(waserror()){
			DBG("mntabort: pid %d flusherr %s\n", up->pid, up->errstr);
			/* don't send more flushes; assume i/o error */
			if(!fr->done)
				mntqrm(mnt, fr, 0);
		}else{
			mountrpcreq(mnt, fr);
			mountrpcrep(fr);
			poperror();
		}
		mntfree(fr);
	}

	/* either flushed everything or failed: game over */
	for(r = r0; r != nil; r = r->tagnext){
		if(r->pid != up->pid)
			continue;
		lock(mnt);
		notdone = !r->done;
		if(notdone){
			r->reply.type = Rflush;
			mntqrm(mnt, r, 1);
		}
		unlock(mnt);
		if(notdone)
			DBG("mntabort: aborted req tag %ud type %d\n",
				r->request.tag, r->request.type);
	}
	kstrcpy(up->errstr, ebuf, ERRMAX);
	mntfree(r0);
	return nr;
}

void
mntqrm(Mnt *mnt, Mntrpc *r, int locked)
{
	if(!locked)
		lock(mnt);
	if(r->done)
		panic("mntqrm: pid %d rpc %#p already done", up->pid, r);
	assert(r->next == nil || r->next->prev == r);
	assert(r->prev == nil || r->prev->next == r);
	
	r->done = 1;
	if(r->prev != nil)
		r->prev->next = r->next;
	else
		mnt->queuehd = r->next;
	if(r->next != nil)
		r->next->prev = r->prev;
	else
		mnt->queuetl = r->prev;
	r->next = nil;
	r->prev = nil;
	if(!locked)
		unlock(mnt);
}

Mnt*
mntchk(Chan *c)
{
	Mnt *mnt;

	/* sanity checking */

	if(c->mchan == nil)
		panic("mntchk 1: nil mchan c %N\n", c->path);

	mnt = c->mchan->mux;

	if(mnt == nil)
		print("mntchk 2: nil mux c %N c->mchan %N\n", c->path, c->mchan->path);

	/*
	 * Was it closed and reused (was error(Eshutdown); now, it cannot happen)
	 */
	if(mnt->id == 0 || mnt->id >= c->devno)
		panic("mntchk 3: can't happen");

	return mnt;
}

/*
 * Rewrite channel type and dev for in-flight data to
 * reflect local values.  These entries are known to be
 * the first two in the Dir encoding after the count.
 */
void
mntdirfix(uchar *dirbuf, Chan *c)
{
	dirbuf += BIT16SZ;	/* skip count */
	PBIT16(dirbuf, 'M');
	dirbuf += BIT16SZ;
	PBIT32(dirbuf, c->devno);
}

int
rpcattn(void *v)
{
	Mntrpc *r;

	r = v;
	return r->done || r->m->rip == 0;
}

Dev mntdevtab = {
	'M',
	"mnt",

	mntreset,
	devinit,
	devshutdown,
	mntattach,
	mntwalk,
	mntstat,
	mntopen,
	mntcreate,
	xmntclose,
	mntread,
	devbread,
	mntwrite,
	devbwrite,
	mntremove,
	mntwstat,
	nil,		/* power */
	nil,		/* config */
	mntncreate,
};
