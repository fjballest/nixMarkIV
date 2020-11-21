#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"

/*
 * Page frame allocation.
 * May not call malloc() and friends.
 *
 * Takes the memory banks noted by calls to pagememinit made
 * during previous initialization of the kernel and
 * prepares for further calls to newpg (newpage).
 *
 * Banks are split so that each bank has a single color (numa).
 * There is a Pgalloc per contiguous range of pages of the
 * same color and size. Each one contains a linked list of free Page
 * structures, one per page frame available in the allocator.
 *
 * A page (larger than the smallest configured size) can 
 * be split into pages of the next (smaller) configured page size.
 * This leads to new (embedded) Pgalloc and Page structures.
 * Once all the pages from the split are free, they can be joined
 * back into a single larger page.
 *
 * For page sizes configured as preallocated, PGalloc and Page
 * structures are pre allocated to cover existing memory.
 * (For other sizes only unaligned head/trails of memory are
 * still given preallocated allocators, and others resulting from
 * split of pages are embedded).
 *
 * There are two types of embedded allocators: embed and bundle.
 * The former keeps the Pgalloc and Page structures within the
 * page being split (eg., for 4K pages within 2M pages).
 *
 * The later relies on an underlying embedded allocator that holds
 * the Page structures that can be bundled into a larger page size.
 * (eg., for 8K pages that are bundles of 4K pages).
 *
 * Without bundle allocators a 16K page split into 4K pages would
 * waste 1/4 of the memory to hold the Pgalloc for the inner 4K pages.
 *
 */

enum
{
	NOTBUNDLED = 0xFF,

	NPROCPOOLSZ = 10,	/* free pages kept on the proc for later */
};

/*
 * TODO: unify Pmem and Asm.
 */
typedef struct Pmem Pmem;
struct Pmem
{
	uintmem base;
	uintmem limit;
	int	color;
};


/*
 * Configured page sizes: 1G, 2M, 16k, 4k
 * 1G and 2M are preallocated for existing memory.
 * 16k are bundles of adjacent 4k pages embedded within 2M pages.
 *
 * - Keep sorted from larger to smaller sizes.
 * - If an entry is not PGprealloc, following ones can't be PGprealloc.
 * - The last entry can't be a bundle.
 * - The next to a bundle can't be a bundle.
 */
static Pgasz	pgasz[] = {
	{.pgszlg2 = 30, .atype = PGprealloc },
	{.pgszlg2 = 21, .atype = PGprealloc },
	{.pgszlg2 = 14, .atype = PGbundle },
	{.pgszlg2 = 12, .atype = PGembed },
};
static int npgasz = nelem(pgasz);
static Lock pgalk;
usize	mallocpgsz = 1<<21;

/* preallocated Pgallocs ang Page structures,
 * not to be confussed with free pages.
 */
static Lock freepgalk;
static Pgalloc *freepga;
static Page *freepg;
static usize nfreepga, nfreepg, nusedpga, nusedpg;

static int joinpages;	/* don't join pages split */
static int nocolors;	/* ignore colors in alloc */
static Pmem pmem[64];

static int verb=1;	/* set to make DBG more verbose */
static int testing;
static int dontfree;	/* debug */

/*
 * Take the burden of dump the allocators sorted by address, it's easy
 * the check them out this way.
 */
static Pgalloc*
pgaat(uintmem pa, int *lastp, Pgalloc *lastr)
{
	int i, n, last;
	Pgalloc *p, *r;

	r = nil;
	n = 0;
	last = *lastp;
	for(i = 0; i < npgasz; i++)
		for(p = pgasz[i].pga; p != nil; p = p->next){
			if(p != lastr)
			if(p->start > pa || (p->start == pa && n > *lastp))
				if(r == nil || p->start < r->start){
					last = n;
					r = p;
				}
			n++;
		}
	if(r != nil && r->start > pa && *lastp >= 0)
		*lastp = -1;
	else
		*lastp = last;
	return r;
}

static void
pgacheck(Pgalloc *p)
{
	uint pgsz, align;
	uint nfree;
	Page *pg;
	int once, ronce;

	pgsz = pgasz[p->szi].pgsz;
	if(p->nfree > p->npg || p->npg == ~0)
		print("pgacheck: bad count\n");
	nfree = 0;
	once = 0;
	ronce = 0;
	align = pgsz-1;
	for(pg = p->free; pg != nil; pg = pg->next){
		if(pg->pa < p->start || pg->pa+pgsz > p->start+p->npg*pgsz)
			print("off page %#P\n", pg->pa);
		if(pgsz != 1<<pg->pgszlg2)
			print("bad page %#P pgszlg2 %d\n", pg->pa, pg->pgszlg2);
		if(pg->pga != p)
			print("bad pga in page %#P\n", pg->pa);
		if((pg->pa & align) != 0 && once++ == 0)
			print("unaligned pages: first at %#P\n", pg->pa);
		if(pg->ref != 0 && ronce++ == 0)
			print("referenced free pages\n");
		if(pg == p->free && pg->prev != nil)
			print("bad prev %#P\n", pg->pa);
		if(pg->prev != nil && pg->prev->next != pg)
			print("bad back link %#P\n", pg->pa);
		if(pg->next != nil && pg->next->prev != pg)
			print("bad fwd link %#P\n", pg->pa);
		nfree++;
	}
	if(nfree != p->nfree)
		print("bad free count: found %ud expected %uld\n", nfree, p->nfree);
}

static char*
seprintsz(char *s, char *e, usize sz)
{
	if(sz < KiB)
		return seprint(s, e, "%uld", sz);
	if(sz < MiB)
		return seprint(s, e, "%uldK", sz/KiB);
	if(sz < GiB)
		return seprint(s, e, "%uldM", sz/MiB);
	return seprint(s, e, "%uldG", sz/GiB);
}

static void
dumppga(Pgalloc *p)
{
	int szi;
	uint pgsz;
	static char sz[12];
	static char *tn[] = {
		"alloc ",
		"embed ",
		"bundle"
	};

	szi = p->szi;
	pgsz = pgasz[szi].pgsz;
	seprintsz(sz, sz+sizeof sz, pgsz);
	print("pga sz %s start %#P end %#P %s %s col %d"
		"\tfree %3uld used %3uld\n",
		sz, p->start, p->start+p->npg*pgsz,
		tn[pgasz[szi].atype],
		p->parent != nil ? "dyna." : "fixed",
		p->color, p->nfree, p->npg - p->nfree);
	pgacheck(p);
}

static void
pgadump(void*)
{
	Pgalloc *p;
	uintmem pa;
	int n;

	n = -1;
	p = nil;
	for(pa = 0; (p = pgaat(pa, &n, p)) != nil; pa = p->start)
		dumppga(p);
}

void
pagememinit(uintmem base, uintmem limit)
{
	int i;

	for(i = 0; i < nelem(pmem); i++)
		if(pmem[i].base == 0 && pmem[i].limit == 0){
			pmem[i].base = base;
			pmem[i].limit = limit;
			return;
		}
	print("pagememinit: losing %P KiB\n", (limit-base)/KiB);

}

static char*
pagesummary(char *s, char *e, void*)
{
	int i;
	usize npg, nfree, nuser, nbundled, nsplit;
	Pgalloc *pga;
	char sz[30];

	s = seprint(s, e, "%llud memory\n", sys->pmoccupied);
	s = seprint(s, e, "%llud kernel\n",
		ROUNDUP(sys->vmend - KTZERO, PGSZ));
	for(i = 0; s < e && i < npgasz; i++){
		ilock(&pgalk);
		npg = 0;
		nfree = 0;
		nuser = 0;
		nbundled = 0;
		nsplit = 0;
		for(pga = pgasz[i].pga; pga != nil; pga = pga->next){
			npg += pga->npg;
			nfree += pga->nfree;
			nuser += pga->nuser;
			nbundled += pga->nbundled;
			nsplit += pga->nsplit;
		}
		iunlock(&pgalk);
		seprintsz(sz, sz + sizeof sz, pgasz[i].pgsz);
		npg -= nbundled+nsplit;
		s = seprint(s, e, "%lud/%lud %s pages", npg-nfree, npg, sz);
		s = seprint(s, e, " %lud user %uld kernel %uld bundled %uld split\n",
			nuser, npg-nfree-nuser, nbundled, nsplit);
	}
	s = seprint(s, e, "%lud/%lud pgas\n", nusedpga, nusedpga+nfreepga);
	s = seprint(s, e, "%lud/%lud pgs\n", nusedpg, nusedpg+nfreepg);
	return s;
}

/* doesn't lock */
static Page*
pganewpg(Pgalloc *pga)
{
	Page *pg;

	if(pga->free == nil)
		return nil;
	pg = pga->free;
	if(pg != nil){
		pga->free = pg->next;
		if(pg->next != nil)
			pg->next->prev = nil;
		pg->next = nil;
		pga->nfree--;
		pg->ref = 1;
	}
	return pg;
}

/* doesn't lock */
static void
pgaunfree(Pgalloc *pga, Page *pg)
{
	if(pg->prev != nil)
		pg->prev->next = pg->next;
	else
		pga->free = pg->next;
	if(pg->next != nil)
		pg->next->prev = pg->prev;
	pg->next = nil;
	pg->prev = nil;
	pga->nfree--;
}

/* helper for allocpga: add pg at the end of the free list
 * doesn't lock
 */
static Page*
pgafreeapp(Pgalloc *pga, Page *pgl, Page *pg)
{
	pg->prev = pgl;
	if(pgl != nil)
		pgl->next = pg;
	else
		pga->free = pg;
	pga->nfree++;
	return pg;
}

/* doesn't lock */
static void
pgafreepg(Pgalloc *pga, Page *pg)
{
	pg->next = pga->free;
	pga->free = pg;
	if(pg->next != nil)
		pg->next->prev = pg;
	pga->nfree++;
}

static void
linkpga(Pgalloc *pga, int first)
{
	if(pga->next != nil || pga->prev != nil)
		panic("linkpga: already linked");
	if(first){
		pga->next = pgasz[pga->szi].pga;
		if(pga->next != nil)
			pga->next->prev = pga;
		else
			pgasz[pga->szi].last = pga;
		pgasz[pga->szi].pga = pga;
		return;
	}
	pga->prev = pgasz[pga->szi].last;
	if(pga->prev != nil)
		pga->prev->next = pga;
	else
		pgasz[pga->szi].pga = pga;
	pgasz[pga->szi].last = pga;
}

static void
unlinkpga(Pgalloc *pga)
{
	if(pga->prev != nil)
		pga->prev->next = pga->next;
	else
		pgasz[pga->szi].pga = pga->next;
	if(pga->next != nil)
		pga->next->prev = pga->prev;
	else
		pgasz[pga->szi].last = pga->prev;
	pga->next = nil;
	pga->prev = nil;
}

/* doesn't lock */
static Pgalloc*
initbundle(Pgalloc *pga, Pgalloc *cpga)
{
	Page *pg, *pgl;
	usize npg, bsz, pgsz, cpgsz;
	int szi, i;
	uintmem end;

	if(pga->free != nil)
		panic("initbundle: pga free");
	if(cpga->bpga != nil)
		panic("initbundle: already has one");
	cpga->bpga = pga;

	szi = pga->szi;
	pgsz = pgasz[szi].pgsz;
	cpgsz = pgasz[szi+1].pgsz;
	bsz = pgsz / cpgsz;

	/* locate first aligned page */
	end = cpga->start + cpga->npg*cpgsz;
	pga->start = ROUNDUP(cpga->start, pgsz);
	if(pga->start >= end)
		panic("initbundle: no aligned pages");
	pga->pg0 = cpga->pg0 + (pga->start - cpga->start)/cpgsz;
	if(pga->pg0->pa != pga->start)
		panic("initbundle: free list pa bug");
	for(pg = cpga->pg0; pg < pga->pg0; pg++)
		pg->bundlei = NOTBUNDLED;

	/* take all aligned bundles from the child */
	pga->npg = (cpga->npg - (pga->pg0 - cpga->pg0)) / bsz;
	if(pga->npg == 0)
		panic("initbundle: no aligned bundles");
	pgl = nil;
	pg = pga->pg0;
	for(npg = 0; npg < pga->npg; npg++){
		for(i = 0; i < bsz; i++){
			assert(i == bsz-1 || pg->next == pg+1);
			pgaunfree(cpga, pg);
			cpga->nbundled++;
			pg->bundlei = i;
			pg->ref = 0;
			assert(i == ((pg - pga->pg0) % bsz));
			if(i == 0){
				pg->pga = pga;
				pg->pgszlg2 = pgasz[szi].pgszlg2;
				pgl = pgafreeapp(pga, pgl, pg);
				if(pg->pa < pga->start || pg->pa+pgsz > end){
					dumppga(pga);
					panic("initbundle %#P", pg->pa);
				}
			}
			pg++;
		}
	}

	/* ignore for bundles the trail of unaligned pages */
	for(; pg < cpga->pg0+cpga->npg; pg++)
		pg->bundlei = NOTBUNDLED;

	if(pga->nfree != pga->npg)
		panic("initbundle: unaligned pages");
	return pga;
}

static Pgalloc*
allocpga(uintmem start, uintmem end, int color, int szi, Page *parent)
{
	Pgalloc *pga, **pgal, *cpga;
	Page *pg, *pgl;
	usize pgsz, npg;

	pgsz = pgasz[szi].pgsz;
	npg = (end-start)/pgsz;

	/* for top-level bundles, alloc the underlying allocator
	 * because it holds the page structures.
	 * The caller will link it later.
	 */
	if(pgasz[szi].atype == PGbundle && parent == nil)
		cpga = allocpga(start, start+npg*pgsz, color, szi+1, nil);
	else
		cpga = nil;

	if(pgasz[szi].atype != PGprealloc && parent != nil)
		panic("allocpga called from %#p", getcallerpc(&start));

	ilock(&freepgalk);
	for(pgal = &freepga; (pga = *pgal) != nil; pgal = &pga->next)
		if(pga->npg == 0 || pga->npg == npg){
			*pgal = pga->next;
			pga->next = nil;
			nfreepga--;
			nusedpga++;
			if(pga->npg != pga->nfree)
				panic("allocpga: bad nfree");
			break;
		}
	if(pga == nil)
		panic("newpga: bug: too few pgas preallocated");
	pga->start = start;
	pga->color = color;
	pga->szi = szi;
	pga->parent = parent;

	if(cpga != nil)
		initbundle(pga, cpga);
	else if(pga->npg == npg){
		npg = 0;
		for(pg = pga->free; pg != nil; pg = pg->next){
			pg->pa = start;
			start += pgsz;
			npg++;
		}
	}else{
		pgl = nil;
		pga->pg0 = freepg;
		for(; pga->npg < npg; pga->npg++){
			pg = freepg;
			if(freepg == nil)
				panic("newpga: bug: too few pgs preallocated");
			freepg = pg->next;
			pg->next = nil;
			pgl = pgafreeapp(pga, pgl, pg);
			nfreepg--;
			nusedpg++;
			pg->pga = pga;
			pg->pgszlg2 = pgasz[szi].pgszlg2;
			pg->pa = start;
			start += pgsz;
		}
	}
	if(pga->nfree != pga->npg || pga->npg != npg)
		panic("allocpga: npg");
	iunlock(&freepgalk);
	return pga;
}

/*
 * Used by pgainit to count pgallocs and pages needed initially.
 * 1 Pgalloc per unaligned head/trail of smaller pages and aligned
 * bank on pages (per call to countpga).
 * 1 Page per page in each such pgalloc.
 * If the next smaller page size also preallocated, we must
 * recur for each page.
 * If it's a bundle, we must count 1 Pgalloc for the underlying
 * alloc and the underlying pages instead of the bundle pages.
 */
static void
countpga(uintmem start, uintmem end, int i)
{
	usize pgsz;

	nfreepga++;
	pgsz = pgasz[i].pgsz;

	if(pgasz[i].atype == PGbundle){
		nfreepga++;
		nfreepg += HOWMANY(end-start, pgasz[i+1].pgsz);
		return;
	}
		nfreepg += HOWMANY(end-start, pgsz);
	if(i < npgasz-1 && pgasz[i+1].atype == PGprealloc)
		for(; start < end; start += pgsz)
			countpga(start, start+pgsz, i+1);
}

static Pgalloc*
newpga(uintmem start, uintmem end, int color, int szi, Page *parent)
{
	Pgalloc *pga;

	DBG("newpga %#p %#p %d parent %p\n", start, end, szi, parent?parent->pa:0);
	if(pgasz[szi].atype != PGprealloc && parent != nil)
		panic("newpga: not PGprealloc");
	pga = allocpga(start, end, color, szi, parent);
	ilock(&pgalk);
	linkpga(pga, parent != nil);
	if(pgasz[szi].atype == PGbundle && parent == nil)
		linkpga(pga->pg0[1].pga, parent != nil);
	iunlock(&pgalk);
	return pga;
}

static Pgalloc*
newepga(uintmem start, uintmem end, int color, int szi, Page *parent, int locked)
{
	Pgalloc *pga, *cpga;
	Page *pg, *pgl;
	usize npg, pgsz;
	uintmem nstart;

	print("newepga %#p %#p %d parent %p\n", start, end, szi, parent->pa);
	if(pgasz[szi].atype == PGprealloc)
		panic("newepga: PGprealloc");
	if(parent == nil)
		panic("newepga: no parent");
	pgsz = pgasz[szi].pgsz;
	pga = KADDR(start);
	start += sizeof *pga;
	memset(pga, 0, sizeof *pga);
	pga->color = color;
	pga->szi = szi;
	pga->parent = parent;
	if(pgasz[szi].atype == PGbundle){
		ilock(&pgalk);
		cpga = newepga(start, end, color, szi+1, parent, 1);
		initbundle(pga, cpga);
		iunlock(&pgalk);
	}else{
		pga->pg0 = (Page*)(&pga[1]);
		npg = (end-start)/pgsz;
		start += npg * sizeof *pg;
		nstart = ROUNDUP(start, pgsz);
		pga->start = nstart;
		print("newepga: lost %ulld bytes due to embedding\n", nstart - start);
		start = nstart;
		if(start >= end)
			panic("newepga: bug");
		pga->npg = (end-start)/pgasz[szi].pgsz;
		memset(pga->pg0, 0, pga->npg * sizeof *pg);
		pgl = nil;
		for(npg = 0; npg < pga->npg; npg++){
			pga->pg0[npg].pga = pga;
			pga->pg0[npg].pgszlg2 = pgasz[pga->szi].pgszlg2;
			pga->pg0[npg].pa = start;
			pgl = pgafreeapp(pga, pgl, &pga->pg0[npg]);
			start += pgsz;
		}
	}
	if(testing)
		dumppga(pga);
	if(!locked)
		ilock(&pgalk);
	linkpga(pga, parent != nil);
	if(!locked)
		iunlock(&pgalk);

	return pga;
}

enum{Init, Count};
static void
pgainit(uintmem start, uintmem end, int color, int count)
{
	int i;
	uintmem s, e, pgsz;

	for(i = 0; i < npgasz; i++){
		pgsz = pgasz[i].pgsz;
		s = ROUNDUP(start, pgsz);
		e = ROUNDDN(end, pgsz);
		if(e > s){
			DBG("pgainit %d start %#P s %#P e %#P end %#P\n",
				i, start, s, e, end);
			if(s > start)
				pgainit(start, s, color, count);
			if(count)
				countpga(s, e, i);
			else
				newpga(s, e, color, i, nil);
			if(e < end)
				pgainit(e, end, color, count);
			break;
		}
	}
}

static void
preallocpgas(void)
{
	Pmem *pm;
	usize tot;
	Pgalloc *pga;

	pm = &pmem[0];
	for(tot = 0; tot < nfreepga; tot++){
		if(pm->limit - pm->base < sizeof *pga){
			pm->base = pm->limit;
			break;
		}
		pga = KADDR(pm->base);
		memset(pga, 0, sizeof *pga);
		pm->base += sizeof *pga;
		pga->next = freepga;
		freepga = pga;
	}
	if(tot < nfreepga){
		/*
		 * If this happens, the fix is also to ensure that
		 * the start of the second bank (if any) is
		 * misaligned and continue with pm = &pmem[1].
		 * By now we assume structures fit in the first bank.
		 */
		panic("preallocpgas: bank 0 exhausted");
	}
}

static void
preallocpgs(void)
{
	Pmem *pm;
	usize tot;
	Page *pg, **pgl;

	pm = &pmem[0];
	pgl = &freepg;
	for(tot = 0; tot < nfreepg; tot++){
		if(pm->limit - pm->base < sizeof *pg){
			pm->base = pm->limit;
			break;
		}
		pg = KADDR(pm->base);
		memset(pg, 0, sizeof *pg);
		pm->base += sizeof *pg;
		*pgl = pg;
		pgl = &pg->next;
	}
	if(tot < nfreepg)
		panic("preallocpgs: bank 0 exhausted");
}

/*
 * Split palloc.mem[i] if it's not all of the same color and we can.
 * Return the new end of the known banks.
 */
static int
splitbank(int i, int e)
{
	Pmem *pm;
	uintmem psz;

	if(e == nelem(pmem))
		return 0;
	pm = &pmem[i];
	pm->color  = memcolor(pm->base, &psz);
	if(pm->color < 0){
		pm->color = 0;
		if(i > 0)
			pm->color = pm[-1].color;
		return 0;
	}

	if(psz <= PGSZ || psz >= (pm->limit - pm->base))
		return 0;
	if(i+1 < e)
		memmove(pm+2, pm+1, (e-i-1)*sizeof(Pmem));
	pm[1].base = pm->base + psz;
	pm[1].limit = pm->limit;
	pm->limit = pm[1].base;
	DBG("palloc split[%d] col %d %#P %#P -> %#P\n",
		i, pm->color, pm->base, pm[1].limit, pm->limit);

	return 1;
}

/*
 * Could try to be clever and decide here depending on the MMU
 * sizes, but it's easy for the clever code to make mistakes
 * causing internal fragmentation in embedded structures (which
 * consume at least one of the inner pages).
 * Sizes are statically configured in this version.
 */
static void
pgsizes(void)
{
	int i;
	Pgasz *p;

	DBG("Pgalloc %d bytes Page %d bytes\n", sizeof(Pgalloc), sizeof(Page));
	for(i = 0; i < npgasz; i++){
		p = &pgasz[i];
		p->pgsz = 1<<p->pgszlg2;
		DBG("setpgsizes %uld\n", p->pgsz);
	}
}

/*
 * Test that we can allocate and free everything with page splits and joins
 */
Page* newpg(usize sz, int color, int iskern);
static void pgfree(Page *pg);


static void
testpga(void)
{
	enum{Limit = 150000};
	Page **pgs, **oldpgs;
	Page *pg;
	int i;
	uvlong tot;

	oldpgs = nil;
	tot = 0;
	testing = 1;
	print("before alloc:\n");
	pgadump(nil);
	for(;;){
		pg = newpg(PGSZ, 0, 0);
		if(pg == nil)
			break;
		tot++;
		pgs = UINT2PTR(KADDR(pg->pa));
		memset(pgs, 0, PGSZ);
		pgs[0] = pg;
		pgs[1] = (Page*)oldpgs;
		oldpgs = pgs;
		for(i = 2; i < PGSZ/(sizeof pg); i++){
			pgs[i] = newpg(PGSZ, 0, 0);
			if(pgs[i] == nil)
				goto done;
			if(tot++ == Limit)
				goto done;
			if(tot%1000 == 0)
				print("%ulld pages\n", tot);
		}
	}
done:
	print("\nafter alloc:\n");
	pgadump(nil);
	print("%ulld pages allocated\n", tot);
	tot = 0;
	joinpages = 1;
	for(pgs = oldpgs; pgs != nil; pgs = oldpgs){
		for(i = 2; i < PGSZ/(sizeof pg); i++){
			if(pgs[i] == nil)
				break;
			pgs[i]->ref = 0;	/* putpage() */
			pgfree(pgs[i]);
			tot++;
		}
		oldpgs = (Page**)pgs[1];
		pgs[0]->ref = 0;	/* putpage() */
		pgfree(pgs[0]);
		tot++;
	}
	print("\nafter free:\n");
	pgadump(nil);
	print("%ulld pages free\n", tot);
	for(;;)
		halt();
}

void
pageinit(void)
{
	int e, i;
	Pmem *pm;
	usize minsz;
	uintmem obase;
	char *s;

	addsummary(pagesummary, nil);

	for(e = 0; e < nelem(pmem); e++){
		if(pmem[e].base == pmem[e].limit)
			break;
	}
	DBG("pageinit e %d\n", e);

	/*
	 * Split banks if not of the same color
	 * and the array can hold another item.
	 */
	for(i = 0; i < e; i++){
		pm = &pmem[i];
		if(splitbank(i, e))
			e++;
		DBG("palloc[%d] col %d %#P %#P\n",
			i, pm->color, pm->base, pm->limit);
	}

	pgsizes();
	if((s=getconf("*joinpages")) != nil)
		joinpages = atoi(s);
	if((s=getconf("*nocolors")) != nil)
		nocolors = atoi(s);
	/*
	 * We allocate structures in the first bank,
	 * which makes its start unlikely to be aligned, which would require
	 * more structures.
	 * Temporarily move the base of the first bank so it's the worst case.
	 * We could do better, by adjusting the start and computing the new
	 * number of structures needed until we reach a fixed point, but
	 * this is good enough at the expense of some memory loss.
	 */
	minsz = pgasz[npgasz-1].pgsz;
	obase = pmem[0].base;
	if((pmem[0].base & minsz) == 0)
		pmem[0].base += minsz;

	for(i = 0; i < e; i++)
		pgainit(pmem[i].base, pmem[i].limit, pmem[i].color, Count);
	DBG("%uld nfreepga %uld nfreepg %uld bytes\n",
		nfreepga, nfreepg, nfreepga*sizeof(Pgalloc) + nfreepg*sizeof(Page));

	pmem[0].base = obase;
	preallocpgas();
	preallocpgs();
	for(i = 0; i < e; i++)
		pgainit(pmem[i].base, pmem[i].limit, pmem[i].color, Init);

	DBG("%uld nfreepga %uld nfreepg %uld bytes\n",
		nfreepga, nfreepg, nfreepga*sizeof(Pgalloc) + nfreepg*sizeof(Page));
	if(DBGFLG)
		pgadump(nil);
	addttescape('a', pgadump, nil);

	if(getconf("*testpage"))
		testpga();
}

static int
pgreclaim(usize sz)
{
	/*
	 * TODO: reclaim segments to release pages.
	 */
	iprint("pgreclaim %uld\n", sz);
	return -1;
}

static Page*
splitbundle(Page *pg)
{
	Pgalloc *pga, *cpga;
	Page *cpg;
	int szi, i, bsz;

	if(verb)DBG("splitbundle pg %#P\n", pg->pa);
	if(pg->bundlei != 0)
		panic("splitbundle: not a bundle");
	pga = pg->pga;
	cpga = pg[1].pga;
	szi = pga->szi;
	if(szi != cpga->szi-1)
		panic("splitbundle: bad bundle szi");
	bsz = pgasz[szi].pgsz/pgasz[szi+1].pgsz;
	ilock(&pgalk);
	cpga->nbundled -= bsz;
	for(i = 1; i < bsz; i++){
		cpg = pg+i;
		if(cpg->bundlei != i || cpg->pga != cpga || cpg->next || cpg->prev)
			panic("splitbundle: bad bundled page");
		if(cpg->ref != 0)
			panic("splitbundle: cpg ref %d i %d", cpg->ref, i);
		pgafreepg(cpga, cpg);
	}
	pg->pga = cpga;
	pg->pgszlg2 = pgasz[szi+1].pgszlg2;
	iunlock(&pgalk);
	return pg;
}

Page*
newpg(usize sz, int color, int iskern)
{
	int i;
	Pgalloc *pga, *ppga;
	Page *pg;
	usize pgsz;

	/* Find the smallest page already available */
	for(i = npgasz-1; i >= 0; i--){
		if(pgasz[i].pgsz < sz)
			continue;
		ilock(&pgalk);
		for(pga = pgasz[i].pga; pga != nil; pga = pga->next){
			if(color >= 0 && pga->color != color)
				continue;
			pg = pganewpg(pga);
			if(pg != nil){
				/* Move pga to head if not yet there */
				if(pga != pgasz[i].pga){
					unlinkpga(pga);
					linkpga(pga, 1);
				}
				iunlock(&pgalk);
				goto found;
			}
			
		}
		iunlock(&pgalk);
	}
	DBG("newpg: no pages: sz %uld\n", sz);
	return nil;
found:
	/*
	 * If the page is larger than we asked for, split it into
	 * smaller pages until we get the desired size.
	 */
	if(verb>1)
	DBG("newpg %#P pgsz %#ulx for %#ulx\n", pg->pa, pgasz[i].pgsz, sz);

	for(; pgasz[i].pgsz > sz && i < npgasz-1; i++){
		pgsz = pgasz[i].pgsz;
		if(pgasz[i].atype == PGbundle){
			pg = splitbundle(pg);
			continue;
		}
		print("newpg split %#p\n", pg->pa);	// was DBG
		ppga = pga;
		switch(pgasz[i+1].atype){
		case PGprealloc:
			pga = newpga(pg->pa, pg->pa + pgsz, pga->color, i+1, pg);
			break;
		case PGbundle:
		case PGembed:
			pga = newepga(pg->pa, pg->pa + pgsz, pga->color, i+1, pg, 0);
			break;
		default:
			pga = nil;
			panic("newpg: atype");
		}
		ilock(&pgalk);
		ppga->nsplit++;
		pg = pganewpg(pga);
		iunlock(&pgalk);
	}
	if(pg == nil)
		return nil;
	if(!iskern){
		ilock(&pgalk);
		pg->pga->nuser++;
		iunlock(&pgalk);
	}
	if(i == npgasz || pgasz[i].pgsz != sz || (1<<pg->pgszlg2) != sz)
		panic("newpg: sizes");
	if(pg->pa < pg->pga->start || pg->pa+sz > pg->pga->start+pg->pga->npg*sz)
		panic("newpg: %#P off limits", pg->pa);
	if(pg->n != 0)
		panic("newpg: n");
	if(pg->ref != 1)
		panic("newpg: ref %d", pg->ref);
	return pg;
}

Page*
newpage(uint pgsz, int color, int clear, uintptr va)
{
	Page *pg;
	KMap *k;

	if(pgsz == 0)
		pgsz = PGSZ;
	/*
	 * If there are pages on the per process pool, use them.
	 */
	pg = nil;
	if(up != nil && pgsz == UPGSZ && up->pgfree != nil){
		if(canlock(&up->selfishlk)){
			if(up->pgfree != nil){
				pg = up->pgfree;
				up->pgfree = pg->next;
				pg->next = nil;
				pg->ref = 1;
				up->npgfree--;
			}
			unlock(&up->selfishlk);
			if(pg != nil){
				if(verb)DBG("new proc page %#p -> %#p src %#p\n",
					va, pg->pa, getcallerpc(&pgsz));
				goto found;
			}
		}
	}
	/*
	 * Try first with the desired color, then with any color,
	 * and finally with any color after reclaiming memory.
	 */
	if(nocolors)
		color = -1;
	pg = newpg(pgsz, color, va == 0);
	if(pg == nil && color >= 0)
		pg = newpg(pgsz, -1, va == 0);

	while(pg == nil && pgreclaim(pgsz) == 0)
		pg = newpg(pgsz, -1, va == 0);
	if(pg == nil)
		panic("no free pages");
	if(verb)DBG("newpage %#p -> %#p src %#p\n",
		va, pg->pa, getcallerpc(&pgsz));
found:
	pg->va = va;
	if(clear){
		k = kmap(pg);
		memset(UINT2PTR(VA(k)), 0, pgsz);
		kunmap(k);
	}
	if(pg->ref != 1)
		panic("newpage: ref");
	return pg;
}

static Page*
joinbundle(Page *pg)
{
	Pgalloc *pga, *bpga;
	Page *bpg;
	int szi, bsz, i;

	pga = pg->pga;
	szi = pga->szi;
	bpga = pga->bpga;
	if(szi == 0 || bpga == nil || pg->bundlei == NOTBUNDLED)
		return nil;
	bsz = pgasz[szi-1].pgsz/pgasz[szi].pgsz;
	/*
	 * The alloc underlying a bundle doesn't have to have
	 * a multiple of bundelsz pages.
	 */
	if(pg->pa >= bpga->start+bpga->npg*pgasz[szi-1].pgsz)
		return nil;

	bpg = pg - pg->bundlei;
	if(bpg < bpga->pg0 || bpg >= bpga->pg0+bpga->npg)
		panic("canjoinbundle: out of range");
	if(bpg->bundlei != 0 || bpg->pga != pga)
		panic("canjoinbundle: bad bundle");
	for(i = 0; i < bsz; i++)
		if(bpg[i].ref != 0)
			return nil;

	if(verb)DBG("pgfree: join bundle %#P\n", bpg->pa);
	
	for(i = 0; i < bsz; i++){
		pga->nbundled++;
		pgaunfree(pga, &bpg[i]);
	}
	bpg->pga = pga->bpga;
	bpg->pgszlg2 = pgasz[szi-1].pgszlg2;

	if(testing){
		pgacheck(pga);
		pgacheck(pga->bpga);
	}

	return bpg;
}

static void
pgfree(Page *pg)
{
	Pgalloc *pga;
	Page *ppg;
	int szi;
	usize pgsz, bsz;

	if(verb)DBG("pgfree pg %#p\n", pg->pa);
	if(pg->next || pg->prev)
		panic("pgfree: linked");
	if(pg->ref)
		panic("pgfree: %#p referenced", pg->pa);
	if(dontfree)
		return;

	pg->n = 0;
	pga = pg->pga;
	szi = pga->szi;
	pgsz = pgasz[szi].pgsz;
	if(pg->pa < pga->start || pg->pa+pgsz > pga->start+pga->npg*pgsz)
		panic("pgfree: %#P off limits", pg->pa);
	ilock(&pgalk);
	if(pg->va != 0)
		pga->nuser--;
	pg->va = 0;
	pgafreepg(pga, pg);
	if((pga->parent == nil && pga->bpga == nil) || joinpages == 0){
		iunlock(&pgalk);
		return;
	}

	/*
	 * If part of a bundle page, try to join the bundled page and free it.
	 */
	ppg = joinbundle(pg);
	iunlock(&pgalk);
	if(ppg != nil){
		ppg->ref = 0;
		pgfree(ppg);
		return;
	}

	/*
	 * If this is the last page in use try to free the parent
	 * page we are living within.
	 */
	if(pga->nfree < pga->npg || joinpages == 0)
		return;

	/*
	 * For PGprealloc just release the structure and the parent
	 * page after removing the pga from the list and making it free.
	 *
	 * For PGembed we know there is no PGbundle built upon it, because
	 * free pages would be joined into bundles in such case, so
	 * we can just unlink the allocator and free the parent page.
	 *
	 * For PGbundle, the underlying PGembed has free all the pages we
	 * are using for the bundle, but there may be a few pages not
	 * bundled. If it's ok and everything is free,
	 * remove pga and the underlying PGembed from the list and free the
	 * page we are living within.
	 * If bundles were directly nested this should iterate.
	 */
	ilock(&pgalk);
	if(pgasz[szi].atype == PGbundle){
		bsz = pgasz[szi].pgsz/pgasz[szi+1].pgsz;
		if(pga->nfree*bsz + pga->pg0[1].pga->nfree < pga->pg0[1].pga->npg){
			iunlock(&pgalk);
			return;
		}
	}else if(pga->nfree < pga->npg){
		iunlock(&pgalk);
		return;
	}
	ppg = pga->parent;
	print("pgfree: join %#p\n", ppg->pa);	// was DBG
	pga->parent = nil;
	ppg->pga->nsplit--;
	unlinkpga(pga);
	if(pgasz[szi].atype == PGprealloc || pgasz[szi].atype == PGembed){
		iunlock(&pgalk);
		if(pgasz[szi].atype == PGprealloc){
			lock(&freepgalk);
			/* pages are kept in pga->free for the next time */
			pga->next = freepga;
			freepga = pga;
			nfreepga++;
			nusedpga--;
			iunlock(&freepgalk);
		}
		ppg->ref = 0;
		pgfree(ppg);
		return;
	}

	if(pga->pg0[1].pga->bpga != pga)
		panic("pgfree: bpga");
	pga = pga->pg0[1].pga;
	unlinkpga(pga);
	iunlock(&pgalk);
	ppg->ref = 0;
	pgfree(ppg);
}

void
putpage(Page *pg)
{
	if(decref(pg) > 0)
		return;
	if(up != nil && pg->pgszlg2 == UPGSHFT && up->npgfree < NPROCPOOLSZ)
	if(canlock(&up->selfishlk)){
		pg->n = 0;
		pg->va = 0;
		pg->next = up->pgfree;
		up->pgfree = pg;
		up->npgfree++;
		unlock(&up->selfishlk);
		return;
	}
	pgfree(pg);
}

void
pagecpy(Page *t, Page *f)
{
	KMap *ks, *kd;

	if(f->pgszlg2 != t->pgszlg2)
		panic("pagecpy");
	ks = kmap(f);
	kd = kmap(t);
	memmove((void*)VA(kd), (void*)VA(ks), 1<<t->pgszlg2);
	kunmap(ks);
	kunmap(kd);
	t->n = f->n;
}
