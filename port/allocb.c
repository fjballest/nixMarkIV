#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"

typedef struct Iac Iac;

enum
{
	Hdrspc		= 64,		/* leave room for high-level headers */
	Bdead		= 0x51494F42,	/* "QIOB" */

	/* blocks larger than this are round to a multiple of it */
	BLOCKROUND	= KiB,
	/* but blocks smaller are rounded to this other size */
	BLOCKMINROUND	= 128,
};

struct Iac
{
	uint	sz;
	uint	n;
};

/* set to n to track the last n allocation sizes */
#define DEBUGALLOC 1024

struct
{
	Lock;
	ulong	bytes;
	ulong	limit;
} ialloc;

static void* baalloc(void);

Nalloc balloc =
{
	"block",
	BLOCKALIGN + ROUNDUP(BLOCKMINROUND+Hdrspc, BLOCKALIGN) + sizeof(Block),
	Selfblock,
	10,
	nil,		/* init */
	nil,		/* term */
	baalloc,
};

static Lock iaclk;
static Iac iac[DEBUGALLOC+1];
int debugialloc;

static void
track(uint sz)
{
	int i;

	ilock(&iaclk);
	for(i = 0; i < nelem(iac); i++)
		if(iac[i].sz == 0 || iac[i].sz == sz){
			iac[i].sz = sz;
			iac[i].n++;
			break;
		}
	iunlock(&iaclk);
}

char*
iallocsummary(char *s, char *e, void*)
{
	s = nasummary(s, e, &balloc);
	return seprint(s, e, "%lud/%lud ialloc bytes\n", ialloc.bytes, ialloc.limit);
}

void
iallocdump(void*)
{
	int i;
	int old;

	old = debugialloc;
	debugialloc = 0;
	for(i = 0; i < nelem(iac); i++)
		if(iac[i].sz != 0)
			print("ialloc %ud bytes %ud times\n", iac[i].sz, iac[i].n);
	memset(iac, 0, sizeof iac);
	debugialloc = !old;
	print("debugialloc %d\n", debugialloc);
}

static void*
baalloc(void)
{
	Block *b;
	uchar *p;

	p = malloc(balloc.elsz);
	if(p == nil)
		panic("balloc: no memory");
	b = (Block*)(p + balloc.elsz - sizeof(Block));	/* at end of allocated space */
	/* align base and bounds of data */
	b->lim = (uchar*)(PTR2UINT(b) & ~(BLOCKALIGN-1));
	b->base = p;
	return b;
}

static Block*
_allocb(int size, uintptr tag)
{
	Block *b;
	uchar *p;
	int n;


	if(size>BLOCKROUND)
		size=ROUNDUP(size, BLOCKROUND);
	else
		size=ROUNDUP(size, BLOCKMINROUND);
	if(debugialloc)
		track(size);

	if(size == BLOCKMINROUND)
		b = nalloc(&balloc);
	else{
		n = BLOCKALIGN + ROUNDUP(size+Hdrspc, BLOCKALIGN) + sizeof(Block);
		if((p = malloc(n)) == nil)
			panic("allocb: no memory");
		b = (Block*)(p + n - sizeof(Block));	/* end of allocated space */
		/* align base and bounds of data */
		b->lim = (uchar*)(PTR2UINT(b) & ~(BLOCKALIGN-1));
		b->base = p;
	}
	setmalloctag(b->base, tag);
	b->flag = 0;
	if(size == BLOCKMINROUND)
		b->flag = BSMALL;
	b->next = nil;
	b->list = nil;
	b->free = nil;
	b->checksum = 0;

	/* align start of writable data, leaving space below for added headers */
	b->rp = b->lim - ROUNDUP(size, BLOCKALIGN);
	b->wp = b->rp;

	if(b->rp < b->base || b->lim - b->rp < size)
		panic("_allocb");

	return b;
}

Block*
allocb(int size)
{
	Block *b;

	/*
	 * Check in a process and wait until successful.
	 * Can still error out of here, though.
	 */
	if(up == nil)
		panic("allocb without up: %#p\n", getcallerpc(&size));
	if((b = _allocb(size, getcallerpc(&size))) == nil)
		panic("allocb: no memory for %d bytes\n", size);
	return b;
}

void
ialloclimit(ulong limit)
{
	ialloc.limit = limit;
}

Block*
iallocb(int size)
{
	Block *b;
	static int m1, m2, mp;

	if(ialloc.bytes > ialloc.limit){
		if((m1++%10000)==0){
			if(mp++ > 1000){
				active.exiting = 1;
				exit(0);
			}
			iprint("iallocb: limited %lud/%lud\n",
				ialloc.bytes, ialloc.limit);
		}
		return nil;
	}

	if((b = _allocb(size, getcallerpc(&size))) == nil){
		if((m2++%10000)==0){
			if(mp++ > 1000){
				active.exiting = 1;
				exit(0);
			}
			iprint("iallocb: no memory %lud/%lud\n",
				ialloc.bytes, ialloc.limit);
		}
		return nil;
	}
	b->flag |= BINTR;

	ilock(&ialloc);
	ialloc.bytes += b->lim - b->base;
	iunlock(&ialloc);

	return b;
}

void
freeb(Block *b)
{
	void *dead = (void*)Bdead;
	uchar *p;

	if(b == nil)
		return;

	/*
	 * drivers which perform non cache coherent DMA manage their own buffer
	 * pool of uncached buffers and provide their own free routine.
	 */
	if(b->free) {
		b->free(b);
		return;
	}

	p = b->base;

	/* poison the block in case someone is still holding onto it */
	b->rp = dead;
	b->wp = dead;
	b->next = dead;

	if((b->flag&BSMALL) != 0){
		if(b->flag & BINTR){
			ilock(&ialloc);
			ialloc.bytes -= b->lim - b->base;
			iunlock(&ialloc);
		}
		nfree(&balloc, b);
		return;
	}
	if(b->flag & BINTR) {
		ilock(&ialloc);
		ialloc.bytes -= b->lim - b->base;
		iunlock(&ialloc);
	}
	b->lim = dead;
	b->base = dead;
	free(p);
}

void
checkb(Block *b, char *msg)
{
	void *dead = (void*)Bdead;

	if(b == dead)
		panic("checkb b %s %#p", msg, b);
	if(b->base == dead || b->lim == dead || b->next == dead
	  || b->rp == dead || b->wp == dead){
		print("checkb: base %#p lim %#p next %#p\n",
			b->base, b->lim, b->next);
		print("checkb: rp %#p wp %#p\n", b->rp, b->wp);
		panic("checkb dead: %s\n", msg);
	}

	if(b->base > b->lim)
		panic("checkb 0 %s %#p %#p", msg, b->base, b->lim);
	if(b->rp < b->base)
		panic("checkb 1 %s %#p %#p", msg, b->base, b->rp);
	if(b->wp < b->base)
		panic("checkb 2 %s %#p %#p", msg, b->base, b->wp);
	if(b->rp > b->lim)
		panic("checkb 3 %s %#p %#p", msg, b->rp, b->lim);
	if(b->wp > b->lim)
		panic("checkb 4 %s %#p %#p", msg, b->wp, b->lim);
}

