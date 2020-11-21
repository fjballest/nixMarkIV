typedef struct Alarms	Alarms;
typedef struct Block	Block;
typedef struct Chan	Chan;
typedef struct Chanflds	Chanflds;
typedef struct Cmdbuf	Cmdbuf;
typedef struct Cmdtab	Cmdtab;
typedef struct Confmem	Confmem;
typedef struct Dev	Dev;
typedef struct DevConf	DevConf;
typedef struct Dirtab	Dirtab;
typedef struct Edf	Edf;
typedef struct Egrp	Egrp;
typedef struct Evalue	Evalue;
typedef struct Fastcall Fastcall;
typedef struct Fgrp	Fgrp;
typedef struct Log	Log;
typedef struct Logflag	Logflag;
typedef struct Mntcache Mntcache;
typedef struct Mount	Mount;
typedef struct Mounted	Mounted;
typedef struct Mnt	Mnt;
typedef struct Mntrpc	Mntrpc;
typedef struct Nalloc	Nalloc;
typedef struct Name	Name;
typedef struct Namec	Namec;
typedef struct Nlink	Nlink;
typedef struct Note	Note;
typedef struct Page	Page;
typedef struct Path	Path;
typedef struct Perf	Perf;
typedef struct Pgalloc	Pgalloc;
typedef struct Pgasz	Pgasz;
typedef struct PhysUart	PhysUart;
typedef struct Pgrp	Pgrp;
typedef struct Physseg	Physseg;
typedef struct Proc	Proc;
typedef struct Procalloc	Procalloc;
typedef struct Pte	Pte;
typedef struct QLock	QLock;
typedef struct Queue	Queue;
typedef struct Ref	Ref;
typedef struct Rendez	Rendez;
typedef struct Rgrp	Rgrp;
typedef struct RWlock	RWlock;
typedef struct Sched	Sched;
typedef struct Schedq	Schedq;
typedef struct Schedstats	Schedstats;
typedef struct Segq	Segq;
typedef struct Segment	Segment;
typedef struct Sema	Sema;
typedef struct Timer	Timer;
typedef struct Timers	Timers;
typedef struct Uart	Uart;
typedef struct Waitq	Waitq;
typedef struct Walkqid	Walkqid;
typedef struct Watchdog	Watchdog;
typedef int    Devgen(Chan*, char*, Dirtab*, int, int, Dir*);

#pragma incomplete DevConf
#pragma incomplete Edf
#pragma incomplete Mntcache
#pragma incomplete Queue
#pragma incomplete Timers

#include <fcall.h>

struct Ref
{
	Lock;
	int	ref;
};

struct Rendez
{
	Lock;
	Proc	*p;
};

struct QLock
{
	Lock	use;		/* to access Qlock structure */
	Proc	*head;		/* next process waiting for object */
	Proc	*tail;		/* last process waiting for object */
	int	locked;		/* flag */
	uintptr	qpc;		/* pc of the holder */
};

struct RWlock
{
	Lock	use;
	Proc	*head;		/* list of waiting processes */
	Proc	*tail;
	uintptr	wpc;		/* pc of writer */
	Proc	*wproc;		/* writing proc */
	int	readers;	/* number of readers */
	int	writer;		/* number of writers */
};

struct Alarms
{
	QLock;
	Proc	*head;
};

/*
 * Must be first element in the data structure allocated
 */
struct Nlink
{
	Nlink	*nnext;
	Nlink	*nlist;
};

struct Nalloc
{
	char	*tag;
	int	elsz;
	int	selfishid;
	int	nselfish;
	void	(*init)(void*);
	void	(*term)(void*);
	void*	(*alloc)(void);
	void	(*dump)(char*, void*);
	Lock;
	Nlink	*free;
	Nlink	*all;
	uint	nused;
	uint	nfree;
	uint	nallocs;
	uint	nfrees;
	uint	nselfallocs;
	uint	nselffrees;
};

/*
 * Access types in namec & channel flags
 */
enum
{
	Aaccess,			/* as in stat, wstat */
	Abind,				/* for left-hand-side of bind */
	Atodir,				/* as in chdir */
	Aopen,				/* for i/o */
	Amount,				/* to be mounted or mounted upon */
	Acreate,			/* is to be created */
	Aremove,			/* remove it */
	Awstat,				/* wstat and clunk it */
	Astat,				/* stat and clunk it */

	COPEN	= 0x0001,		/* for i/o */
	CMSG	= 0x0002,		/* the message channel for a mount */
/*rsc	CCREATE	= 0x0004,		/* permits creation if c->mnt */
	CCEXEC	= 0x0008,		/* close on exec */
	CFREE	= 0x0010,		/* not in use */
	CRCLOSE	= 0x0020,		/* remove on close */
	CCACHE	= 0x0080,		/* client cache */
	CBEHIND	= 0x1000,		/* write behind */
};

/*
 * call-specific arguments for namec.
 */
struct Namec
{
	int omode;	/* Aopen, Acreate */
	ulong perm;	/* Acreate */
	uchar* d;	/* Astat, Awstat */
	long nd;	/* Astat, Awstat */
	int ismtpt;	/* Astat */
};

/* flag values */
enum
{
	BINTR	=	(1<<0),
	BSMALL	=	(1<<1),		/* block kept in small alloc */
	Bipck	=	(1<<2),		/* ip checksum */
	Budpck	=	(1<<3),		/* udp checksum */
	Btcpck	=	(1<<4),		/* tcp checksum */
	Bpktck	=	(1<<5),		/* packet checksum */

};

struct Block
{
	Nlink;				/* keep this first */
	Block*	next;
	Block*	list;
	uchar*	rp;			/* first unconsumed byte */
	uchar*	wp;			/* first empty byte */
	uchar*	lim;			/* 1 past the end of the buffer */
	uchar*	base;			/* start of the buffer */
	void	(*free)(Block*);
	ushort	flag;
	ushort	checksum;		/* IP checksum of complete packet (minus media header) */
};
#define BLEN(s)	((s)->wp - (s)->rp)
#define BALLOC(s) ((s)->lim - (s)->base)

struct Name
{
	Nlink;
	char	*s;
	int	na;
};

struct Mntrpc
{
	Chan*	c;		/* Channel for whom we are working */
	Mntrpc*	next;		/* in free or pending list */
	Mntrpc*	prev;		/* in pending list */
	Fcall	request;	/* Outgoing file system protocol message */
	Fcall 	reply;		/* Incoming reply */
	Mnt*	m;		/* Mount device during rpc */
	Rendez	r;		/* Place to hang out */
	uchar*	rpc;		/* I/O Data buffer */
	uint	rpclen;		/* len of buffer */
	Block	*b;		/* reply blocks */
	char	done;		/* Rpc completed */
	uvlong	stime;		/* start time for mnt statistics */
	ulong	reqlen;		/* request length for mnt statistics */
	ulong	replen;		/* reply length for mnt statistics */
	Mntrpc*	flushed;	/* message this one flushes */
	Mntrpc*	tagnext;	/* next in the same tag (9P2000.ix) */
	int	owntag;		/* tag is owned by this rpc (9P2000.ix) */
	int	pid;		/* rpc has been issued by this process id */
	Walkqid *wq;		/* cloned chan and walk qids */

	uintptr	sendpc;		/* debug */
};

/*
 * Parsed path name.
 */
struct Path
{
	Nlink;		/* keep first */
	Ref;
	char**	els;	/* path elements; no elements means "/" */
	int	naels;	/* elements allocated */
	int 	slen;	/* size for string with path */
	int	nels;	/* elements used */
	int	nres;	/* elements resolved */


	char*	buf;	/* if not nil, free(buf) releases els [0:nbuf) */
	Name*	name;	/* idem, for Names */
	int nbuf;
};

#pragma varargck type "N" Path*

struct Chanflds
{
	vlong	offset;			/* in fd */
	vlong	devoffset;		/* in underlying device; see read */
	Dev*	dev;
	uint	devno;
	ushort	mode;			/* read/write */
	ushort	flag;
	Qid	qid;
	ulong	iounit;			/* chunk size for i/o; 0==default */
	Chan*	umc;			/* channel in union; held for union read */
	QLock	umqlock;		/* serialize unionreads */
	int	uri;			/* union read index */
	int	dri;			/* devdirread index */
	uchar*	dirrock;		/* directory entry rock for translations */
	int	nrock;
	int	mrock;
	QLock	rockqlock;
	int	ismtpt;			/* is a mount point */
	int	hasmtpt;		/* has mount points under it */
	Lock	mclock;
	Segment	*mc;			/* Mount cache pointer */
	Mnt*	mux;			/* Mnt for clients using me for messages */
	union {
		void*	aux;
		Qid	pgrpid;		/* for #p/notepg */
		ulong	mid;		/* for ns in devproc */
	};
	Chan*	mchan;			/* channel to mounted server */
	Qid	mqid;			/* qid of root of mount point */
	Path*	path;
	Mount*	mnt;			/* prefix mount table entry for union reads */
	char*	spec;			/* nil if this is not a mounted fd or mount spec */
	ulong	nsvers;			/* used to update dot */

	Chan*	lchan;			/* devlater */
	Chan*	cnext;			/* ccloseq */
};

struct Chan
{
	Nlink;				/* keep first */
	Ref;				/* the Lock in this Ref is also Chan's lock */
	int	fid;			/* for devmnt */
	Chanflds;			/* fields; cleared for new chans */
};

struct Dev
{
	int	dc;
	char*	name;

	void	(*reset)(void);
	void	(*init)(void);
	void	(*shutdown)(void);
	Chan*	(*attach)(char*);
	Walkqid*(*walk)(Chan*, Chan*, char**, int);
	long	(*stat)(Chan*, uchar*, long);
	Chan*	(*open)(Chan*, int);
	void	(*create)(Chan*, char*, int, int);
	void	(*close)(Chan*);
	long	(*read)(Chan*, void*, long, vlong);
	Block*	(*bread)(Chan*, long, vlong);
	long	(*write)(Chan*, void*, long, vlong);
	long	(*bwrite)(Chan*, Block*, vlong);
	void	(*remove)(Chan*);
	long	(*wstat)(Chan*, uchar*, long);
	void	(*power)(int);	/* power mgt: power(1) => on, power (0) => off */
	int	(*config)(int, char*, DevConf*);	/* returns 0 on error */
	Chan*	(*ncreate)(Chan*, char*, int, int);
};

struct Dirtab
{
	char	name[KNAMELEN];
	Qid	qid;
	vlong	length;
	long	perm;
};

struct Walkqid
{
	Nlink;
	Chan	*clone;
	int	nqid;
	int	naqid;
	Qid	*qid;
};

enum
{
	NSMAX	=	1000,
	NSLOG	=	7,
	NSCACHE	=	(1<<NSLOG),
};

/*
 * Element in union or child mount entry.
 */
struct Mounted 
{
	int	flags;	/* MCREATE|MCACHE */
	Mounted*	next;	/* in union */
	Chan*	to;	/* file in server OR ... */
	Mount *	child;	/* ... children mount entry */
};

/*
 * A mount table and a mount point, depending on your focus.
 * The table is protected by up->pgrp->ns, the rwlock is only
 * for the list of entries to sync with union reads.
 */
struct Mount 
{
	Ref;
	Mount*	parent;		/* parent prefix. null if "/" */
	char*	elem;		/* name prefix */
	int	ismount;	/* number of Chans mounted in um */

	RWlock;
	Mounted*	um;		/* union mount */
};

#pragma varargck type "T" Mount*
#pragma varargck type ">" int


struct Mnt
{
	Lock;
	/* references are counted using c->ref; channels on this mount point incref(c->mchan) == Mnt.c */
	Chan	*c;		/* Channel to file service */
	Proc	*rip;		/* Reader in progress */
	Mntrpc	*queuehd;	/* Queue of pending requests on this channel */
	Mntrpc	*queuetl;
	uint	id;		/* Multiplexer id for channel check */
	Mnt	*list;		/* Free or in use lists */
	int	msize;		/* data + IOHDRSZ */
	char	*version;	/* 9P version */
	int	sharedtags;	/* Ok to share tags in this version? */
	Queue	*q;		/* input queue */
};

enum
{
	NUser,				/* note provided externally */
	NExit,				/* deliver note quietly */
	NDebug,				/* print debug message */
};

struct Note
{
	char	msg[ERRMAX];
	int	flag;			/* whether system posted it */
};

/*
 * One per page allocator.
 * There are one or more top-level allocators plus inner
 * allocators when pages are split into smaller pages.
 */
struct Pgalloc
{
	Page*	parent;		/* parent page or nil for top-level */
	Pgalloc*next;		/* in list of allocators for this pgasz */
	Pgalloc*prev;		/* in list of allocators for this pgasz */
	uintmem	start;		/* Physical address in memory for page 0 */
	usize	npg;		/* number of pages */
	usize	nfree;		/* number of free pages */
	usize	nuser;		/* number of pages used by user segments */
	usize	nbundled;	/* number of pages stealed by bundle allocs */
	usize	nsplit;		/* number of pages stealed by splits */
	uchar	szi;		/* index in Pgasz array (page size) */
	uchar	color;		/* memory locality */
	Page *pg0;		/* first page in page array */
	Page *free;		/* Unused pages */
	Pgalloc *bpga;		/* bundle allocator built upon us */
};

/* Pgasz.type */
enum{PGprealloc, PGembed, PGbundle};

/*
 * One per configured page allocator page size.
 */
struct Pgasz
{
	usize	pgsz;		/* page size */
	uchar	pgszlg2;	/* log2 pgsz */
	uchar	atype;		/* allocation type */
	Pgalloc *pga;		/* allocator list */
	Pgalloc *last;		/* last in allocator list */
	Rendez	r;		/* Sleep for free mem */
	QLock	pwait;		/* Queue of procs waiting for pages */

};

struct Page
{
	Ref;
	QLock;
	uintptr	pa;			/* Physical address in memory */
	uintptr	va;			/* Virtual address for user */
	ushort	n;			/* paged in flag, mmu index */
	uchar	pgszlg2;
	uchar	bundlei;		/* page nb in page bundle [0..bundlesz-1] */
	Page	*next;			/* used by mmu and pgalloc */
	Page	*prev;			/* used by mmu and pgalloc */
	Pgalloc	*pga;			/* Allocator this pg comes from */
};

enum
{
	PG_NOFLUSH	= 0,
	PG_TXTFLUSH	= 1,		/* flush dcache and invalidate icache */
	PG_DATFLUSH	= 2,		/* flush both i & d caches (UNUSED) */

	/* interface between fixfault and mmuput */
	PTEVALID	= 1<<0,
	PTERONLY	= 0<<1,
	PTEWRITE	= 1<<1,
	PTEUSER		= 1<<2,
	PTEUNCACHED	= 1<<4,
};

/*
 * virtual MMU, set to host up to SEGMAXSIZE bytes using
 * the smallest page size and PTEPERTAB entries per table.
 * The max segment size might be larger when using larger pages.
 * The max size is set fixed to be safe also on 32bit systems.
 */
enum
{
	PTEPERTAB = 256,
	SEGMAXSIZE = 0x7c000000u,
};

struct Pte
{
	Page	*pages[PTEPERTAB];	/* Page map for this chunk of pte */
	Page	**first;		/* First used entry */
	Page	**last;			/* Last used entry */
};

/* Segment types */
enum
{
	SG_TEXT		= 0x0,
	SG_DATA,
	SG_STACK,
	SG_SHARED,
	SG_PHYSICAL,
	SG_FREE,
	SG_TYPE		= 0x7,		/* Mask type of segment */
	SG_RONLY	= 0x20,		/* Segment is read only */
	SG_CEXEC	= 0x40,		/* Detach at exec */
	SG_CACHE	= 0x80,		/* segment used as cache */
};


struct Physseg
{
	ulong	attr;			/* Segment attributes */
	char	*name;			/* Attach name */
	uintptr	pa;			/* Physical address */
	usize	size;			/* Maximum segment size in pages */
	Page	*(*pgalloc)(Segment*, uintptr);	/* Allocation if we need it */
	void	(*pgfree)(Page*);
	uchar	lg2pgsize;		/* log2(size of pages in segment) */
};

struct Sema
{
	Rendez;
	int*	addr;
	int	waiting;
	Sema*	next;
	Sema*	prev;
};

/*
 * Segment queue. Usually an lru list
 */
struct Segq
{
	Segment	*hd;		/* hd/tl of lru queue of cached files */
	Segment *tl;
};

struct Segment
{
	Ref;
	QLock	lk;
	ushort	type;		/* segment type */
	uintptr	base;		/* virtual base */
	uintptr	top;		/* virtual top */
	usize	size;		/* size in pages */
	ulong	fstart;		/* start address in file for demand load */
	ulong	flen;		/* length of segment in file */
	uchar	pgszlg2;	/* log2(size of pages in segment) */
	int	flushme;	/* maintain icache for this segment */
	Physseg *pseg;
	ulong*	profile;	/* Tick profile area */

	uintptr	ptemapmem;	/* space mapped by one Pte in this segment */
	Pte	**map;
	Pte	**first;
	Pte	**last;
	int	mapsize;
	int	color;		/* memory locality */

	Lock	semalock;
	Sema	sema;

	Chan	*c;			/* channel to text file */
	Segment	*src;			/* image the data comes from */
	Segment	*hash;			/* Qid hash chains */
	Segment	*lnext;			/* lru list */
	Segment	*lprev;			/* lru list */
	Segment	*anext;			/* alloc list */
	Segment	*fnext;			/* free list */
	int	used;			/* used since the last reclaim */
	/* these are used by cache.c */
	Path	*cpath;			/* debug only */
	Qid	cqid;
	Dev	*cdev;
	vlong	clength;
	vlong	nbytes;
};

enum
{
	RENDLOG	=	5,
	RENDHASH =	1<<RENDLOG,	/* Hash to lookup rendezvous tags */
	MNTLOG	=	5,
	MNTHASH =	1<<MNTLOG,	/* Hash to walk mount table */
	NFD =		100,		/* per process file descriptors */
};
#define REND(p,s)	((p)->rendhash[(s)&((1<<RENDLOG)-1)])
#define MOUNTH(p,qid)	((p)->mnthash[(qid).path&((1<<MNTLOG)-1)])

struct Pgrp
{
	Ref;				/* also used as a lock when mounting */
	int	noattach;
	ulong	pgrpid;
	QLock	debug;			/* single access via devproc.c */
	RWlock	ns;			/* Namespace n read/one write lock */
	Mount	*mnt;			/* prefix mount table */
	char	**ops;		/* for proc/_/ns */
	int	nops;
	int	naops;

	ulong	vers;		/* version of the table; to update dot */
};

struct Rgrp
{
	Ref;				/* the Ref's lock is also the Rgrp's lock */
	Proc	*rendhash[RENDHASH];	/* Rendezvous tag hash */
};

struct Egrp
{
	Ref;
	RWlock;
	Evalue	**ent;
	int	nent;
	int	ment;
	ulong	path;	/* qid.path of next Evalue to be allocated */
	ulong	vers;	/* of Egrp */
};

struct Evalue
{
	char	*name;
	char	*value;
	int	len;
	Evalue	*link;
	Qid	qid;
};

struct Fgrp
{
	Ref;
	Chan	**fd;
	int	nfd;			/* number allocated */
	int	maxfd;			/* highest fd in use */
	int	exceed;			/* debugging */
};

enum
{
	DELTAFD	= 20		/* incremental increase in Fgrp.fd's */
};



struct Waitq
{
	Waitmsg	w;
	Waitq	*next;
};

/*
 * fasttick timer interrupts
 */
enum {
	/* Mode */
	Trelative,	/* timer programmed in ns from now */
	Tperiodic,	/* periodic timer, period in ns */
};

struct Timer
{
	/* Public interface */
	int	tmode;		/* See above */
	vlong	tns;		/* meaning defined by mode */
	void	(*tf)(Ureg*, Timer*);
	void	*ta;
	/* Internal */
	Lock;
	Timers	*tt;		/* Timers queue this timer runs on */
	vlong	twhen;		/* ns represented in fastticks */
	Timer	*tnext;
};

enum
{
	RFNAMEG		= (1<<0),
	RFENVG		= (1<<1),
	RFFDG		= (1<<2),
	RFNOTEG		= (1<<3),
	RFPROC		= (1<<4),
	RFMEM		= (1<<5),
	RFNOWAIT	= (1<<6),
	RFCNAMEG	= (1<<10),
	RFCENVG		= (1<<11),
	RFCFDG		= (1<<12),
	RFREND		= (1<<13),
	RFNOMNT		= (1<<14),
};

/*
 *  process memory segments - NSEG always last !
 */
enum
{
	SSEG, TSEG, DSEG, ESEG, LSEG, SEG1, SEG2, SEG3, SEG4, NSEG
};

enum
{
	Dead = 0,		/* Process states */
	Moribund,
	Ready,
	Scheding,
	Running,
	Queueing,
	QueueingR,
	QueueingW,
	Wakeme,
	Broken,
	Stopped,
	Rendezvous,
	Waitrelease,

	Proc_stopme = 1, 	/* devproc requests */
	Proc_exitme,
	Proc_traceme,
	Proc_exitbig,
	Proc_tracesyscall,

	TUser = 0, 		/* Proc.time */
	TSys,
	TReal,
	TCUser,
	TCSys,
	TCReal,

	NERR = 64,
	NNOTE = 5,

	PriNormal = 10,		/* base priority for normal processes */
	PriKproc = 13,
	PriRoot = 13,
	PriExtra = 19,		/* edf processes at high best-effort pri */
	Npriq,			/* number of scheduler priority levels */
	PriRelease = Npriq,	/* released edf processes */
	PriEdf,			/* active edf processes */
	Nrq,			/* number of priority levels including real time */

};

struct Schedq
{
	Lock;
	Proc*	head;
	Proc*	tail;
	int	n;
};

struct Schedstats
{
	ulong nruns;
	ulong npreempts;
	ulong ndelayscheds;
	ulong nmaxdelayscheds;
	ulong ncs;
	ulong nrebalance;
};

struct Sched
{
	Lock;
	int	nrdy;
	Schedq	runq[Nrq];
	ulong	runvec;
	Mach*	mp;	/* processor for bookkeeping; nil if sched idle */
	ulong balancetime;
	Schedstats;
};

typedef union Ar0 Ar0;
union Ar0 {
	int	i;
	long	l;
	uintptr	p;
	usize	u;
	void*	v;
	vlong	vl;
};

/*
 * Ids for selfish allocators
 */
enum
{
	Selfnone,	/* id 0 unused for safety */
	Selfname,
	Selfpath,
	Selfchan,
	Selfblock,
	Selfwq,
	NSelf,
};

struct Proc
{
	Label	sched;		/* known to l.s */
	char	*kstack;	/* known to l.s */
	Mach	*mach;		/* machine running this proc */
	char	*text;
	char	*user;
	char	*args;
	int	nargs;		/* number of bytes of args */
	Proc	*rnext;		/* next process in run queue */
	Proc	*qnext;		/* next process on queue for a QLock */
	QLock	*qlock;		/* addr of qlock being queued for DEBUG */
	int	state;
	char	*psstate;	/* What /proc/#/status reports */
	Segment	*seg[NSEG];
	QLock	seglock;	/* locked whenever seg[] changes */
	int	pid;
	int	index;		/* index (slot) in proc array */
	int	ref;		/* indirect reference */
	int	noteid;		/* Equivalent of note group */
	Proc	*pidhash;	/* next proc in pid hash */

	Lock	exl;		/* Lock count and waitq */
	Waitq	*waitq;		/* Exited processes wait children */
	int	nchild;		/* Number of living children */
	int	nwait;		/* Number of uncollected wait records */
	QLock	qwaitr;
	Rendez	waitr;		/* Place to hang out in wait */
	Proc	*parent;

	Pgrp	*pgrp;		/* Process group for namespace */
	Egrp 	*egrp;		/* Environment group */
	Fgrp	*fgrp;		/* File descriptor group */
	Rgrp	*rgrp;		/* Rendez group */

	Fgrp	*closingfgrp;	/* used during teardown */

	int	parentpid;
	ulong	time[6];	/* User, Sys, Real; child U, S, R */

	uvlong	kentry;		/* Kernel entry time stamp (for profiling) */
	/*
	 * pcycles: cycles spent in this process (updated on procsave/restore)
	 * when this is the current proc and we're in the kernel
	 * (procrestores outnumber procsaves by one)
	 * the number of cycles spent in the proc is pcycles + cycles()
	 * when this is not the current process or we're in user mode
	 * (procrestores and procsaves balance), it is pcycles.
	 */
	vlong	pcycles;

	int	insyscall;

	QLock	debug;		/* to access debugging elements of User */
	Proc	*pdbg;		/* the debugging process */
	ulong	procmode;	/* proc device file mode */
	ulong	privatemem;	/* proc does not let anyone read mem */
	int	hang;		/* hang at next exec for debug */
	int	procctl;	/* Control for /proc debugging */
	uintptr	pc;		/* DEBUG only */

	Lock	rlock;		/* sync sleep/wakeup with postnote */
	Rendez	*r;		/* rendezvous point slept on */
	Rendez	sleep;		/* place for syssleep/debug */
	int	notepending;	/* note issued but not acted on */
	int	kp;		/* true if a kernel process */
	Proc	*palarm;	/* Next alarm time */
	ulong	alarm;		/* Time of call */
	int	newtlb;		/* Pager has changed my pte's, I must flush */

	uintptr	rendtag;	/* Tag for rendezvous */
	uintptr	rendval;	/* Value for rendezvous */
	Proc	*rendhash;	/* Hash list for tag values */

	Timer;			/* For tsleep and real-time */
	Rendez	*trend;
	int	(*tfn)(void*);
	void	(*kpfun)(void*);
	void	*kparg;

	int	scallnr;	/* system call number */
	uchar	arg[MAXSYSARG*sizeof(void*)];	/* system call arguments */
	int	nerrlab;
	Label	errlab[NERR];
	char	*syserrstr;	/* last error from a system call, errbuf0 or 1 */
	char	*errstr;	/* reason we're unwinding the error stack, errbuf1 or 0 */
	char	errbuf0[ERRMAX];
	char	errbuf1[ERRMAX];
	char	genbuf[128];	/* buffer used e.g. for last name element from namec */
	Chan	*slash;
	Chan	*dot;

	Note	note[NNOTE];
	short	nnote;
	short	notified;	/* sysnoted is due */
	Note	lastnote;
	void	(*notify)(void*, char*);

	Lock	*lockwait;
	Lock	*lastlock;	/* debugging */
	Lock	*lastilock;	/* debugging */

	Mach	*wired;
	Mach	*mp;		/* machine this process last ran on */
	int	nlocks;		/* number of locks held by proc */
	int	nschedlocks;	/* number of locks held that prevent sched()ing */
	ulong	delaysched;
	ulong	priority;	/* priority level */
	ulong	basepri;	/* base priority level */
	int	fixedpri;	/* priority level does not change */
	ulong	cpu;		/* cpu average */
	ulong	lastupdate;
	ulong	readytime;	/* time process came ready */
	ulong	movetime;	/* last time process switched processors */
	int	preempted;	/* true if this process hasn't finished the interrupt
				 *  that last preempted it
				 */
	Sched	*sch;		/* scheduler this process belongs to */
	Edf	*edf;		/* if non-null, real-time proc, edf contains scheduling params */
	int	trace;		/* process being traced? */

	uintptr	qpc;		/* pc calling last blocking qlock */
	uintptr	spc;		/* pc calling last sleep */ 
	int	setargs;

	void	*ureg;		/* User registers for notes */
	void	*dbgreg;	/* User registers for devproc */

	Fastcall* fc;
	int	fcount;
	char*	syscalltrace;

	/* Pools of per-process allocator, caching a few resources
	 * not in use for later. Only pages and chans and paths by now.
	 */
	Lock	selfishlk;
	Nlink	*selfish[NSelf];
	int	nselfish[NSelf];
	Page	*pgfree;
	int	npgfree;

	/*
	 *  machine specific fpu, mmu and notify
	 */
	PFPU;
	PMMU;
	PNOTIFY;
};

enum
{
	PROCMAX	= 2000,			/* maximum number of processes */
};

struct Procalloc
{
	Lock;
	Proc*	ht[128];
	Proc*	arena;
	Proc*	free;
};

enum
{
	PRINTSIZE =	256,
	NUMSIZE	=	12,		/* size of formatted number */
	/* READSTR was 1000, which is way too small for usb's ctl file */
	READSTR =	4000,		/* temporary buffer size for device reads */
};

extern	char*	conffile;
extern	char	configfile[];
extern	int	cpuserver;
extern	char*	eve;
extern	char	hostdomain[];
extern	uchar	initcode[];
extern	int	kbdbuttons;
extern	Ref	noteidalloc;
extern	int	nphysseg;
extern	int	nsyscall;
extern	Physseg	physseg[];
extern	Procalloc	procalloc;
extern	uint	qiomaxatomic;
extern	char*	statename[];
extern	char*	sysname;
extern struct {
	char*	n;
	void (*f)(Ar0*, va_list);
	Ar0	r;
} systab[];

enum
{
	LRESPROF	= 3,
};

/*
 *  action log
 */
struct Log {
	Lock;
	int	opens;
	char*	buf;
	char	*end;
	char	*rptr;
	int	len;
	int	nlog;
	int	minread;

	int	logmask;	/* mask of things to debug */

	QLock	readq;
	Rendez	readr;
};

struct Logflag {
	char*	name;
	int	mask;
};

enum
{
	NCMDFIELD = 128
};

struct Cmdbuf
{
	char	*buf;
	char	**f;
	int	nf;
};

struct Cmdtab
{
	int	index;	/* used by client to switch on result */
	char	*cmd;	/* command name */
	int	narg;	/* expected #args; 0 ==> variadic */
};

/*
 *  routines to access UART hardware
 */
struct PhysUart
{
	char*	name;
	Uart*	(*pnp)(void);
	void	(*enable)(Uart*, int);
	void	(*disable)(Uart*);
	void	(*kick)(Uart*);
	void	(*dobreak)(Uart*, int);
	int	(*baud)(Uart*, int);
	int	(*bits)(Uart*, int);
	int	(*stop)(Uart*, int);
	int	(*parity)(Uart*, int);
	void	(*modemctl)(Uart*, int);
	void	(*rts)(Uart*, int);
	void	(*dtr)(Uart*, int);
	long	(*status)(Uart*, void*, long, long);
	void	(*fifo)(Uart*, int);
	void	(*power)(Uart*, int);
	int	(*getc)(Uart*);			/* polling version for rdb */
	void	(*putc)(Uart*, int);		/* polling version for iprint */
	void	(*poll)(Uart*);			/* polled interrupt routine */
};

enum {
	Stagesize=	2048
};

/*
 *  software UART
 */
struct Uart
{
	void*	regs;			/* hardware stuff */
	void*	saveregs;		/* place to put registers on power down */
	char*	name;			/* internal name */
	ulong	freq;			/* clock frequency */
	int	bits;			/* bits per character */
	int	stop;			/* stop bits */
	int	parity;			/* even, odd or no parity */
	int	baud;			/* baud rate */
	PhysUart*phys;
	int	console;		/* used as a serial console */
	int	special;		/* internal kernel device */
	Uart*	next;			/* list of allocated uarts */

	QLock;
	int	type;			/* ?? */
	int	dev;
	int	opens;

	int	enabled;
	Uart	*elist;			/* next enabled interface */

	int	perr;			/* parity errors */
	int	ferr;			/* framing errors */
	int	oerr;			/* rcvr overruns */
	int	berr;			/* no input buffers */
	int	serr;			/* input queue overflow */

	/* buffers */
	int	(*putc)(Queue*, int);
	Queue	*iq;
	Queue	*oq;

	Lock	rlock;
	uchar	istage[Stagesize];
	uchar	*iw;
	uchar	*ir;
	uchar	*ie;

	Lock	tlock;			/* transmit */
	uchar	ostage[Stagesize];
	uchar	*op;
	uchar	*oe;
	int	drain;

	int	modem;			/* hardware flow control on */
	int	xonoff;			/* software flow control on */
	int	blocked;
	int	cts, dsr, dcd;		/* keep track of modem status */
	int	ctsbackoff;
	int	hup_dsr, hup_dcd;	/* send hangup upstream? */
	int	dohup;

	Rendez	r;
};

enum
{
	/* addconsdev flags */
	Ciprint		= 2,		/* call this fn from iprint */
	Cntorn		= 4,		/* change \n to \r\n */

	/* addconsdev default consoles */
	Ckmesg = 0,
	Ckprint,
	Cuart,
};

extern	Uart*	consuart;

/*
 *  performance timers, all units in perfticks
 */
struct Perf
{
	ulong	intrts;		/* time of last interrupt */
	ulong	inintr;		/* time since last clock tick in interrupt handlers */
	ulong	avg_inintr;	/* avg time per clock tick in interrupt handlers */
	ulong	inidle;		/* time since last clock tick in idle loop */
	ulong	avg_inidle;	/* avg time per clock tick in idle loop */
	ulong	last;		/* value of perfticks() at last clock tick */
	ulong	period;		/* perfticks() per clock tick */
};

struct Watchdog
{
	void	(*enable)(void);	/* watchdog enable */
	void	(*disable)(void);	/* watchdog disable */
	void	(*restart)(void);	/* watchdog restart */
	void	(*stat)(char*, char*);	/* watchdog statistics */
};

/* queue state bits,  Qmsg, Qcoalesce, and Qkick can be set in qopen */
enum
{
	/* Queue.state */
	Qstarve		= (1<<0),	/* consumer starved */
	Qmsg		= (1<<1),	/* message stream */
	Qclosed		= (1<<2),	/* queue has been closed/hungup */
	Qflow		= (1<<3),	/* producer flow controlled */
	Qcoalesce	= (1<<4),	/* coalesce packets on read */
	Qkick		= (1<<5),	/* always call the kick routine after qwrite */
};

#define DEVDOTDOT -1

#pragma	varargck	type	"I"	uchar*
#pragma	varargck	type	"V"	uchar*
#pragma	varargck	type	"E"	uchar*
#pragma	varargck	type	"M"	uchar*

#pragma	varargck	type	"m"	Mreg
