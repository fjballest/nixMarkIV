#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

#include "amd64.h"

#define ALIGNED(p, a)	(!(((uintptr)(p)) & ((a)-1)))

#define PDMAP		(0xffffffffff800000ull)
#define PDPX(v)		PTLX((v), 2)
#define PDX(v)		PTLX((v), 1)
#define PTX(v)		PTLX((v), 0)
#define PPN(p)		((p)->pa & ~((1<<(p)->pgszlg2)-1))

#define VMAP		(0xffffffffe0000000ull)
#define VMAPSZ		(256*MiB)

#define KSEG1PML4	(0xffff000000000000ull\
			|(PTLX(KSEG1, 3)<<(((3)*PTSHFT)+PGSHFT))\
			|(PTLX(KSEG1, 3)<<(((2)*PTSHFT)+PGSHFT))\
			|(PTLX(KSEG1, 3)<<(((1)*PTSHFT)+PGSHFT))\
			|(PTLX(KSEG1, 3)<<(((0)*PTSHFT)+PGSHFT)))

#define KSEG1PTP(va, l)	((0xffff000000000000ull\
			|(KSEG1PML4<<((3-(l))*PTSHFT))\
			|(((va) & 0xffffffffffffull)>>(((l)+1)*PTSHFT))\
			& ~0xfffull))

typedef struct Ptpalloc Ptpalloc;

struct Ptpalloc
{
	Lock;
	Page *free;
	usize nused;
	usize nfree;
};

static Lock vmaplock;
static Page mach0pml4;

static Ptpalloc ptpalloc;

void
mmuflushtlb(u64int)
{
	if(m->pml4->n){
		memset(UINT2PTR(m->pml4->va), 0, m->pml4->n*sizeof(PTE));
		m->pml4->n = 0;
	}
	cr3put(m->pml4->pa);
}

void
mmuflush(void)
{
	int s;

	s = splhi();
	up->newtlb = 1;
	mmuswitch();
	splx(s);
}

static Page*
mmuptpalloc(void)
{
	Page *page;

	lock(&ptpalloc);
	page = ptpalloc.free;
	if(page){
		ptpalloc.nfree--;
		ptpalloc.free = page->next;
		page->next = nil;
	}
	ptpalloc.nused++;
	unlock(&ptpalloc);
	if(page == nil){
		page = newpage(PTSZ, 0, 0, 0);
		if(PTSZ != 1<<page->pgszlg2)
			panic("mmuptpalloc page size");
		page->va = VA(kmap(page));
		/* don't unmap */
	}
	memset(UINT2PTR(page->va), 0, PTSZ);
	return page;
}

static void
mmuptprelease(Page *p)
{
	lock(&ptpalloc);
	p->next = ptpalloc.free;
	ptpalloc.free = p;
	ptpalloc.nfree++;
	ptpalloc.nused--;
	unlock(&ptpalloc);
}

static char*
ptpsummary(char *s, char *e, void*)
{
	return seprint(s, e, "%ld/%ld mmu %d pages\n",
		ptpalloc.nused, ptpalloc.nused+ptpalloc.nfree, PTSZ);
}

static void
mmuptpfree(Proc* proc, int release)
{
	int l;
	PTE *pte;
	Page **last, *page;

	/*
	 * TODO here:
	 *	coalesce the clean and release functionality
	 *	(it's either one or the other);
	 *	0-based levels, not 1-based, for consistency;
	 *	fix memset level for 2MiB pages;
	 *	use a dedicated datastructure rather than Page?
	 */
	for(l = 1; l < 4; l++){
		last = &proc->mmuptp[l];
		if(*last == nil)
			continue;
		for(page = *last; page != nil; page = page->next){
			if(!release){
				if(l == 1)
					memset(UINT2PTR(page->va), 0, PTSZ);
				pte = UINT2PTR(page->prev->va);
				pte[page->n] = 0;
			}
			last = &page->next;
		}
		*last = proc->mmuptp[0];
		proc->mmuptp[0] = proc->mmuptp[l];
		proc->mmuptp[l] = nil;
	}

	m->pml4->n = 0;
}

void
mmuswitch(void)
{
	PTE *pte;
	Page *page;

	if(up->newtlb){
		mmuptpfree(up, 0);
		up->newtlb = 0;
	}

	if(m->pml4->n){
		memset(UINT2PTR(m->pml4->va), 0, m->pml4->n*sizeof(PTE));
		m->pml4->n = 0;
	}

	pte = UINT2PTR(m->pml4->va);
	for(page = up->mmuptp[3]; page != nil; page = page->next){
		pte[page->n] = PPN(page)|PteU|PteRW|PteP;
		if(page->n >= m->pml4->n)
			m->pml4->n = page->n+1;
		page->prev = m->pml4;
	}

	tssrsp0(STACKALIGN(PTR2UINT(up->kstack+KSTACK)));
	cr3put(m->pml4->pa);
}

void
mmurelease(Proc* proc)
{
	Page *page, *next;

	/*
	 * See comments in mmuptpfree above.
	 */
	mmuptpfree(proc, 1);

	for(page = proc->mmuptp[0]; page != nil; page = next){
		next = page->next;
		mmuptprelease(page);
	}
	proc->mmuptp[0] = nil;

	tssrsp0(STACKALIGN(m->stack+MACHSTKSZ));
	cr3put(m->pml4->pa);
}

static PTE*
mmuptpget(uintptr va, int level)
{
	return (PTE*)KSEG1PTP(va, level);
}

static void
mmuput1(Proc *p, uintptr va, uintmem pa, uint pflags)
{
	Mpl pl;
	int l, x;
	PTE *pte, *ptp;
	Page *page, *prev;

	pte = nil;
	ptp = nil;
	pl = splhi();
	prev = m->pml4;
	for(l = 3; l >= 0; l--){
		x = PTLX(va, l);
		if(p == up)
			ptp = mmuptpget(va, l);
		pte = &ptp[x];
		for(page = p->mmuptp[l]; page != nil; page = page->next){
			if(page->prev == prev && page->n == x)
				break;
		}
		if(page == nil){
			if(p->mmuptp[0] == 0){
				page = mmuptpalloc();
			}
			else {
				page = p->mmuptp[0];
				p->mmuptp[0] = page->next;
			}
			page->n = x;
			page->next = p->mmuptp[l];
			p->mmuptp[l] = page;
			page->prev = prev;
			if(p == up || l != 3)
				*pte = PPN(page)|PteU|PteRW|PteP;
			if(p == up && l == 3 && x >= m->pml4->n)
				m->pml4->n = x+1;
		}
		if(p != up)
			ptp = UINT2PTR(KADDR(page->pa));
		prev = page;
	}

	*pte = pa|pflags|PteU;
//if(pa & PteRW)
//  *pte |= PteNX;
	splx(pl);

	if(p == up)
		invlpg(va);		/* only if old entry valid? */
}

/*
 * TODO: If the pg size is directly supported in HW,
 * for pgsz > PGSZ, use a single HW entry.
 */
void
mmuput(Proc *p, Page *pg, uint flags)
{
	uint pgsz, pflags;
	uintmem pa;
	uintptr va;

	pgsz = (1<<pg->pgszlg2);
	va = pg->va;
	pa = PPN(pg);
	pflags = (flags&(PGSZ-1));
	do{
		mmuput1(p, va, pa, pflags);
		va += PGSZ;
		pa += PGSZ;
		pgsz -= PGSZ;
	}while(pgsz > 0);
}

void
mmuflushpg(Page *pg)
{
	uintptr va;
	usize pgsz;
	PTE *pte;

	pgsz = 1<<pg->pgszlg2;
	va = pg->va;
	do{
		if(mmuwalk(va, 0, &pte, nil) != -1 && *pte != 0){
			*pte = 0;
			invlpg(va);
		}
		va += PGSZ;
		pgsz -= PGSZ;
	}while(pgsz > 0);
}

static PTE
pdeget(uintptr va)
{
	PTE *pdp;

	if(va < 0xffffffffc0000000ull)
		panic("pdeget(%#p)", va);

	pdp = (PTE*)(PDMAP+PDX(PDMAP)*4096);

	return pdp[PDX(va)];
}

/*
 * Add kernel mappings for pa -> va for a section of size bytes.
 * Called only after the va range is known to be unoccupied.
 */
static int
pdmap(uintmem pa, int attr, uintptr va, usize size)
{
	uintmem pae;
	PTE *pd, *pde, *pt, *pte;
	int pdx, pgsz;
	Page *pg;
	/* can't use newpage(), it's ok if it returns nil */
	extern Page *newpg(usize, int, int);

	pd = (PTE*)(PDMAP+PDX(PDMAP)*4096);

	for(pae = pa + size; pa < pae; pa += pgsz){
		pdx = PDX(va);
		pde = &pd[pdx];

		/*
		 * Check if it can be mapped using a big page,
		 * i.e. is big enough and starts on a suitable boundary.
		 * Assume processor can do it.
		 */
		if(ALIGNED(pa, PGLSZ(1)) && ALIGNED(va, PGLSZ(1)) && (pae-pa) >= PGLSZ(1)){
			assert(*pde == 0);
			*pde = pa|attr|PtePS|PteP;
			pgsz = PGLSZ(1);
		}
		else{
			if(*pde == 0){
				void *alloc;
				/*
				 * Ugly. But don't know how to do it better.
				 * Page depends on acpi for coloring, which depends on
				 * malloc, and to make things worse, acpi calls vmap,
				 * which means that there's a window during which we
				 * can only use malloc (it took the asm memory!) and
				 * can't use newpg yet.
				 */
				pg = newpg(PTSZ, -1, 1);
				if(pg == nil)
					alloc = mallocalign(PTSZ, PTSZ, 0, 0);
				else
					alloc = KADDR(pg->pa);
				if(alloc == nil)
					panic("pdmap: no pg\n");
				*pde = PADDR(alloc)|PteRW|PteP;
				memset((PTE*)(PDMAP+pdx*4096), 0, 4096);
			}
			assert(*pde != 0);

			pt = (PTE*)(PDMAP+pdx*4096);
			pte = &pt[PTX(va)];
			assert(!(*pte & PteP));
			*pte = pa|attr|PteP;
			pgsz = PGLSZ(0);
		}
		va += pgsz;
	}

	return 0;
}

static int
findhole(PTE* a, int n, int count)
{
	int have, i;

	have = 0;
	for(i = 0; i < n; i++){
		if(a[i] == 0)
			have++;
		else
			have = 0;
		if(have >= count)
			return i+1 - have;
	}

	return -1;
}

/*
 * Look for free space in the vmap.
 */
static uintptr
vmapalloc(usize size)
{
	int i, n, o;
	PTE *pd, *pt;
	int pdsz, ptsz;

	pd = (PTE*)(PDMAP+PDX(PDMAP)*4096);
	pd += PDX(VMAP);
	pdsz = VMAPSZ/PGLSZ(1);

	/*
	 * Look directly in the PD entries if the size is
	 * larger than the range mapped by a single entry.
	 */
	if(size >= PGLSZ(1)){
		n = HOWMANY(size, PGLSZ(1));
		if((o = findhole(pd, pdsz, n)) != -1)
			return VMAP + o*PGLSZ(1);
		return 0;
	}

	/*
	 * Size is smaller than that mapped by a single PD entry.
	 * Look for an already mapped PT page that has room.
	 */
	n = HOWMANY(size, PGLSZ(0));
	ptsz = PGLSZ(0)/sizeof(PTE);
	for(i = 0; i < pdsz; i++){
		if(!(pd[i] & PteP) || (pd[i] & PtePS))
			continue;

		pt = (PTE*)(PDMAP+(PDX(VMAP)+i)*4096);
		if((o = findhole(pt, ptsz, n)) != -1)
			return VMAP + i*PGLSZ(1) + o*PGLSZ(0);
	}

	/*
	 * Nothing suitable, start using a new PD entry.
	 */
	if((o = findhole(pd, pdsz, 1)) != -1)
		return VMAP + o*PGLSZ(1);

	return 0;
}

void*
vmap(uintmem pa, usize size)
{
	uintptr va;
	usize o, sz;

	DBG("vmap(%#P, %lud)\n", pa, size);

	if(m->machno != 0)
		panic("vmap");

	/*
	 * This is incomplete; the checks are not comprehensive
	 * enough.
	 * Sometimes the request is for an already-mapped piece
	 * of low memory, in which case just return a good value
	 * and hope that a corresponding vunmap of the address
	 * will have the same address.
	 * To do this properly will require keeping track of the
	 * mappings; perhaps something like kmap, but kmap probably
	 * can't be used early enough for some of the uses.
	 */
	if(pa+size < 1ull*MiB)
		return KADDR(pa);
	if(pa < 1ull*MiB)
		return nil;

	/*
	 * Might be asking for less than a page.
	 * This should have a smaller granularity if
	 * the page size is large.
	 */
	o = pa & (PGSZ-1);
	pa -= o;
	sz = ROUNDUP(size+o, PGSZ);

	if(pa == 0){
		DBG("vmap(0, %lud) pc=%#p\n", size, getcallerpc(&pa));
		return nil;
	}
	ilock(&vmaplock);
	if((va = vmapalloc(sz)) == 0 || pdmap(pa, PtePCD|PteRW, va, sz) < 0){
		iunlock(&vmaplock);
		return nil;
	}
	iunlock(&vmaplock);

	DBG("vmap(%#P, %lud) => %#p\n", pa+o, size, va+o);

	return UINT2PTR(va + o);
}

void
vunmap(void* v, usize size)
{
	uintptr va;

	DBG("vunmap(%#p, %lud)\n", v, size);

	if(m->machno != 0)
		panic("vunmap");

	/*
	 * See the comments above in vmap.
	 */
	va = PTR2UINT(v);
	if(va >= KZERO && va+size < KZERO+1ull*MiB)
		return;

	/*
	 * Here will have to deal with releasing any
	 * resources used for the allocation (e.g. page table
	 * pages).
	 */
	DBG("vunmap(%#p, %lud)\n", v, size);
}

int
mmuwalk(uintptr va, int level, PTE** ret, u64int (*alloc)(usize))
{
//alloc and pa - uintmem or PTE or what?
	int l;
	Mpl pl;
	uintptr pa;
	PTE *pte, *ptp;

	DBG("mmuwalk%d: va %#p level %d\n", m->machno, va, level);
	pte = nil;
	pl = splhi();
	for(l = 3; l >= 0; l--){
		ptp = mmuptpget(va, l);
		pte = &ptp[PTLX(va, l)];
		if(l == level)
			break;
		if(!(*pte & PteP)){
			if(alloc == nil)
				break;
			pa = alloc(PTSZ);
			if(pa == ~0)
				return -1;
if(pa & 0xfffull) print("mmuwalk pa %#llux\n", pa);
			*pte = pa|PteRW|PteP;
			if((ptp = mmuptpget(va, l-1)) == nil)
				panic("mmuwalk: mmuptpget(%#p, %d)\n", va, l-1);
			memset(ptp, 0, PTSZ);
		}
		else if(*pte & PtePS)
			break;
	}
	*ret = pte;
	splx(pl);

	return l;
}

u64int
mmuphysaddr(uintptr va)
{
	int l;
	PTE *pte;
	u64int mask, pa;

	/*
	 * Given a VA, find the PA.
	 * This is probably not the right interface,
	 * but will do as an experiment. Usual
	 * question, should va be void* or uintptr?
	 */
	l = mmuwalk(va, 0, &pte, nil);
	DBG("mmuphysaddr: va %#p l %d\n", va, l);
	if(l < 0)
		return ~0;

	mask = (1ull<<(((l)*PTSHFT)+PGSHFT))-1;
	pa = (*pte & ~mask) + (va & mask);

	DBG("mmuphysaddr: l %d va %#p pa %#llux\n", l, va, pa);

	return pa;
}

void
mmuinit(void)
{
	int l;
	uchar *p;
	PTE *pte;
	Page *page;
	uintptr pml4;
	u64int o, pa, r, sz;

	if(m->machno == 0)
		addsummary(ptpsummary, nil);

	archmmu();
	DBG("mach%d: %#p npgsz %d\n", m->machno, m, m->npgsz);
	if(m->machno != 0){
		/*
		 * GAK: Has to go when each mach is using
		 * its own page table
		 */
		p = UINT2PTR(m->stack);
		p += MACHSTKSZ;
		memmove(p, UINT2PTR(mach0pml4.va), PTSZ);
		m->pml4 = &m->pml4kludge;
		m->pml4->va = PTR2UINT(p);
		m->pml4->pa = PADDR(p);
		m->pml4->n = mach0pml4.n;	/* # of user mappings in pml4 */
		if(m->pml4->n){
			memset(p, 0, m->pml4->n*sizeof(PTE));
			m->pml4->n = 0;
		}
pte = (PTE*)p;
pte[PTLX(KSEG1PML4, 3)] = m->pml4->pa|PteRW|PteP;

		r = rdmsr(Efer);
		r |= Nxe;
		wrmsr(Efer, r);
		cr3put(m->pml4->pa);
		print("mach%d: %#p pml4 %#p\n", m->machno, m, m->pml4);
		return;
	}

	page = &mach0pml4;
	page->pa = cr3get();
	page->va = PTR2UINT(sys->pml4);

	m->pml4 = page;

	r = rdmsr(Efer);
	r |= Nxe;
	wrmsr(Efer, r);

	/*
	 * Set up the various kernel memory allocator limits:
	 * pmstart/pmend bound the unused physical memory;
	 * vmstart/vmend bound the total possible virtual memory
	 * used by the kernel;
	 * vmunused is the highest virtual address currently mapped
	 * and used by the kernel;
	 * vmunmapped is the highest virtual address currently
	 * mapped by the kernel.
	 * Vmunused can be bumped up to vmunmapped before more
	 * physical memory needs to be allocated and mapped.
	 *
	 * This is set up here so meminit can map appropriately.
	 */
	o = sys->pmstart;
	sz = ROUNDUP(o, 4*MiB) - o;
	pa = asmalloc(0, sz, 1, 0);
	if(pa != o)
		panic("mmuinit: pa %#llux memstart %#llux\n", pa, o);
	sys->pmstart += sz;

	sys->vmstart = KSEG0;
	sys->vmunused = sys->vmstart + ROUNDUP(o, 4*KiB);
	sys->vmunmapped = sys->vmstart + o + sz;
	sys->vmend = sys->vmstart + TMFM;

	print("mmuinit: vmstart %#p vmunused %#p vmunmapped %#p vmend %#p\n",
		sys->vmstart, sys->vmunused, sys->vmunmapped, sys->vmend);

	/*
	 * Set up the map for PD entry access by inserting
	 * the relevant PDP entry into the PD. It's equivalent
	 * to PADDR(sys->pd)|PteRW|PteP.
	 *
	 * Change code that uses this to use the KSEG1PML4
	 * map below.
	 */
	sys->pd[PDX(PDMAP)] = sys->pdp[PDPX(PDMAP)] & ~(PteD|PteA);
	print("sys->pd %#p %#p\n", sys->pd[PDX(PDMAP)], sys->pdp[PDPX(PDMAP)]);

	assert((pdeget(PDMAP) & ~(PteD|PteA)) == (PADDR(sys->pd)|PteRW|PteP));

	/*
	 * Set up the map for PTE access by inserting
	 * the relevant PML4 into itself.
	 * Note: outwith level 0, PteG is MBZ on AMD processors,
	 * is 'Reserved' on Intel processors, and the behaviour
	 * can be different.
	 */
	pml4 = cr3get();
	sys->pml4[PTLX(KSEG1PML4, 3)] = pml4|PteRW|PteP;
	cr3put(m->pml4->pa);

	if((l = mmuwalk(KZERO, 3, &pte, nil)) >= 0)
		print("l %d %#p %llux\n", l, pte, *pte);
	if((l = mmuwalk(KZERO, 2, &pte, nil)) >= 0)
		print("l %d %#p %llux\n", l, pte, *pte);
	if((l = mmuwalk(KZERO, 1, &pte, nil)) >= 0)
		print("l %d %#p %llux\n", l, pte, *pte);
	if((l = mmuwalk(KZERO, 0, &pte, nil)) >= 0)
		print("l %d %#p %llux\n", l, pte, *pte);

	mmuphysaddr(PTR2UINT(end));
}
