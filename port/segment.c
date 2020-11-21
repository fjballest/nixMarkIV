#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

/*
 * Locking: Segment.lk is hold while operating on the ptes, unless
 * it's known we can do it safely.
 * Pages linked from the segment may be still on its way from the disk
 * if Page.daddr is 0, only those with Page.daddr set to 1 are known
 * to be ok.
 * The Page.QLock is hold while the page is being paged in.
 * The Lock in the Ref is used only for the ref.
 * There may be processes not holding the segment lock but holding page
 * locks, they must be preserved by the segment (or the page) Refs.
 *
 * Caching: this is responsible for the text file cache for executing
 * files. cache.c has a similar machinery for caching all other files.
 *
 * Segments are never set free, they are kept linked until reused.
 * Ptes are kept in the segments and never free.
 * Stacks keep also their pages.
 */

#define _PTEMAPMEM_	(PTEPERTAB*PGSZ)
#define SEGMAPSIZE	HOWMANY(SEGMAXSIZE,_PTEMAPMEM_)

typedef struct Segcache Segcache;
typedef struct Segalloc Segalloc;

enum
{
	SHASHSIZE = 101,
	SCACHESIZE = 100,
	SMAPINCR = 16,
};

struct Segcache
{
	Lock;
	Segment	*hash[SHASHSIZE];
	Segq;				/* lru queue of cached segs */
	uint	nseg;			/* used or cached segs */

	QLock	reclaimlk;		/* one reclaimer at a time */
	int	nrcalls;		/* nb of seg reclaim calls */
	int	nreclaims;		/* nb of segs reclaimed */

};

struct Segalloc
{
	Lock;
	Segment *segs;		/* all segments */
	Segment *free;		/* free segments (but for stacks) */
	Segment *sfree;		/* free stack segments */
	int	nused;
	int	nfree;		/* in free and sfree */
};

static Segcache segcache;
static Segalloc segalloc;

char *segtypename[] = 
{
	[SG_TEXT]	"Text",
	[SG_DATA]	"Data",
	[SG_STACK]	"Stack",
	[SG_SHARED]	"Shared",
	[SG_PHYSICAL]	"Phys",
	[SG_FREE]	"free",
};


#define shash(s)	(&segcache.hash[(s)%SHASHSIZE])

static char*
segsummary(char *s, char *e, void*)
{
	s = seprint(s, e, "%d/%d segs\n",
		segalloc.nused, segalloc.nused+segalloc.nfree);
	return seprint(s, e, "%d/%d text segs %d/%d reclaims\n",
		segcache.nseg, SCACHESIZE, segcache.nreclaims, segcache.nrcalls);
}

static void
dumpseg(Segment *s, int verb)
{
	Pte **p;
	Page **pg;
	int printed;

	print("seg %#p ref %d %s addr %#p top %#p c %N lpc %#p\n",
		s, s->ref, segtypename[s->type&SG_TYPE], s->base, s->top,
		s->cpath, s->lk.locked?s->lk.qpc:0);
	if(!verb || s->map == nil)
		return;
	for(p = s->map; p < &s->map[s->mapsize]; p++){
		printed = 0;
		if(*p == nil)
			continue;
		for(pg = (*p)->first; pg <= (*p)->last; pg++){
			if(*pg == nil)
				continue;
			if(printed++ == 0)
				if(p < s->first)
					print("\tmap[%#04lx<FIRST]\n", p - s->map);
				else if(p > s->last)
					print("\tmap[%#04lx>LAST]\n", p - s->map);
				else
					print("\tmap[%#04lx]\n", p - s->map);
			print("\t\tpte %#04lx\t%#P -> %#P ref %d\n",
				pg - (*p)->first, (*pg)->va, (*pg)->pa, (*pg)->ref);
		}
	}
}

static void
segdump(void*)
{
	Segment *s;

	print("segments:\n");
	for(s = segalloc.segs; s != nil; s = s->anext)
		dumpseg(s, 1);
	print("lru segments:\n");
	for(s = segcache.hd; s != nil; s = s->lnext)
		dumpseg(s, 0);
}

void
initseg(void)
{
	addsummary(segsummary, nil);
	addttescape('P', segdump, nil);
}

Segment*
unlinkseg(Segq *segq, Segment *s)
{
	if(s->lprev != nil)
		s->lprev->lnext = s->lnext;
	else
		segq->hd = s->lnext;
	if(s->lnext != nil)
		s->lnext->lprev = s->lprev;
	else
		segq->tl = s->lprev;
	s->lnext = nil;
	s->lprev = nil;
	return s;
}

void
linkseg(Segq *segq, Segment *s)
{
	if(s->lnext != nil || s->lprev != nil)
		panic("linkseg");
	s->lprev = segq->tl;
	if(s->lprev != nil)
		s->lprev->lnext = s;
	else
		segq->hd = s;
	segq->tl = s;
}

void
usedseg(Segq *segq, Segment *s)
{
	unlinkseg(segq, s);
	linkseg(segq, s);
}

Page**
segwalk(Segment *s, uintptr addr, int alloc)
{
	Pte **p, *etp;
	Page **pg;
	uintptr soff, pgsz;
	int idx;

	if(canqlock(&s->lk))
		panic("segwalk: lock pc %#p", getcallerpc(&s));
	pgsz = 1<<s->pgszlg2;
	addr &= ~(pgsz-1);
	soff = addr - s->base;
	idx = soff/s->ptemapmem;
	if(idx >= s->mapsize)
		panic("segwalk: addr %#p s %#p %#p idx %d npte %d",
			addr, s->base, s->top,
			idx, s->mapsize);
	p = &s->map[idx];
	if(*p == nil)
		if(alloc)
			*p = ptealloc();
		else
			return nil;
	if(p < s->first)
		s->first = p;
	if(p > s->last)
		s->last = p;
	etp = *p;
	idx = ((soff&(s->ptemapmem-1))>>s->pgszlg2);
	if(idx >= PTEPERTAB)
		panic("segwalk: idx %d ptepertab %d", idx, PTEPERTAB);
	pg = &etp->pages[idx];
	if(pg < etp->first)
		etp->first = pg;
	if(pg > etp->last)
		etp->last = pg;
	return pg;
}

Page*
lookpage(Segment *s, uintptr addr)
{
	Page **p, *pg;

	DBG("lookpage\n");
	p = segwalk(s, addr, 0);
	if(p == nil)
		return nil;
	pg = *p;
	if(pg != nil)
		incref(pg);
	return pg;
}

Segment*
segvictim(Segq *segq)
{
	Segment *s, *s0;
	int loops;

	s0 = segq->hd;
	if(s0 == nil)
		return nil;
	loops = 0;
	do{
		s = unlinkseg(segq, segq->hd);
		if(s->used)		/* second chance */
			s->used = 0;
		else if(s->ref == 1)	/* cached and unused */
			return s;
		linkseg(segq, s);
		s = segq->hd;
	}while(s != nil && (s != s0 || loops++ == 0));
	return nil;
}

int
segreclaim(void)
{
	Segment *s, **l;

	segcache.nrcalls++;
	/* Somebody is already segreclaiming */
	if(!canqlock(&segcache.reclaimlk))
		return -1;

	iprint("segreclaim\n");
	lock(&segcache);
	s = segvictim(&segcache);
	if(s == nil){
		unlock(&segcache);
		qunlock(&segcache.reclaimlk);
		print("segreclaim: no unused segs\n");
		return -1;
	}
	segcache.nreclaims++;
	for(l = shash(s->c->qid.path); *l != nil; l = &(*l)->hash)
		if(*l == s)
			break;
	if(*l == nil)
		panic("segreclaim: not found");
	*l = s->hash;
	s->hash = nil;
	segcache.nseg--;
	unlock(&segcache);
	if(s->ref != 1)
		panic("segreclaim: ref");
	putseg(s);
	qunlock(&segcache.reclaimlk);
	return 0;
}

static Segment*
attachseg(int type, uintptr base, uintptr top, Chan *c, uint pgszlg2)
{
	Segment *s, **h, *ns;

	DBG("attachseg pid %d %N\n", up?up->pid:0, c?c->path:nil);
	while(segcache.nseg >= SCACHESIZE)
		if(segreclaim()<0)
			break;

	/*
	 * Use a cached segment for c if found, or
	 * create a new one if not cached.
	 * Because newseg might sleep, we release the lock
	 * and scan the cache again in case another process
	 * created and cached the same thing in the meanwhile.
	 */
	ns = nil;
Again:
	lock(&segcache);
	h = shash(c->qid.path);
	for(s = *h; s != nil; s = s->hash)
		if(eqchan(c, s->c, 1)){
			usedseg(&segcache, s);
			unlock(&segcache);
			if(ns != nil)
				putseg(ns);
			if(base < s->base || top > s->top)
				panic("attachseg: bad base/top");
			incref(s);
			s->used = 1;
			return s;
		}

	if(ns == nil){
		unlock(&segcache);
		ns = newseg(type|SG_CACHE, base, top, nil, pgszlg2);
		incref(c);
		ns->c = c;
		ns->cpath = c->path;
		if(ns->cpath)
			incref(ns->cpath);
		goto Again;
	}
	linkseg(&segcache, ns);
	ns->hash = *h;
	*h = ns;
	segcache.nseg++;
	incref(ns);
	unlock(&segcache);
	DBG("attachseg new %#p %#p\n", ns->base, ns->top);
	return ns;
}

void
prefaultseg(Segment *s)
{
	int type, justlast;
	Pte **p;
	Page **pg;
	uint mmuflags;

	type = s->type&SG_TYPE;
	justlast = 0;
	switch(type){
	case SG_TEXT:
		mmuflags = PTERONLY|PTEVALID;
		break;
	case SG_STACK:
		justlast = (s->top == TSTKTOP);
		mmuflags = PTEWRITE|PTEVALID;
		break;
	default:
		return;
	}
	qlock(&s->lk);
	p = s->first;
	if(justlast && p <= s->last)
		p = s->last;
	for(; p <= s->last; p++){
		if(*p == nil)
			continue;
		pg = (*p)->first;
		if(justlast && pg <= (*p)->last)
			pg = (*p)->last;
		for(; pg <= (*p)->last; pg++){
			if(*pg == nil || (*pg)->n == 0)
				continue;
			mmuput(up, *pg, mmuflags);
		}
	}
	qunlock(&s->lk);
}

Pte*
ptealloc(void)
{
	Pte *new;

	new = smalloc(sizeof(Pte));
	new->first = &new->pages[PTEPERTAB];
	new->last = new->pages;
	return new;
}

static void
clearpte(Segment *s, Pte *p)
{
	void (*put)(Page*);
	Page **pg;
	usize pgsz;

	put = putpage;
	switch(s->type&SG_TYPE) {
	case SG_STACK:
		/*
		 * BUG: We love security.
		 * Only the tos page is zeroed, all other pages
		 * reused are not, which means they could carry
		 * data from other processes.
		 * If you want to zero them out, you should limit
		 * how many pages are kept by recycled stack segments,
		 * or the time to clear all the pages will be more than
		 * the time saved by recycling those pages.
		 */
		pgsz = 1<<s->pgszlg2;
		if(p->first <= p->last){
			pg = p->last;
			memset(UINT2PTR((*pg)->va), 0, pgsz);
		}
		break;
	case SG_PHYSICAL:
		if(s->pseg->pgfree != nil)
			put = s->pseg->pgfree;
		p->first = p->pages;			/* put all seg */
		p->last = &p->pages[PTEPERTAB-1];
		/* fall */

	default:
		for(pg = p->first; pg <= p->last; pg++)
			if(*pg != nil){
				put(*pg);
				*pg = nil;
			}
		p->first = &p->pages[PTEPERTAB];
		p->last = p->pages;
		break;
	}
}

void
segmapsize(Segment *s, usize mapsize)
{
	Pte **nmap;

	if(mapsize < SMAPINCR)
		mapsize = SMAPINCR;
	if(mapsize > s->mapsize){
		mapsize = ROUNDUP(mapsize, SMAPINCR);
		nmap = smalloc(mapsize*sizeof(Pte*));
		memmove(nmap, s->map, s->mapsize*sizeof(Pte*));
		s->first = nmap + (s->first - s->map);
		s->last = nmap + (s->last - s->map);
		free(s->map);
		s->map = nmap;
		s->mapsize = mapsize;
	}
}

Segment *
newseg(int type, uintptr base, uintptr top, Chan *c, uint pgszlg2)
{
	Segment *s, *ss;
	int mapsize;
	usize size, pgsz;

	if(c != nil)
		return attachseg(type, base, top, c, pgszlg2);

	pgsz = 1<<pgszlg2;
	if((base|top) & (pgsz-1))
		panic("newseg base %#p top %#p pgsz %#ulx", base, top , pgsz);

	if(top-base > SEGMAXSIZE)
		error(Enovmem);
	size = (top-base)/pgsz;

	lock(&segalloc);
	s = nil;
	ss = nil;
	if((type&SG_TYPE) == SG_STACK){
		s = segalloc.sfree;
		if(s != nil){
			segalloc.sfree = s->fnext;
			ss = s;
		}
	}
	if(s == nil){
		s = segalloc.free;
		if(s != nil)
			segalloc.free = s->fnext;
	}
	if(s != nil){
		s->fnext = nil;
		segalloc.nfree--;
	}
	segalloc.nused++;
	unlock(&segalloc);

	if(s == nil){
		s = smalloc(sizeof(Segment));
		lock(&segalloc);
		s->anext = segalloc.segs;
		segalloc.segs = s;
		unlock(&segalloc);
	}
	if(ss != nil)
		relocateseg(ss, base, ss->base);
	s->ref = 1;
	s->type = type;
	s->base = base;
	s->top = top;
	s->size = size;
	s->fstart = 0;
	s->flen = 0;
	s->pgszlg2 = pgszlg2;
	s->flushme = 0;
	s->pseg = nil;
	s->ptemapmem = PTEPERTAB<<s->pgszlg2;
	s->color = -1;
	s->sema.addr = nil;
	s->sema.waiting = 0;
	s->sema.prev = &s->sema;
	s->sema.next = &s->sema;
	s->used = 0;
	/* these are used by cache.c */
	s->cdev = nil;
	s->clength = -1;
	s->nbytes = 0;

	if(type&SG_CACHE)
		mapsize = SEGMAPSIZE;
	else
		mapsize = HOWMANY(size, PTEPERTAB);
	segmapsize(s, mapsize);
	if(s->first == nil){
		s->first = &s->map[s->mapsize];
		s->last = s->map;
	}
	DBG("newseg t %d %#p %#p sz %uld npte %d (%d)\n",
		type, base, top, s->size, s->mapsize, mapsize);

	return s;
}

void
clearseg(Segment *s)
{
	Pte **pp;

	for(pp = s->first; pp <= s->last; pp++)
		if(*pp != nil)
			clearpte(s, *pp);
	if((s->type&SG_TYPE) != SG_STACK){
		s->first = &s->map[s->mapsize];
		s->last = s->map;
	}
	s->used = 0;
}

void
putseg(Segment *s)
{
	if(s == nil || decref(s) > 0)
		return;
	DBG("putseg %#p\n", s->base);
	if(s->lnext != nil || s->lprev != nil || s->hash != nil || s->fnext != nil)
		panic("putseg: linked");
	if(s->src != nil){
		putseg(s->src);
		s->src = nil;
	}
	if(s->c != nil){
		ccloseq(s->c);
		s->c = nil;
	}
	if(s->profile != nil)
		free(s->profile);
	s->profile = nil;
	if(s->cpath != nil){
		pathclose(s->cpath);
		s->cpath = nil;
	}
	clearseg(s);
	lock(&segalloc);
	if((s->type&SG_TYPE) == SG_STACK){
		s->fnext = segalloc.sfree;
		segalloc.sfree = s;
	}else{
		s->fnext = segalloc.free;
		segalloc.free = s;
	}
	s->type = SG_FREE;	/* poison */
	segalloc.nfree++;
	segalloc.nused--;
	unlock(&segalloc);
}

void
relocateseg(Segment *s, uintptr new, uintptr old)
{
	Page **pg, *x;
	Pte *pte, **p;
	uintptr va;

	if(s->map == nil || new == old)
		return;
	for(p = s->first; p <= s->last; p++){
		if(*p == nil)
			continue;
		pte = *p;
		for(pg = pte->first; pg <= pte->last; pg++)
			if((x = *pg) != nil){
				va = x->va - old + new;
				DBG("reloc %#p %#p -> %#p %#p\n", old, x->va, new, va);
				x->va = va;
			}
	}
}

/*
 * If it's not a deep copy, the mmu entries for the pte are flushed.
 * If it's a deep copy, the mmu for the new process is updated to include
 * the entries for the copied pages.
 */
static void
ptecpy(Proc *p, Segment *s, int i, Pte *old, int deep, uint flags)
{
	Pte *new;
	Page **src, **dst, *pg;

	if(s->map[i] == nil)
		s->map[i] = ptealloc();
	new = s->map[i];
	dst = &new->pages[old->first-old->pages];
	if(new->first > dst)
		new->first = dst;
	for(src = old->first; src <= old->last; src++, dst++){
		pg = *src;
		if(pg == nil)
			continue;
		if(deep){
			if(*dst == nil)
				*dst = newpage(1<<pg->pgszlg2, s->color, 0, pg->va);
			pagecpy(*dst, pg);
			mmuput(p, *dst, flags);
		}else{
			mmuflushpg(pg);
			if(*dst != nil)
				putpage(*dst);
			incref(pg);
			*dst = pg;
		}
		if(new->last < dst)
			new->last = dst;
	}
}

void
forkseg(Proc *p, int segno, int share)
{
	Pte **pp;
	Segment *n, *s;
	int deep;
	uint flags;

	SET(n);
	s = up->seg[segno];

	DBG("forkseg pid %d s %N sref %d %s %#p\n",
		up->pid, s->cpath, s->ref, segtypename[s->type&SG_TYPE], s->base);
	qlock(&s->lk);
	if(waserror()){
		qunlock(&s->lk);
		nexterror();
	}
	deep = 0;
	flags = 0;
	switch(s->type&SG_TYPE) {
	case SG_TEXT:		/* New segment shares pte set */
	case SG_SHARED:
	case SG_PHYSICAL:
		goto sameseg;

	case SG_STACK:
		/* stack pages are be copied; it's likely we will fault on them
		 * and rfork() may depend on the stack being copied.
		 */
		deep = 1;
		flags = PTEWRITE|PTEVALID;
		n = newseg(s->type, s->base, s->top, nil, s->pgszlg2);
		break;

	case SG_DATA:		/* Copy on ref,  demand load, zero fill on demand*/
		if(segno == TSEG){
			poperror();
			qunlock(&s->lk);
			p->seg[segno] = data2txt(s);
			return;
		}

		if(share)
			goto sameseg;
		n = newseg(s->type, s->base, s->top, nil, s->pgszlg2);
		n->src = s->src;
		if(n->src == nil)
			n->src = s;
		incref(n->src);
		n->fstart = s->fstart;
		n->flen = s->flen;
		break;
	}
	n->color = s->color;
	for(pp = s->first; pp <= s->last; pp++)
		if(*pp != nil)
			 ptecpy(p, n, pp - s->map, *pp, deep, flags);
	if(n->first > n->map + (s->first - s->map))
		n->first = n->map + (s->first - s->map);
	if(n->last < n->map + (s->last - s->map))
		n->last = n->map + (s->last - s->map);
	n->flushme = s->flushme;
	n->cpath = s->cpath;
	if(n->cpath)
		incref(n->cpath);
	if(s->ref > 1){
		DBG("forkseg flush pid %d s %N sref %d %s %#p\n",
			up->pid, s->cpath, s->ref, segtypename[s->type&SG_TYPE], s->base);
		procflushseg(s);
	}
	poperror();
	qunlock(&s->lk);
	p->seg[segno] = n;
	return;

sameseg:
	incref(s);
	poperror();
	qunlock(&s->lk);
	p->seg[segno] = s;
}

void
segpage(Segment *s, Page *p)
{
	Pte **pte;
	uintptr soff;
	Page **pg;

	if(p->va < s->base || p->va >= s->top)
		panic("segpage");

	soff = p->va - s->base;
	pte = &s->map[soff/s->ptemapmem];
	if(*pte == nil)
		*pte = ptealloc();
	if(pte < s->first)
		s->first = pte;
	if(pte > s->last)
		s->last = pte;
	pg = &(*pte)->pages[(soff&(s->ptemapmem-1))>>s->pgszlg2];
	if(*pg != nil)
		putpage(*pg);
	*pg = p;
	if(pg < (*pte)->first)
		(*pte)->first = pg;
	if(pg > (*pte)->last)
		(*pte)->last = pg;
	p->n = 1;
}

void
mfreeseg(Segment *s, uintptr start, uintptr top)
{
	int i, j, size;
	usize pages;
	uintptr soff;
	Page *pg;
	Page *list;

	if(canqlock(&s->lk))
		panic("mfreeseg: lock");
	start = ROUNDUP(start, (1<<s->pgszlg2));
	pages = (top-start)>>s->pgszlg2;
	soff = start-s->base;
	j = (soff&(s->ptemapmem-1))>>s->pgszlg2;

	size = s->mapsize;
	list = nil;
	for(i = soff/s->ptemapmem; i < size; i++) {
		if(pages <= 0)
			break;
		if(s->map[i] == nil) {
			pages -= PTEPERTAB-j;
			j = 0;
			continue;
		}
		while(j < PTEPERTAB) {
			pg = s->map[i]->pages[j];
			/*
			 * We want to zero s->map[i]->page[j] and putpage(pg),
			 * but we have to make sure other processors flush the
			 * entry from their TLBs before the page is freed.
			 * We construct a list of the pages to be freed, zero
			 * the entries, then (below) call procflushseg, and call
			 * putpage on the whole list. If swapping were implemented,
			 * paged-out pages can't be in a TLB and could be disposed of here.
			 */
			if(pg != nil){
				if(pg->n == 0){
					/* if being paged in, wait until quiescent */
					qlock(pg);
					qunlock(pg);
				}
				pg->next = list;
				list = pg;
				s->map[i]->pages[j] = nil;
			}
			if(--pages == 0)
				goto out;
			j++;
		}
		j = 0;
	}
out:
	/* flush this seg in all other processes */
	if(s->ref > 1)
		procflushseg(s);

	/* free the pages */
	for(pg = list; pg != nil; pg = list){
		list = list->next;
		putpage(pg);
	}
}

Segment*
isoverlap(Proc* p, uintptr va, usize len)
{
	int i;
	Segment *ns;
	uintptr newtop;

	newtop = va+len;
	for(i = 0; i < NSEG; i++) {
		ns = p->seg[i];
		if(ns == nil)
			continue;
		if((newtop > ns->base && newtop <= ns->top) ||
		   (va >= ns->base && va < ns->top))
			return ns;
	}
	return nil;
}

void
segclock(uintptr pc)
{
	Segment *s;

	s = up->seg[TSEG];
	if(s == 0 || s->profile == nil)
		return;

	s->profile[0] += TK2MS(1);
	if(pc >= s->base && pc < s->top) {
		pc -= s->base;
		s->profile[pc>>LRESPROF] += TK2MS(1);
	}
}
