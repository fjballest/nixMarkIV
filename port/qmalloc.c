/*
 * malloc
 *
 *	Derived from Quickfit (see SIGPLAN Notices October 1988)
 *	with a Kernighan & Ritchie allocator.
 *
 *	Tries to avoid external fragmentation by
 *	using a larger atomic unit, caching most of the mallocs
 *	in the kernel and rounding allocations.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"

/* set to n to track the last n allocations */
#define DEBUGALLOC 512

typedef union Header Header;
typedef struct Qlist Qlist;
typedef struct MSeg MSeg;
typedef struct Pcx Pcx;
typedef struct Pcc Pcc;

/* stats */
enum
{
	QSmalloc,
	QSmallocquick,
	QSmallocrover,
	QSmalloctail,
	QSmalign,
	QSmalignquick,
	QSmalignrover,
	QSmalignfront,
	QSmalignrear,
	QSmaligntail,
	QSmaligntail2,
	QSfree,
	QSfreetail,
	QSfreequick,
	QSfreeprev,
	QSfreefrag,
	QSrealloc,
	QSrealloceq,
	QSrealloctail,
	QSreallocnew,
	NQStats,
};

union Header
{
	struct{
		Header*	next;
		union{
			uint	size;
			Header*	prev;
		};
		uintptr	tag;
	} s;
	uchar _align[0x20];
};

/*
 * The atomic allocation unit is Unitsz bytes and matches a Header.
 * The user is always given a multiple of Mult*Unitsz bytes.
 */
enum
{
	Unitsz	= sizeof(Header),	/* actual allocation unit size */
	Mult	= 4,			/* a multiple of Nunits */

	/*
	 * enough for a 9P message.
	 */
	NQUICK = HOWMANY(8192+IOHDRSZ+1, Unitsz*Mult)*Mult + 1,

	ATAG = 0x99u,
	FTAG = 0x66u,
};

struct Qlist
{
	Lock	lk;
	Header*	first;
	uint	nalloc;
};

struct MSeg
{
	Header*	base;		/* base of malloc area */
	Header*	end;		/* end of malloc area */
	Header*	rover;		/* ptr to next fit fragment list */
	Header*	tailptr;	/* start of unallocated tail in area */
	MSeg*	next;		/* next malloc area */
	uint	maxsz;		/* ~0 or size of largest free fragment */
	Lock;
};

struct Pcx
{
	void* v;
	uintptr	pc;
};

struct Pcc
{
	uintptr pc;
	uint n;
	uint min;
	uint max;
};

/* ((x) != 0) && ... */
#define ISPOWEROF2(x)	(!((x) & ((x)-1)))
#define ALIGNHDR(h, a)	(Header*)((((uintptr)(h))+((a)-1)) & ~((a)-1))
#define ALIGNED(h, a)	((((uintptr)(h)) & (a-1)) == 0)

/* debug */
#define xmalloc(x)	malloc(x)
#define	xfree(x)	free(x)

static	void*	qmalloc(usize);
static	void	qfreeinternal(void*, int);
static	int	morecore(usize);

static	int	qstats[NQStats];


static	Qlist	quicklist[NQUICK+1];

static	Lock	mainlock;
static	MSeg	mseg0, *mseg;
static	int	verbdump;
static	int	poison;

/* >sed 's/.*(QS[a-z]+).*$/[\1]	"\1",/'
 */
static	char*	qstatname[NQStats] = 
{
[QSmalloc]	"QSmalloc",
[QSmallocquick]	"QSmallocquick",
[QSmallocrover]	"QSmallocrover",
[QSmalloctail]	"QSmalloctail",
[QSmalign]	"QSmalign",
[QSmalignquick]	"QSmalignquick",
[QSmalignrover]	"QSmalignrover",
[QSmalignfront]	"QSmalignfront",
[QSmalignrear]	"QSmalignrear",
[QSmaligntail]	"QSmaligntail",
[QSmaligntail2]	"QSmaligntail2",
[QSfree]	"QSfree",
[QSfreetail]	"QSfreetail",
[QSfreequick]	"QSfreequick",
[QSfreeprev]	"QSfreeprev",
[QSfreefrag]	"QSfreefrag",
[QSrealloc]	"QSrealloc",
[QSrealloceq]	"QSrealloceq",
[QSrealloctail]	"QSrealloctail",
[QSreallocnew]	"QSreallocnew",
};


static Lock pcxlock;
static Pcx pcx[DEBUGALLOC+1];
static Pcc pcc[DEBUGALLOC+1];
int debugalloc;

/*
 * units needed to alloc n user bytes + 1 byte holding the end tag:
 * if user asks for less than 1 unit: give 1 unit + 1 header unit.
 * Otherwise 1 unit for the header plus a multiple of Mult units of Unitsz bytes.
 *
 */
static uint
NUNITS(usize nbytes)
{
	uint nu;

	nu = HOWMANY(nbytes+1, Unitsz);
	if(nu < Mult)
		return nu+1;
	return ROUNDUP(nu, Mult)+1;
}

static void
remember(uintptr pc, void *v, uint sz)
{
	int i;

	if(debugalloc == 0 || v == nil)
		return;

	ilock(&pcxlock);
	for(i = 0; i < nelem(pcc); i++)
		if(pcc[i].pc == pc || pcc[i].pc == 0){
			pcc[i].pc = pc;
			pcc[i].n++;
			if(pcc[i].min == 0 || pcc[i].min > sz)
				pcc[i].min = sz;
			if(pcc[i].max == 0 || pcc[i].max < sz)
				pcc[i].max = sz;
			break;
		}
	for(i = 0; i < nelem(pcx); i++)
		if(pcx[i].v == v){
			pcx[i].pc = pc;
			break;
		}
	if(i == nelem(pcx))
		for(i = 0; i < nelem(pcx); i++)
			if(pcx[i].pc == 0){
				pcx[i].pc = pc;
				pcx[i].v = v;
				break;
			}

	iunlock(&pcxlock);
}

static void
forget(void *v)
{
	int i;

	if(debugalloc == 0 || v == nil)
		return;

	ilock(&pcxlock);
	for(i = 0; i < nelem(pcx); i++)
		if(pcx[i].v == v){
			pcx[i].pc = 0;
			break;
		}
	iunlock(&pcxlock);
}


static char*
mallocsummary(char* s, char* e, void*)
{
	uintmem used, tot;
	int n;
	MSeg *mseg;

	used = 0;
	tot = 0;
	n = 0;
	for(mseg = &mseg0; mseg != nil; mseg = mseg->next){
		used += (mseg->tailptr - mseg->base)*Unitsz;
		tot += (mseg->end - mseg->base)*Unitsz;
		n++;
	}
	return seprint(s, e, "%llud/%llud malloc %d segs\n", used, tot, n);
}

static MSeg*
mallocinitseg(MSeg *s, void *p, uintmem len)
{
	char *cp, *ep;

	if(s == nil){
		s = p;
		memset(s, 0, sizeof *s);
		cp = p;
		cp += sizeof *s;
		len -= sizeof *s;
	}else
		cp = p;
	s->base = (Header*)cp;
	s->base = ALIGNHDR(s->base, Unitsz);
	ep = cp+len;
	cp = (char*)s->base;
	s->tailptr = s->base;
	s->end = s->base + ((ep-cp)/Unitsz);
	s->maxsz = ~0;
	return s;
}

void
setmalloctag(void *ap, uintptr tag)
{
	Header *p;

	p = ap;
	if(p == nil){
		iprint("setmalloctag: nil");
		panic("setmalloctag: nil");
	}
	p--;
	p->s.tag = tag;
	if(DEBUGALLOC != 0)
		remember(tag, ap, p->s.size);
}

uintptr
getmalloctag(void *ap)
{
	Header *p;

	p = ap;
	if(p == nil){
		iprint("getmalloctag: nil");
		panic("getmalloctag: nil");
	}
	p--;
	return p->s.tag;
}

static Header*
tailalloc(uint n)
{
	Header *p;

	p = mseg->tailptr;
	mseg->tailptr += n;
	p->s.size = n;
	p->s.next = nil;
	return p;
}

static void*
qmallocalign(usize nbytes, uintptr align)
{
	Qlist *qlist;
	uintptr aligned;
	Header **pp, *p, *prev, *r;
	uint naligned, nunits, n, mc;

	if(nbytes == 0)
		return nil;
	if(!ISPOWEROF2(align))
		panic("qmallocalign");
	if(align <= Unitsz)
		return xmalloc(nbytes);

	qstats[QSmalign]++;
	nunits = NUNITS(nbytes);
	if(nunits <= NQUICK){
		/*
		 * Look for a conveniently aligned block
		 * on one of the quicklists.
		 */
		qlist = &quicklist[nunits];
		ilock(&qlist->lk);
		pp = &qlist->first;
		for(p = *pp; p != nil; p = p->s.next){
			if(p->s.size != nunits)
				iprint("quick[%ud] %#p size %ud\n",
					nunits, p, p->s.size);
			if(ALIGNED(p+1, align)){
				*pp = p->s.next;
				p->s.next = nil;
				qstats[QSmalignquick]++;
				((uchar*)(p+p->s.size))[-1] = ATAG;
				iunlock(&qlist->lk);
				return p+1;
			}
			pp = &p->s.next;
		}
		iunlock(&qlist->lk);
	}

	ilock(&mainlock);
	mc = 0;
Again:
	mseg = &mseg0;
Another:
	for(; mseg != nil && nunits > mseg->end - mseg->tailptr; mseg = mseg->next)
		;
	if(mseg != nil){
		p = ALIGNHDR(mseg->tailptr+1, align);
		if(p != mseg->tailptr+1){
			naligned = HOWMANY(align, Unitsz);
			if(mseg->end - mseg->tailptr < nunits+naligned){
				mseg = mseg->next;
				goto Another;
			}
			/*
			 * Save the residue before the aligned allocation
			 * and free it after the tail pointer has been bumped
			 * for the main allocation.
			 */
			n = p - mseg->tailptr - 1;
			r = tailalloc(n);
			p = tailalloc(nunits);
			qstats[QSmaligntail2]++;
			qfreeinternal(r+1, 0);
		}else{
			p = tailalloc(nunits);
			qstats[QSmaligntail]++;
		}
		iunlock(&mainlock);
		((uchar*)(p+nunits))[-1] = ATAG;
		return p+1;
	}

	/* hard way */
	for(mseg = &mseg0; mseg != nil; mseg = mseg->next){
		prev = mseg->rover;
		if(prev == nil)
			continue;
		do{
			p = prev->s.next;
			if(p->s.size < nunits)
				continue;
			aligned = ALIGNED(p+1, align);
			naligned = HOWMANY(align, Unitsz);
			if(!aligned && p->s.size < nunits+naligned)
				continue;

			/*
			 * This block is big enough, remove it
			 * from the list.
			 */
			qstats[QSmalignrover]++;
			if(prev->s.next == p->s.next)
				mseg->rover = nil;
			else{
				prev->s.next = p->s.next;
				mseg->rover = prev;
			}
			if(p->s.size == mseg->maxsz)
				mseg->maxsz = ~0;
			/*
			 * Free any runt in front of the alignment.
			 */
			if(!aligned){
				r = p;
				p = ALIGNHDR(p+1, align) - 1;
				n = p - r;
				p->s.size = r->s.size - n;

				r->s.size = n;
				r->s.next = nil;
				qfreeinternal(r+1, 0);
				qstats[QSmalignfront]++;
			}

			/*
			 * Free any residue after the aligned block.
			 */
			if(p->s.size > nunits && p->s.size-nunits>Mult){
				r = p+nunits;
				r->s.size = p->s.size - nunits;
				r->s.next = nil;
				qfreeinternal(r+1, 0);
				p->s.size = nunits;
				qstats[QSmalignrear]++;
			}

			p->s.next = nil;
			((uchar*)(p+p->s.size))[-1] = ATAG;
			iunlock(&mainlock);
			return p+1;
		}while((prev = p) != mseg->rover);
	}
	if(mc++ < 3 && morecore(nunits+HOWMANY(align, Unitsz)) == 0)
		goto Again;
	iunlock(&mainlock);
	panic(Enomem);
	return nil;
}

static void*
qmalloc(usize nbytes)
{
	Qlist *qlist;
	Header *p, *prev;
	uint nunits, i, msize, mc, nq;

	if(nbytes == 0)
		return nil;

	qstats[QSmalloc]++;
	nunits = NUNITS(nbytes);
	if(nunits <= NQUICK){
		nq = 1;
		if(nunits > Mult)
			nq = Mult;
		for(i = 0; i < nq && nunits+i < NQUICK; i++){
			qlist = &quicklist[nunits+i];
			ilock(&qlist->lk);
			if((p = qlist->first) != nil){
				if(p->s.size < nunits+i)
					panic("quick %d\t%#p %ud %ud\n",
						nunits+i, p, p->s.size, nunits+i);
				qlist->first = p->s.next;
				qlist->nalloc++;
				p->s.next = nil;
				((uchar*)(p+p->s.size))[-1] = ATAG;
				qstats[QSmallocquick]++;
				iunlock(&qlist->lk);
				return p+1;
			}
			iunlock(&qlist->lk);
		}
	}
	ilock(&mainlock);
	mc = 0;
Again:
	mseg = &mseg0;
	for(; mseg != nil && nunits > mseg->end - mseg->tailptr; mseg = mseg->next)
		;
	if(mseg != nil){
		p = tailalloc(nunits);
		((uchar*)(p+nunits))[-1] = ATAG;
		qstats[QSmalloctail]++;
		iunlock(&mainlock);
		return p+1;
	}
	for(mseg = &mseg0; mseg != nil; mseg = mseg->next){
		prev = mseg->rover;
		if(prev == nil)
			continue;
		if(mseg->maxsz != ~0 && nunits > mseg->maxsz)
			continue;
		msize = 0;
		do {
			p = prev->s.next;
			if(p->s.size > msize)
				msize = p->s.size;
			if(p->s.size >= nunits){
				if(p->s.size > nunits && p->s.size-nunits>=Mult){
					p->s.size -= nunits;
					p += p->s.size;
					p->s.size = nunits;
				}else if(prev->s.next == p->s.next)
					mseg->rover = nil;
				else{
					prev->s.next = p->s.next;
					mseg->rover = prev;
				}
				if(p->s.size == mseg->maxsz)
					mseg->maxsz = ~0;
				p->s.next = nil;
				qstats[QSmallocrover]++;
				((uchar*)(p+p->s.size))[-1] = ATAG;
				iunlock(&mainlock);
				return p+1;
			}
		}while((prev = p) != mseg->rover);
		mseg->maxsz = msize;
	}
	if(mc++ < 3 && morecore(nunits) == 0)
		goto Again;
	iunlock(&mainlock);
	panic(Enomem);
	return nil;
}

static void
qfreeinternal(void* ap, int checktag)
{
	Qlist *qlist;
	Header *p, *prev;
	uint nunits;

	p = ap;
	if(p == nil)
		return;
	qstats[QSfree]++;

	p--;
	if(p < mseg->base || p >= mseg->end)
		panic("qfreeinternal: base %#p p %#p end %#p", mseg->base, p, mseg->end);
	nunits = p->s.size;
	if(nunits == 0 || p->s.next != nil)
		panic("free: corrupt allocation arena");
	if(checktag){
		if(((uchar*)(p+nunits))[-1] == FTAG)
			panic("free: double free");
		if(((uchar*)(p+nunits))[-1] != ATAG)
			panic("free: user overflow");
	}
	if(poison)
		memset(p+1, 0x99, (nunits-1)*Unitsz);
	((uchar*)(p+p->s.size))[-1] = FTAG;

	if(p+nunits == mseg->tailptr){
		/* block before tail */
		mseg->tailptr = p;
		qstats[QSfreetail]++;
		return;
	}
	if(nunits <= NQUICK){
		qlist = &quicklist[nunits];
		ilock(&qlist->lk);
		p->s.next = qlist->first;
		qlist->first = p;
		qstats[QSfreequick]++;
		iunlock(&qlist->lk);
		return;
	}
	if(mseg->rover == nil){
		mseg->rover = p;
		p->s.next = p;
		qstats[QSfreefrag]++;
		return;
	}
	prev = mseg->rover;
	do{
		if(prev + prev->s.size == p){
			prev->s.size += p->s.size;
			if(mseg->maxsz != ~0 && prev->s.size > mseg->maxsz)
				mseg->maxsz = prev->s.size;
			qstats[QSfreeprev]++;
			return;
		}
	}while(prev != mseg->rover);
	p->s.next = prev->s.next;
	prev->s.next = p;
	if(mseg->maxsz != ~0 && p->s.size > mseg->maxsz)
		mseg->maxsz = p->s.size;
	qstats[QSfreefrag]++;
}

void*
realloc(void* ap, ulong size)
{
	void *v;
	Header *p;
	ulong osize;
	uint nunits, ounits;

	/*
	 * Free and return nil if size is 0
	 * (implementation-defined behaviour);
	 * behave like malloc if ap is nil;
	 * check for arena corruption;
	 * do nothing if units are the same.
	 */
	p = ap;
	if(size == 0){
		xfree(ap);
		return nil;
	}

	if(p == nil)
		return xmalloc(size);

	p--;
	ounits = p->s.size;
	if(ounits == 0 || p->s.next != nil)
		panic("realloc: corrupt allocation arena");
	if(((uchar*)(p+ounits))[-1] == FTAG)
		panic("realloc: already free");
	if(((uchar*)(p+ounits))[-1] != ATAG)
		panic("realloc: user overflow");
	nunits = NUNITS(size);

	/*
	 * Disregard quiescence and reductions (but for those to zero).
	 */
	qstats[QSrealloc]++;
	if(ounits >= nunits){
		qstats[QSrealloceq]++;
		return ap;
	}
	/*
	 * If this allocation abuts the tail, adjust the tailptr.
	 */
	ilock(&mainlock);
	for(mseg = &mseg0; mseg != nil; mseg = mseg->next)
		if(p >= mseg->base && p < mseg->end)
		if(p+ounits == mseg->tailptr && p+nunits <= mseg->end){
			p->s.size = nunits;
			mseg->tailptr = p + nunits;
			iunlock(&mainlock);
			((uchar*)(p+nunits))[-1] = ATAG;
			qstats[QSrealloctail]++;
			return ap;
		}
	iunlock(&mainlock);

	/*
	 * Allocate, copy and free.
	 * What does the standard say for failure here?
	 */
	qstats[QSreallocnew]++;
	v = xmalloc(size);
	osize = (ounits-1)*Unitsz;
	if(size < osize)
		osize = size;
	memmove(v, ap, osize);
	xfree(ap);

	return v;
}

static void
mallocdump(void*)
{
	Header *p;
	int i, j, old;
	usize n, tot, totn;
	uintptr pc;
	Qlist *qlist;
	MSeg *mseg;

	ixdumpsummary();

	print("unit %d bytes\n", Unitsz);

	tot = 0;
	totn = 0;
	for(i = 0; i <= NQUICK; i++){
		n = 0;
		qlist = &quicklist[i];
		ilock(&qlist->lk);
		pc = 0;
		if(qlist->first != nil)
			pc = qlist->first->s.tag;
		for(p = qlist->first; p != nil; p = p->s.next){
			if(p->s.size < i)
				panic("quick[%ud] %#p size %ud\n",
					i, p, p->s.size);
			n++;
		}
		iunlock(&qlist->lk);
		if(n > 0)
			print("\tquick[%d] %uld bytes, %uld frags"
				", %d bytes/frag pc %#p\n",
				i, n*i*Unitsz, n, i*Unitsz, pc);
		tot += n * i*Unitsz;
		totn += n;
	}
	print("quick: %uld bytes %uld fragments total\n", tot, totn);

	for(mseg = &mseg0; mseg != nil; mseg = mseg->next){
		p = mseg->rover;
		tot = 0;
		n = 0;
		if(p != nil){
			do {
				tot += p->s.size * Unitsz;
				n++;
				if(n < 16)
					print("rover pc %#p bytes %ud\n",
						p->s.tag, p->s.size * Unitsz);
				if(n == 16)
					print("...\n");
			} while((p = p->s.next) != mseg->rover);
		}
		print("rover: base %#p %uld bytes %uld fragments\n", mseg->base, tot, n);
		print("total: %uld/%uld\n", (mseg->tailptr-mseg->base)*Unitsz,
			(mseg->end-mseg->base)*Unitsz);
	}
	for(i = 0; i < nelem(qstats); i++)
		if(qstats[i] != 0)
			print("%s\t%ud\n", qstatname[i], qstats[i]);
	old = debugalloc;
	debugalloc = 0;
	for(i = 0; i < nelem(pcx); i++)
		if(pcx[i].pc != 0){
			n = 1;
			for(j = i+1; j < nelem(pcx); j++)
				if(pcx[i].pc == pcx[j].pc){
					pcx[j].pc = 0;
					n++;
				}
			print("src(%#p)\t#%uld\n", pcx[i].pc, n);
		}
	memset(pcx, 0, sizeof pcx);
	for(i = 0; i < nelem(pcc); i++)
		if(pcc[i].pc != 0)
			print("src(%#p)\t#%ud times: min %ud max %ud\n",
				pcc[i].pc, pcc[i].n, pcc[i].min, pcc[i].max);
	memset(pcc, 0, sizeof pcc);
	debugalloc = !old;
	print("debugalloc %d\n", debugalloc);
}

void
free(void* ap)
{
	if(ap == nil)
		return;
	if(DEBUGALLOC != 0)
		forget(ap);

	ilock(&mainlock);
	for(mseg = &mseg0; mseg != nil; mseg = mseg->next)
		if(ap >= mseg->base && ap < mseg->end)
			break;
	if(mseg == nil)
		panic("free: not from malloc");
	qfreeinternal(ap, 1);
	iunlock(&mainlock);
}

void*
malloc(ulong size)
{
	void* v;

	v = qmalloc(size);
	setmalloctag(v, getcallerpc(&size));
	memset(v, 0, size);
	return v;
}

void*
smalloc(ulong size)
{
	void* v;

	v = qmalloc(size);
	setmalloctag(v, getcallerpc(&size));
	memset(v, 0, size);
	return v;
}

void*
mallocz(ulong size, int clr)
{
	void *v;

	v = qmalloc(size);
	setmalloctag(v, getcallerpc(&size));
	if(clr)
		memset(v, 0, size);
	return v;
}

void*
mallocalign(ulong nbytes, ulong align, long offset, ulong span)
{
	void *v;

	if(span != 0 && align <= span){
		if(nbytes > span)
			return nil;
		align = span;
		span = 0;
	}

	/*
	 * Should this memset or should it be left to the caller?
	 */
	if(offset != 0 || span != 0)
		panic("mallocalign: off/span not implemented. pc %#p\n",
			getcallerpc(&nbytes));
	v = qmallocalign(nbytes, align);
	setmalloctag(v, getcallerpc(&nbytes));
	if((uintptr)v & (align-1))
		panic("mallocalign: %#p not aligned to %#ulx\n", v, align);
	memset(v, 0, nbytes);
	return v;
}

void
mallocinit(void)
{
	if(mseg0.tailptr != nil)
		return;
	if(!ISPOWEROF2(Unitsz))
		panic("mallocinit: Unitsz");
	mseg = &mseg0;
	mallocinitseg(mseg, UINT2PTR(sys->vmunused), sys->vmend - sys->vmunused);
	addsummary(iallocsummary, nil);
	addsummary(mallocsummary, nil);
	addttescape('x', mallocdump, nil);
	addttescape('i', iallocdump, nil);
}

static int
morecore(usize nunits)
{
	Page *pg;
	MSeg *s;
	extern Page *newpg(usize, int);
	extern usize mallocpgsz;
	static Lock mclock;

	if(mallocpgsz/Unitsz < nunits){
		iprint("morecore: alloc too large: %uld\n", nunits);
		return -1;
	}
	iunlock(&mainlock);
	iprint("morecore\n");

	if(canlock(&mclock) == 0){
		if(up)
			sched();
		lock(&mclock);
		unlock(&mclock);
		return 0;
	}
	/*
	 * BUG:
	 * There's a deadlock here.
	 * compile the kernel until we get the right call to
	 * morecore and the last print we'll see is "morecore".
	 * interrupts are disabled.
	 * We should probably print here interrupt status,
	 * where have they been disabled, and the entire process
	 * list, to try to spot which lock can't we get because
	 * of the call to newpg.
	 * Perhaps it's the debug print in page?
	 */
	pg = newpg(mallocpgsz, -1);
	if(pg == nil){
		iprint("no more core\n");
		unlock(&mclock);
		return -1;
	}
	ilock(&mainlock);
	s = mallocinitseg(nil, KADDR(pg->pa), mallocpgsz);
	s->next = mseg0.next;
	mseg0.next = s;
	mseg = &mseg0;
	unlock(&mclock);
	return 0;
}

