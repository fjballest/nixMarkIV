#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"

Segment* (*_globalsegattach)(Proc*, char*);

static Lock physseglock;

int
addphysseg(Physseg* new)
{
	Physseg *ps;

	/*
	 * Check not already entered and there is room
	 * for a new entry and the terminating null entry.
	 */
	lock(&physseglock);
	for(ps = physseg; ps->name; ps++){
		if(strcmp(ps->name, new->name) == 0){
			unlock(&physseglock);
			return -1;
		}
	}
	if(ps-physseg >= nphysseg-2){
		unlock(&physseglock);
		return -1;
	}

	*ps = *new;
	unlock(&physseglock);

	return 0;
}

int
isphysseg(char *name)
{
	int rv;
	Physseg *ps;

	lock(&physseglock);
	rv = 0;
	for(ps = physseg; ps->name; ps++){
		if(strcmp(ps->name, name) == 0){
			rv = 1;
			break;
		}
	}
	unlock(&physseglock);
	return rv;
}

/* Needs to be non-static for BGP support */
uintptr
ibrk(uintptr addr, int seg)
{
	Segment *s, *ns;
	uintptr newtop, pgsize;
	usize newsize;
	int i;

	s = up->seg[seg];
	if(s == 0)
		error(Ebadarg);

	if(addr == 0)
		return s->top;

	qlock(&s->lk);

	/* You can grow the segment but shrink is not allowed */
	if(addr < s->base) {
		if(seg != DSEG || up->seg[DSEG] == 0 || addr < up->seg[DSEG]->top) {
			qunlock(&s->lk);
			error(Enovmem);
		}
		addr = s->base;
	}

	pgsize = 1<<s->pgszlg2 ;
	newtop = ROUNDUP(addr, pgsize);
	newsize = (newtop-s->base)/pgsize;
	if(newtop < s->top)
		panic("ibrk doesn't shrink");

	for(i = 0; i < NSEG; i++) {
		ns = up->seg[i];
		if(ns == 0 || ns == s)
			continue;
		if(newtop >= ns->base && newtop < ns->top) {
			qunlock(&s->lk);
			error(Esoverlap);
		}
	}

	if(newtop - s->base > SEGMAXSIZE) {
		qunlock(&s->lk);
		error(Enovmem);
	}

	segmapsize(s, HOWMANY(newsize, PTEPERTAB));

	s->top = newtop;
	s->size = newsize;
	qunlock(&s->lk);

	return newtop;
}

void
syssegbrk(Ar0* ar0, va_list list)
{
	int i;
	uintptr addr;
	Segment *s;

	/*
	 * int segbrk(void*, void*);
	 * should be
	 * void* segbrk(void* saddr, void* addr);
	 */
	addr = PTR2UINT(va_arg(list, void*));
	for(i = 0; i < NSEG; i++) {
		s = up->seg[i];
		if(s == nil || addr < s->base || addr >= s->top)
			continue;
		switch(s->type&SG_TYPE) {
		case SG_TEXT:
		case SG_DATA:
		case SG_STACK:
			error(Ebadarg);
		default:
			addr = PTR2UINT(va_arg(list, void*));
			ar0->v = UINT2PTR(ibrk(addr, i));
			return;
		}
	}
	error(Ebadarg);
}

void
sysbrk_(Ar0* ar0, va_list list)
{
	uintptr addr;

	/*
	 * int brk(void*);
	 *
	 * Deprecated; should be for backwards compatibility only.
	 * But, ipconfig, acme, ... everyone keeps using this. [nemo]
	 */
	addr = PTR2UINT(va_arg(list, void*));

	ibrk(addr, DSEG);

	ar0->i = 0;
}

static uintptr
segattach(Proc* p, int attr, char* name, uintptr va, usize len)
{
	int sno;
	Segment *s, *os;
	Physseg *ps;

	/* BUG: Only ok for now */
	if((va != 0 && va < UTZERO) || ISKADDR(va))
		error("virtual address in kernel");

	vmemchr(name, 0, ~0);

	for(sno = 0; sno < NSEG; sno++)
		if(p->seg[sno] == nil && sno != ESEG)
			break;

	if(sno == NSEG)
		error("too many segments in process");

	/*
	 *  first look for a global segment with the
	 *  same name
	 */
	if(_globalsegattach != nil){
		s = (*_globalsegattach)(p, name);
		if(s != nil){
			p->seg[sno] = s;
			return s->base;
		}
	}

	len = ROUNDUP(len, PGSZ);
	if(len == 0)
		error("length overflow");

	/*
	 * Find a hole in the address space.
	 * Starting at the lowest possible stack address - len,
	 * check for an overlapping segment, and repeat at the
	 * base of that segment - len until either a hole is found
	 * or the address space is exhausted.
	 */
//need check here to prevent mapping page 0?
	if(va == 0) {
		va = p->seg[SSEG]->base - len;
		for(;;) {
			os = isoverlap(p, va, len);
			if(os == nil)
				break;
			va = os->base;
			if(len > va)
				error("cannot fit segment at virtual address");
			va -= len;
		}
	}

	va = va&~(PGSZ-1);
	if(isoverlap(p, va, len) != nil)
		error(Esoverlap);

	for(ps = physseg; ps->name; ps++)
		if(strcmp(name, ps->name) == 0)
			goto found;

	error("segment not found");
found:
	if((len/PGSZ) > ps->size)
		error("len > segment size");

	attr &= ~SG_TYPE;		/* Turn off what is not allowed */
	attr |= ps->attr;		/* Copy in defaults */

	s = newseg(attr, va, va+len, nil, PGSHFT);
	if(s->pgszlg2 != PGSHFT)
		panic("segattach: physseg with wrong page size");
	s->pseg = ps;
	p->seg[sno] = s;

	return va;
}

void
syssegattach(Ar0* ar0, va_list list)
{
	int attr;
	char *name;
	uintptr va;
	usize len;

	/*
	 * long segattach(int, char*, void*, ulong);
	 * should be
	 * void* segattach(int, char*, void*, usize);
	 */
	attr = va_arg(list, int);
	name = va_arg(list, char*);
	va = PTR2UINT(va_arg(list, void*));
	len = va_arg(list, usize);

	ar0->v = UINT2PTR(segattach(up, attr, validaddr(name, 1, 0), va, len));
}

void
syssegdetach(Ar0* ar0, va_list list)
{
	int i;
	uintptr addr;
	Segment *s;

	/*
	 * int segdetach(void*);
	 */
	addr = PTR2UINT(va_arg(list, void*));

	qlock(&up->seglock);
	if(waserror()){
		qunlock(&up->seglock);
		nexterror();
	}

	s = 0;
	for(i = 0; i < NSEG; i++)
		if(s = up->seg[i]) {
			qlock(&s->lk);
			if((addr >= s->base && addr < s->top) ||
			   (s->top == s->base && addr == s->base))
				goto found;
			qunlock(&s->lk);
		}

	error(Ebadarg);

found:
	/*
	 * Can't detach the initial stack segment
	 * because the clock writes profiling info
	 * there.
	 */
	if(s == up->seg[SSEG]){
		qunlock(&s->lk);
		error(Ebadarg);
	}
	up->seg[i] = 0;
	qunlock(&s->lk);
	putseg(s);
	qunlock(&up->seglock);
	poperror();

	/* Ensure we flush any entries from the lost segment */
	mmuflush();

	ar0->i = 0;
}

void
syssegfree(Ar0* ar0, va_list list)
{
	Segment *s;
	uintptr from, to;
	usize len;

	/*
	 * int segfree(void*, ulong);
	 * should be
	 * int segfree(void*, usize);
	 */
	from = PTR2UINT(va_arg(list, void*));
	s = seg(up, from, 1);
	if(s == nil)
		error(Ebadarg);
	len = va_arg(list, usize);
	to = from + len;
	if(to < from || to > s->top){
		qunlock(&s->lk);
		error(Ebadarg);
	}
	mfreeseg(s, from, to);
	qunlock(&s->lk);
	mmuflush();

	ar0->i = 0;
}

static void
pteflush(Pte *pte, int s, int e)
{
	USED(pte);
	USED(s);
	USED(e);
}

void
syssegflush(Ar0* ar0, va_list list)
{
	Segment *s;
	uintptr addr, pgsize;
	Pte *pte;
	usize chunk, l, len, pe, ps;

	/*
	 * int segflush(void*, ulong);
	 * should be
	 * int segflush(void*, usize);
	 */
	addr = PTR2UINT(va_arg(list, void*));
	len = va_arg(list, usize);

	while(len > 0) {
		s = seg(up, addr, 1);
		if(s == nil)
			error(Ebadarg);

		s->flushme = 1;
		pgsize = 1<<s->pgszlg2 ;
	more:
		l = len;
		if(addr+l > s->top)
			l = s->top - addr;

		ps = addr-s->base;
		pte = s->map[ps/s->ptemapmem];
		ps &= s->ptemapmem-1;
		pe = s->ptemapmem;
		if(pe-ps > l){
			pe = ps + l;
			pe = (pe+pgsize-1)&~(pgsize-1);
		}
		if(pe == ps) {
			qunlock(&s->lk);
			error(Ebadarg);
		}

		if(pte)
			pteflush(pte, ps/pgsize, pe/pgsize);

		chunk = pe-ps;
		len -= chunk;
		addr += chunk;

		if(len > 0 && addr < s->top)
			goto more;

		qunlock(&s->lk);
	}
	mmuflush();

	ar0->i = 0;
}
