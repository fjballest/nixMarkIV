#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"

/*
 * See segment.c for locking rules.
 * This is a cache for files not used as program binaries.
 * The cache in segment.c keeps text files cached.
 *
 * TODO: reclaim cache entries when low on memory. Now keeps NBYTES
 * cached no matter how low on memory we are. When this is done,
 * we can increase NBYTES to a large size and the the cache eat
 * unused memory.
 *
 */

typedef struct Mntcache Mntcache;
typedef struct Frd Frd;

enum
{
	FHASHSIZE = 101,
	NBYTES = 64ull*MiB,	/* reclaim when cache has this size */
	NFILES = 100,		/* max nb. of files cached */
	NRPROCS = 8,		/* max nb. of read procs */

	CPGSHFT = UPGSHFT,	/* log2(page size used in cache segments) */
	CPGSZ = 1<<CPGSHFT,	/* page size used */
	NREAD = 2*CPGSZ,	/* read window size in bytes (32k) */
};

struct Frd
{
	Frd	*next;
	Chan	*c;
	Segment	*s;
	vlong	off;
};

struct Mntcache
{
	Lock;
	Segment	*hash[FHASHSIZE];
	Segq;
	uint	nseg;			/* used or cached files */
	uvlong	nbytes;
	QLock	reclaimlk;		/* one reclaimer at a time */
	int	nrcalls;		/* nb of seg reclaim calls */
	int	nreclaims;		/* nb of segs reclaimed */

	int	nprocs;
	Frd	*rfree;
	Frd	*rhead;
	Frd	*rtail;
	QLock	rlk;
	Rendez	rr;
};

#define chash(mc)	(&cache.hash[(mc)%FHASHSIZE])

/* Don't cache anything other than plain files, and
 * exclude exclusive open, append only, auth files and directories.
 */
#define	cacheable(c)	(((c)->flag&CCACHE) != 0 && (c)->qid.type == QTFILE)

extern int	mntabort(Mntrpc*);
extern void	pagedin(Page*);
extern Mntrpc*	mntrdwred(Mntrpc *r, long *np);
extern Mntrpc*	mntrdwring(Mntrpc *prev, int type, Chan *c, void *buf, long n, vlong off);
extern void	mntfree(Mntrpc *r);

static Mntcache cache;
static int nocache;

static char*
cachesummary(char *s, char *e, void*)
{
	return seprint(s, e, "%ulld/%ulld cache bytes\n"
		"%d/%d cache segs %d/%d reclaims %d procs\n",
		cache.nbytes, NBYTES, cache.nseg, NFILES,
		cache.nreclaims, cache.nrcalls, cache.nprocs);
}

/*
 * lookup the entry for c and return it or nil.
 * If mkit and there is no entry, create one.
 */
static Segment*
clookup(Chan *c, int mkit)
{
	Segment *s, **h;

	if(c->mc != nil)
		return c->mc;
	lock(&c->mclock);
	if(c->mc != nil){
		unlock(&c->mclock);
		return c->mc;
	}
	lock(&cache);
	h = chash(c->qid.path);
	for(s = *h; s != nil; s = s->hash)
		if(c->qid.path == s->cqid.path && c->qid.type == s->cqid.type &&
		   c->dev == s->cdev){
			usedseg(&cache, s);
			c->mc = s;
			incref(s);
			break;
		}
	if(s == nil && mkit){
		DBG("clookup new %N\n", c->path);
		s = newseg(SG_DATA|SG_CACHE, 0, SEGMAXSIZE, nil, CPGSHFT);
		c->mc = s;
		incref(s);			/* one for the hash, another for mc */
		s->cpath = c->path;
		if(s->cpath != nil)		/* for debug */
			incref(s->cpath);
		s->cqid = c->qid;
		s->cdev = c->dev;
		s->clength = -1;
		s->nbytes = 0;
		linkseg(&cache, s);
		s->hash = *h;
		*h = s;
		cache.nseg++;
	}
	unlock(&cache);
	unlock(&c->mclock);
	return s;
}

static Segment*
cseg(Chan *c)
{
	Segment *mc;

	mc = clookup(c, 0);
	if(mc == nil)
		panic("cseg: no mc. pc %#p", getcallerpc(&c));
	if(mc->c != nil && !eqchan(mc->c, c, 1))
		panic("cseg: wrong chan");
	qlock(&mc->lk);
	return mc;
}

static vlong
clen(Segment *mc, long len, vlong off)
{
	if(mc->clength < 0)
		return len;
	if(off >= mc->clength)
		return 0;
	if(off+len > mc->clength)
		return mc->clength - off;
	return len;
}

static void
cclear(Segment *s)
{
	clearseg(s);
	s->clength = -1;
	s->nbytes = 0;
}

static void
ccheckvers(Chan *c)
{
	Segment *s;

	s = c->mc;
	if(c->qid.vers != s->cqid.vers){
		qlock(&s->lk);
		if(c->qid.vers != s->cqid.vers)
			cclear(s);
		s->cqid.vers = c->qid.vers;
		qunlock(&s->lk);
	}
}

static vlong
ceof(Chan *c, vlong off)
{
	Segment *mc;

	if(!cacheable(c)){
		c->flag &= ~CCACHE;
		return -1;
	}
	mc = clookup(c, 0);
	if(mc == nil)
		return -1;
	DBG("ceof pid %d %N %#llx\n", up->pid, c->path, off);
	qlock(&mc->lk);
	if(off >= 0 && (mc->clength < 0 || mc->clength > off))
		mc->clength = off;
	off = mc->clength;
	qunlock(&mc->lk);
	return off;
}

static int
mcread(Chan *c, Segment *s, vlong off, int wait)
{
	uintptr addr, tot, count, ptot;
	vlong eof;
	usize pgsz, nr;
	long rlen, np;
	Mntrpc *r0, *r, *rr;
	Page **pg, *pgs[NREAD/CPGSZ], *new;
	int i, nrpcs[NREAD/CPGSZ];
	KMap *k;
	uchar *p;

	qlock(&s->lk);
	count = clen(s, NREAD, off);
	qunlock(&s->lk);
	if(count == 0)
		return 0;
	r = nil;
	r0 = nil;
	np = 0;
	if(waserror()){
		DBG("mcread pid %d abort %N %llx %s\n", up->pid, c->path, off, up->errstr);
		mntabort(r0);
		for(pg = pgs; pg < pgs+np; pg++)
			if(*pg != nil){
				kunmap(*pg);
				qunlock(*pg);
				putpage(*pg);
			}
		qlock(&s->lk);
		count = s->nbytes;
		cclear(s);
		qunlock(&s->lk);
		lock(&cache);
		cache.nbytes -= count;
		unlock(&cache);
		nexterror();
	}
	DBG("mcread pid %d %N %#llx\n", up->pid, c->path, off);
	pgsz = (1<<s->pgszlg2);
	qlock(&s->lk);
	for(tot = 0; tot < count; tot += pgsz){
		addr = ROUNDDN(off+tot, pgsz);
		pg = segwalk(s, addr, 1);
		new = *pg;
		if(new != nil){
			if(wait){
				incref(new);
				qunlock(&s->lk);
				if(waserror()){
					putpage(new);
					nexterror();
				}
				pagedin(new);
				poperror();
				putpage(new);
				qlock(&s->lk);
			}
			continue;
		}
		new = newpage(pgsz, s->color, 0, 0);
		*pg = new;
		s->nbytes += pgsz;
		nrpcs[np] = 0;
		pgs[np++] = new;	/* keep in pgs[] until we are done */
		incref(new);
		qlock(new);
		qunlock(&s->lk);
		lock(&cache);
		cache.nbytes += pgsz;
		unlock(&cache);
		k = kmap(*pg);
		p = (uchar*)VA(k);
		for(ptot = 0; ptot < pgsz; ptot += nr){
			USED(&r0);
			USED(&np);
			nrpcs[np-1]++;
			r = mntrdwring(r, Tread, c, p+ptot, pgsz-ptot, addr+ptot);
			nr = r->request.count;
			if(r0 == nil)
				r0 = r;
		}
		qlock(&s->lk);
	}
	qunlock(&s->lk);
	eof = -1;
	r = r0;
	for(i = 0; i < np; i++){
		pg = &pgs[i];
		while(nrpcs[i]-- > 0){
			if(r == nil)
				panic("mcread: no r");
			rr = mntrdwred(r, &rlen);
			if(rlen < r0->request.count
			&& (eof < 0 || eof > r->request.offset+rlen))
				eof = r->request.offset+rlen;
			r = rr;
		}
		if(*pg != nil){
			(*pg)->n = 1;
			kunmap(*pg);
			qunlock(*pg);
			putpage(*pg);
			*pg = nil;
		}
	}
	if(r != nil)
		panic("mcread: r");
	poperror();
	mntfree(r0);
	if(eof >= 0)
		ceof(c, eof);
	DBG("mcread pid %d %N %#llx done\n", up->pid, c->path, off);
	return 1;
}

static Frd*
crunlink(void)
{
	Frd *fr;

	lock(&cache);
	fr = cache.rhead;
	cache.rhead = fr->next;
	fr->next = nil;
	if(cache.rhead == nil)
		cache.rtail = nil;
	unlock(&cache);
	return fr;
}

static void
crlink(Frd *fr)
{
	lock(&cache);
	if(cache.rtail == nil)
		cache.rhead = fr;
	else
		cache.rtail->next = fr;
	cache.rtail = fr;
	unlock(&cache);
}

static int
mustread(void*)
{
	return cache.rhead != nil;
}

static void
creadproc(void*)
{
	Frd *fr;
	int n;

	for(;;){
		qlock(&cache.rlk);
		DBG("creadproc pid %d idle\n", up->pid);
		while(cache.rhead == nil){
			if(!waserror()){
				tsleep(&cache.rr, mustread, nil, 5000);
				poperror();
			}
			if(cache.rhead == nil){
				qunlock(&cache.rlk);
				lock(&cache);
				n = cache.nprocs;
				if(n > 2)
					cache.nprocs--;
				unlock(&cache);
				if(n > 2)
					pexit("no work", 1);
				qlock(&cache.rlk);
			}
		}
		fr = crunlink();
		qunlock(&cache.rlk);

		DBG("creadproc pid %d %N\n", up->pid, fr->c->path);
		if(!waserror()){
			mcread(fr->c, fr->s, fr->off, 0);
			poperror();
		}
		if(!waserror()){
			cclose(fr->c);
			poperror();
		}
		fr->c = nil;
		putseg(fr->s);
		fr->s = nil;
		lock(&cache);
		fr->next = cache.rfree;
		cache.rfree = fr;
		unlock(&cache);
	}
}

void
creadq(Chan *c, vlong off)
{
	Frd *fr;
	int n;

	lock(&cache);
	fr = cache.rfree;
	if(fr != nil){
		cache.rfree = fr->next;
		fr->next = nil;
	}
	unlock(&cache);
	if(fr == nil)
		fr = smalloc(sizeof *fr);
	fr->c = c;
	incref(c);
	fr->s = c->mc;
	incref(fr->s);
	fr->off = off;
	crlink(fr);

	if(wakeup(&cache.rr))
		return;

	lock(&cache);
	n = cache.nprocs;
	if(n >= NRPROCS){
		unlock(&cache);
		return;
	}
	cache.nprocs++;
	unlock(&cache);
	kproc("creadproc", creadproc, nil);
}

void
cinit(void)
{
	char *s;

	addsummary(cachesummary, nil);
	if((s=getconf("*nocache")) != nil)
		nocache = atoi(s);
}

static void
unhashseg(Segment *s)
{
	Segment **l;

	for(l = chash(s->cqid.path); *l != nil; l = &(*l)->hash)
		if(*l == s)
			break;
	if(*l == nil)
		panic("unhashseg: not found");
	*l = s->hash;
	s->hash = nil;
	cache.nseg--;
	cache.nbytes -= s->nbytes;
}

static int
creclaim(void)
{
	Segment *s;

	cache.nrcalls++;
	/* Somebody is already segreclaiming */
	if(!canqlock(&cache.reclaimlk))
		return -1;

	iprint("creclaim\n");
	lock(&cache);
	s = segvictim(&cache);
	if(s == nil){
		unlock(&cache);
		qunlock(&cache.reclaimlk);
		print("cache: nothing to reclaim\n");
		return -1;
	}
	cache.nreclaims++;
	unhashseg(s);
	unlock(&cache);
	if(s->ref != 1)
		panic("creclaim: ref");
	DBG("creclaim pid %d %N\n", up->pid, s->cpath);
	putseg(s);
	qunlock(&cache.reclaimlk);
	return 0;
}

void
copen(Chan *c)
{
	if(!cacheable(c) || nocache){
		c->flag &= ~CCACHE;
		return;
	}
	if(cache.nseg > NFILES)
		creclaim();
	while(cache.nbytes >= NBYTES && creclaim()==0)
		;

	DBG("copen %N\n", c->path);
	clookup(c, 1);
	ccheckvers(c);
}

void
cremove(Chan *c)
{
	Segment *mc;
	usize count;

	if(!cacheable(c)){
		c->flag &= ~CCACHE;
		return;
	}
	DBG("cremove %N\n", c->path);
	mc = clookup(c, 0);
	if(mc != nil){
		qlock(&mc->lk);
		count = mc->nbytes;
		cclear(mc);
		qunlock(&mc->lk);
		lock(&cache);
		cache.nbytes -= count;
		if(mc->ref == 2){
			/* unused: 1 for lru; 1 for c */
			unlinkseg(&cache, mc);
			unhashseg(mc);
			if(decref(mc) != 1)
				panic("cremove ref");
		}
		unlock(&cache);
	}
}

long
cread(Chan *c, uchar *buf, long len, vlong off)
{
	Segment *mc;
	uintptr addr;
	int o, nr;
	Page **pp, *pg;
	uchar *p;
	usize pgsz, tot;
	KMap *k;

	if(!cacheable(c)){
		c->flag &= ~CCACHE;
		return -1;
	}
	DBG("cread pid %d %N %#lx %#llx\n", up->pid, c->path, len, off);
again:
	mc = cseg(c);
	if(mc->cpath == nil && c->path != nil){
		mc->cpath = c->path;
		incref(mc->cpath);
	}
	pgsz = (1<<mc->pgszlg2);
	len = clen(mc, len, off);
	for(tot = 0; tot < len; tot += nr){
		addr = ROUNDDN(off+tot, pgsz);
		pp = segwalk(mc, off+tot, 1);
		pg = *pp;
		if(pg == nil){
			qunlock(&mc->lk);
			if(tot > 0){
				/* got some; done but read more ahead */
				DBG("cread pid %d %N %#lx %#llx -> %#lx\n",
					up->pid, c->path, len, off, tot);
				creadq(c, addr);
				return tot;
			}
			/* nothing: read it now and try again */
			mcread(c, mc, addr, 1);
			goto again;
		}
		incref(pg);
		if(waserror()){
			putpage(pg);
			nexterror();
		}
		if(pg->n == 0){
			qunlock(&mc->lk);
			pagedin(pg);
			qlock(&mc->lk);
		}
		o = off+tot - addr;
		k = kmap(pg);
		p = UINT2PTR(VA(k));
		nr = pgsz - o;
		if(tot+nr > len)
			nr = len-tot;
		memmove(buf+tot, p+o, nr);
		kunmap(k);
		putpage(pg);
		poperror();
	}
	qunlock(&mc->lk);
	DBG("cread pid %d %N %#llx -> %#lx\n", up->pid, c->path, off, tot);
	return tot;
}

long
cwrite(Chan *c, uchar *buf, long len, vlong off)
{
	Segment *mc;
	long tot, nw;
	Mntrpc *r, *r0;
	usize count;

	if(!cacheable(c)){
		c->flag &= ~CCACHE;
		return -1;
	}
	DBG("cwrite pid %d %N %#lx %#llx\n", up->pid, c->path, len, off);

	mc = cseg(c);
	/*
	 * TODO: could invalidate only the range written, or
	 * update the cache like the old cache code did.
	 */
	count = mc->nbytes;
	cclear(mc);
	qunlock(&mc->lk);
	lock(&cache);
	cache.nbytes -= count;
	unlock(&cache);
	r0 = nil;
	r = nil;
	if(waserror()){
		mntabort(r0);
		nexterror();
	}
	for(tot = 0; tot < len; tot += nw){
		USED(&r0);
		r = mntrdwring(r, Twrite, c, buf+tot, len-tot, off+tot);
		nw = r->request.count;
		if(r0 == nil)
			r0 = r;
	}
	USED(&r0);
	r = r0;
	for(tot = 0; r != nil; tot += nw)
		r = mntrdwred(r, &nw);
	poperror();
	mntfree(r0);
	return tot;
}

void
cflushed(Chan*)
{
}
