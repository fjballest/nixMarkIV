#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

/*
 * See segment.c for locking rules
 */

extern char *segtypename[];

int
fault(uintptr addr, int read)
{
	Segment *s;
	char *sps;
	int r;

	DBG("fault pid %d %#p %c\n", up->pid, addr, read?'r':'w');
	if(up->nlocks){
		print("fault nlocks %d\n", up->nlocks);
		dumpstack();
	}

	sps = up->psstate;
	up->psstate = "Fault";
	spllo();

	m->pfault++;
	s = seg(up, addr, 1);	/* leaves s->lk qlocked if s != nil */
	if(s == nil) {
		up->psstate = sps;
		return -1;
	}

	if(!read && (s->type&SG_RONLY)) {
		qunlock(&s->lk);
		up->psstate = sps;
		return -1;
	}
	/* fixfault releases s->lk */
	r = fixfault(s, addr, read, 1);
	up->psstate = sps;
	return r;
}

static void
faulterror(char *s, Chan *c, int freemem)
{
	char buf[ERRMAX];

	if(up->errstr[0] == 0)
		kstrcpy(up->errstr, "unexpected eof", ERRMAX);
	if(c != nil && c->path != nil){
		seprint(buf, buf+sizeof buf, "%s: %N: %s", s, c->path, up->errstr);
		s = buf;
	}
	DBG("faulterror pid %d %s\n", up->pid, s);
	if(up->nerrlab) {
		postnote(up, 1, s, NDebug);
		error(s);
	}
	pexit(s, freemem);
}

static void
pgread(Chan *c, usize pgsz, Page *pg, usize count, vlong daddr)
{
	usize n, tot;
	KMap *k;
	char *kaddr;

	k = kmap(pg);
	kaddr = (char*)VA(k);
	if(waserror()){
		kunmap(k);
		pg->n = 0;
		nexterror();
	}
	if(count > pgsz)
		count = pgsz;
	while(waserror()) {
		if(strcmp(up->errstr, Eintr) == 0)
			continue;
		DBG("pagein: %s\n", up->errstr);
		faulterror(Eioload, c, 0);
	}
	/* Pages might be large: readn */
	for(tot = 0; tot < count; tot += n){
		up->errstr[0] = 0;
		kaddr[tot] = 0;	/* make sure it's paged in */
		n = c->dev->read(c, kaddr+tot, count-tot, daddr+tot);
		if(n <= 0){
			poperror();
			faulterror(Eioload, c, 0);
		}
	}
	if(count < pgsz)
		memset(kaddr+count, 0, pgsz-count);
	poperror();
	kunmap(k);
	pg->n = 1;
	poperror();
}

void
pagedin(Page *pg)
{
	if(pg->n != 0)
		return;
	/* Make sure it's paged in, by going through the turnstile */
	DBG("pagein wait pid %d addr %#p\n", up->pid, pg->va);
	qlock(pg);
	qunlock(pg);
	if(pg->n == 0)
		faulterror(Eioload, nil, 0);
	DBG("pagein woke pid %d addr %#p\n", up->pid, pg->va);
}

static void
pagein(Segment *s, uintptr addr, Page **pg)
{
	Page *new, **spg;
	uintptr soff, pgsz;

	pgsz = 1<<s->pgszlg2;
	addr &= ~(pgsz-1);
	soff = addr-s->base;

	if(*pg != nil){
		qunlock(&s->lk);
		pagedin(*pg);
		return;
	}
	if(soff >= s->flen){
		DBG("pagein: zfod %#p\n", addr);
		new = newpage(1<<s->pgszlg2, s->color, 1, addr);
		*pg = new;
		new->n = 1;
		qunlock(&s->lk);
		return;
	}
	DBG("pagein pid %d s %#p addr %#p soff %#p\n", up->pid, s->base, addr, soff);
	spg = nil;
	if(s->src != nil){
		qlock(&s->src->lk);
		spg = segwalk(s->src, addr, 1);
		if(*spg != nil){
			*pg = *spg;
			incref(*pg);
			qunlock(&s->src->lk);
			qunlock(&s->lk);
			pagedin(*pg);
			return;
		}
	}
	new = newpage(1<<s->pgszlg2, s->color, 0, addr);
	*pg = new;
	DBG("pagein io pid %d addr %#p\n", up->pid, addr);
	qlock(new);
	if(spg != nil){
		*spg = new;
		incref(new);
		qunlock(&s->src->lk);
	}
	qunlock(&s->lk);
	if(waserror()){
		qunlock(new);
		nexterror();
	}
	if(s->src != nil)
		pgread(s->src->c, pgsz, new, s->flen-soff, s->fstart+soff);
	else
		pgread(s->c, pgsz, new, s->flen-soff, s->fstart+soff);
	qunlock(new);
	poperror();
}

/*
 * Called with s->lk locked to fix a fault.
 * Returns with s->lk unlocked.
 *
 * There are no page outs.
 * calls to pagein() release the lock, because the process
 * might have to sleep waiting for another to do the page in.
 * Thus, after doing the pagein(), the segment is unlocked
 * and the page must be already allocated and paged in.
 */
int
fixfault(Segment *s, uintptr addr, int read, int dommuput)
{
	int type;
	uint mmuflags;
	uintptr pgsize;
	Page **pg, *opg, *new;
	Page *(*fn)(Segment*, uintptr);
	Chan *c;

	USED(read);
	if(canqlock(&s->lk))
		panic("fixfault: lock");

	c = s->c;
	if(c == nil && s->src != nil)
		c = s->src->c;
	pgsize = 1<<s->pgszlg2;
	addr &= ~(pgsize-1);
	pg = segwalk(s, addr, 1);
	type = s->type&SG_TYPE;
	mmuflags = 0;
	DBG("fixfault pid %d s %N sref %d %s %#p addr %#p pg %#p r%d n%d\n",
		up?up->pid:0, s->cpath, s->ref, segtypename[s->type&SG_TYPE], s->base, addr,
		(*pg)?(*pg)->pa:0, (*pg)?(*pg)->ref:0, (*pg)?(*pg)->n:-1);

	if(waserror()){
		DBG("fixfault err pid %d %s s %N %s %#p addr %#p\n",
			up?up->pid:0, up->errstr,
			c?c->path:nil, segtypename[s->type&SG_TYPE], s->base, addr);
		qunlock(&s->lk);
		return -1;
	}
	switch(type) {
	default:
		panic("fault");
		break;

	case SG_TEXT:
		/* Demand load */
		pagein(s, addr, pg);	/* releases s->lk */
		mmuflags = PTERONLY|PTEVALID;
		break;

	case SG_SHARED:	
	case SG_STACK:
		/* Zero fill on demand */
		if(*pg == nil){
			new = newpage(1<<s->pgszlg2, s->color, 1, addr);
			qlock(new);
			new->n = 1;
			*pg = new;
			qunlock(new);
			qunlock(&s->lk);
			mmuflags = PTEWRITE|PTEVALID;
			break;
		}
		goto cow;

	case SG_DATA:
		/* Demand load and copy on reference */
		pagein(s, addr, pg);	/* releases s->lk */
		qlock(&s->lk);
	cow:
		/*
		 * If the page is shared, copy it, even for read access.
		 * Otherwise, just install the entry in the mmu.
		 */
		if((*pg)->ref <= 0)
			panic("fixfault: ref %d", (*pg)->ref);
		lock(*pg);
		if((*pg)->ref > 1){
			unlock(*pg);
			new = newpage(1<<s->pgszlg2, s->color, 0, addr);
			pagecpy(new, *pg);
			opg = *pg;
			*pg = new;
			putpage(opg);
		}else
			unlock(*pg);
		if((*pg)->ref != 1 && (*pg)->n == 0)
			panic("fixfault: cow: ref %d n %d", (*pg)->ref, (*pg)->n);
		qunlock(&s->lk);
		mmuflags = PTEWRITE | PTEVALID;
		break;

	case SG_PHYSICAL:
		if(*pg == 0) {
			fn = s->pseg->pgalloc;
			if(fn)
				*pg = (*fn)(s, addr);
			else
				panic("phys seg palloc not implemented");
		}
		qunlock(&s->lk);

		mmuflags = PTEWRITE|PTEUNCACHED|PTEVALID;
		break;
	}
	if(dommuput){
		if(addr != (*pg)->va)
			panic("fixfault addr %#p va %#p", addr, (*pg)->va);
		mmuput(up, *pg, mmuflags);
	}
	poperror();
	DBG("fixfaulted pid %d s %N %s %#p addr %#p pg %#p ref %d\n",
		up?up->pid:0, c?c->path:nil, segtypename[s->type&SG_TYPE], s->base, addr,
		(*pg)?(*pg)->pa:0, (*pg)?(*pg)->ref:0);
	return 0;
}

/*
 * Called only in a system call
 */
int
okaddr(uintptr addr, long len, int write)
{
	Segment *s;
	uchar *p, *e, c;
	long tot, n;

	for(tot = 0; tot < len; tot += n){
		s = seg(up, addr+tot, 0);
		if(s == nil || (write && (s->type&SG_RONLY)))
			return 0;
		n = len-tot;
		if(addr+len > s->top)
			n = s->top - (addr+tot);
		e = UINT2PTR(addr+tot+n);
		for(p = UINT2PTR(addr+tot); p < e; p += (1<<s->pgszlg2)){
			c = *p;
			if(write)
				*p = c;
		}
	}
	return 1;
}

void*
validaddr(void* addr, long len, int write)
{
	if(!okaddr(PTR2UINT(addr), len, write)){
		pprint("suicide: invalid address %#p/%ld in sys call pc=%#p\n",
			addr, len, userpc(nil));
		pexit("Suicide", 0);
	}

	return UINT2PTR(addr);
}

/*
 * &s[0] is known to be a valid address.
 * Uses PGSZ to be sure all pages are checked out, the user
 * might be using larger pages.
 */
void*
vmemchr(void *s, int c, int n)
{
	int r;
	uintptr a;
	void *t;

	a = PTR2UINT(s);
	while(ROUNDUP(a, PGSZ) != ROUNDUP(a+n-1, PGSZ)){
		/* spans pages; handle this page */
		r = PGSZ - (a & (PGSZ-1));
		t = memchr(UINT2PTR(a), c, r);
		if(t)
			return t;
		a += r;
		n -= r;
		if(!ISKADDR(a))
			validaddr(UINT2PTR(a), 1, 0);
	}

	/* fits in one page */
	return memchr(UINT2PTR(a), c, n);
}

Segment*
seg(Proc *p, uintptr addr, int dolock)
{
	Segment **s, **et, *n;

	et = &p->seg[NSEG];
	for(s = p->seg; s < et; s++) {
		n = *s;
		if(n == 0)
			continue;
		if(addr >= n->base && addr < n->top) {
			if(dolock == 0)
				return n;
			qlock(&n->lk);
			if(addr >= n->base && addr < n->top)
				return n;
			qunlock(&n->lk);
		}
	}

	return 0;
}
