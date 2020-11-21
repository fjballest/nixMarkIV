#include	<u.h>
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

#include	"../port/edf.h"
#include	"errstr.h"
#include	<ptrace.h>

enum
{
	Scaling=2,
	Schedgain = 30,		/* secs */

	Schedsz = 2,		/* one scheduler every 2 machs */
	Nsched = MACHMAX/Schedsz,

	Nbalance = 3,		/* one out of Nbalance runs affinity is ignored */

	Ndelaysched = 50,	/* max delayed scheds */
};

extern Proc* psalloc(void);
extern void pshash(Proc*);
extern void psrelease(Proc*);
extern void psunhash(Proc*);

Sched scheds[Nsched];

Ref	noteidalloc;
static Ref pidalloc;
Procalloc procalloc;

char *statename[] =
{	/* BUG: generate automatically */
	"Dead",
	"Moribund",
	"Ready",
	"Scheding",
	"Running",
	"Queueing",
	"QueueingR",
	"QueueingW",
	"Wakeme",
	"Broken",
	"Stopped",
	"Rendez",
	"Waitrelease",
};

/*
 * Update the cpu time average for this particular process,
 * which is about to change from up -> not up or vice versa.
 * p->lastupdate is the last time an updatecpu happened.
 *
 * The cpu time average is a decaying average that lasts
 * about D clock ticks.  D is chosen to be approximately
 * the cpu time of a cpu-intensive "quick job".  A job has to run
 * for approximately D clock ticks before we home in on its
 * actual cpu usage.  Thus if you manage to get in and get out
 * quickly, you won't be penalized during your burst.  Once you
 * start using your share of the cpu for more than about D
 * clock ticks though, your p->cpu hits 1000 (1.0) and you end up
 * below all the other quick jobs.  Interactive tasks, because
 * they basically always use less than their fair share of cpu,
 * will be rewarded.
 *
 * If the process has not been running, then we want to
 * apply the filter
 *
 *	cpu = cpu * (D-1)/D
 *
 * n times, yielding
 *
 *	cpu = cpu * ((D-1)/D)^n
 *
 * but D is big enough that this is approximately
 *
 * 	cpu = cpu * (D-n)/D
 *
 * so we use that instead.
 *
 * If the process has been running, we apply the filter to
 * 1 - cpu, yielding a similar equation.  Note that cpu is
 * stored in fixed point (* 1000).
 *
 * Updatecpu must be called before changing up, in order
 * to maintain accurate cpu usage statistics.  It can be called
 * at any time to bring the stats for a given proc up-to-date.
 */
static void
updatecpu(Proc *p)
{
	int D, n, t, ocpu;

	if(p->edf != nil)
		return;

	t = sys->ticks*Scaling + Scaling/2;
	n = t - p->lastupdate;
	p->lastupdate = t;

	if(n == 0)
		return;
	D = Schedgain*HZ*Scaling;
	if(n > D)
		n = D;

	ocpu = p->cpu;
	if(p != up)
		p->cpu = (ocpu*(D-n))/D;
	else{
		t = 1000 - ocpu;
		t = (t*(D-n))/D;
		p->cpu = 1000 - t;
	}

	if(0)
		iprint("pid %d %s for %d cpu %d -> %uld\n",
			p->pid,p==up?"active":"inactive",n, ocpu,p->cpu);
}

/*
 * On average, p has used p->cpu of a cpu recently.
 * Its fair share is sys.nonline/m->load of a cpu.  If it has been getting
 * too much, penalize it.  If it has been getting not enough, reward it.
 * I don't think you can get much more than your fair share that
 * often, so most of the queues are for using less.  Having a priority
 * of 3 means you're just right (?).  Having a higher priority (up to p->basepri)
 * means you're not using as much as you could.
 */
static int
reprioritize(Proc *p)
{
	int fairshare, n, load, ratio;

	if(p->edf != nil)
		return p->priority;

	load = sys->machptr[0]->load;
	if(load == 0)
		return p->basepri;

	/*
	 * fairshare = 1.000 * PROCMAX * 1.000/load,
	 * except the decimal point is moved three places
	 * on both load and fairshare.
	 */
	fairshare = (sys->nonline*1000*1000)/load;
	n = p->cpu;
	if(n == 0)
		n = 1;
	ratio = (fairshare+n/2) / n;
	if(ratio > p->basepri)
		ratio = p->basepri;
	if(ratio < 0)
		panic("reprioritize");
	if(0)
		iprint("pid %d cpu %ldd load %d fair %d pri %d\n",
			p->pid, p->cpu, load, fairshare, ratio);
	return ratio;
}

void
setsched(void)
{
	m->sch = &scheds[m->machno/Schedsz];
}

/*
 * Always splhi()'ed.
 */
void
schedinit(void)		/* never returns */
{
	static Lock lk;

	lock(&lk);
	m->sch->mp = m;
	unlock(&lk);
	setlabel(&m->sched);

	if(up) {
		if(up->edf != nil)
			edfrecord(up);
		updatecpu(up);
		m->proc = 0;
		switch(up->state) {
		case Running:
			ready(up);
			break;
		case Moribund:
			up->state = Dead;

			/*
			 * Holding lock from pexit:
			 * 	procalloc
			 */
			mmurelease(up);

			psrelease(up);
			noschedunlock(&procalloc);
			break;
		}
		up->mach = nil;
		up = nil;
	}
	sched();
}

/*
 *  If changing this routine, look also at sleep().  It
 *  contains a copy of the guts of sched().
 */
void
sched(void)
{
	Proc *p;
	Sched *sch;

	sch = m->sch;
	if(m->ilockdepth)
		panic("cpu%d: ilockdepth %d, last lock %#p at %#p, sched called from %#p",
			m->machno,
			m->ilockdepth,
			up? up->lastilock: nil,
			(up && up->lastilock)? up->lastilock->pc: m->ilockpc,
			getcallerpc(&p+2));

	if(up){
		/*
		 * Delay the sched until the process gives up the locks
		 * it is holding.  This avoids dumb lock loops.
		 * Don't delay if the process is Moribund.
		 * It called sched to die.
		 * But do sched eventually.  This avoids a missing unlock
		 * from hanging the entire kernel.
		 * But don't reschedule procs holding no-sched-locks
		 * (ie., page alloc or procalloc locks)
		 * Those are far too important to be holding while asleep.
		 *
		 * This test is not exact.  There can still be a few
		 * instructions in the middle of taslock when a process
		 * holds a lock but Lock.p has not yet been initialized.
		 */
		if(up->nlocks)
		if(up->state != Moribund)
		if(up->delaysched < Ndelaysched || up->nschedlocks){
			up->delaysched++;
			sch->ndelayscheds++;
			return;
		}
		if(up->delaysched > sch->nmaxdelayscheds)
			sch->nmaxdelayscheds = up->delaysched;
		up->delaysched = 0;

		splhi();

		/* statistics */
		m->cs++;
		sch->ncs++;

		procsave(up);
		if(setlabel(&up->sched)){
			procrestore(up);
			spllo();
			return;
		}
		gotolabel(&m->sched);
	}
	p = runproc();
	updatecpu(p);
	p->priority = reprioritize(p);
	m->schedticks = m->ticks + HZ/10;
	up = p;
	up->state = Running;
	up->mach = m;
	m->proc = up;
	mmuswitch();
	gotolabel(&up->sched);
}

static int
anyready(void)
{
	return m->sch->runvec;
}

static int
anyhigher(void)
{
	return m->sch->runvec & ~((1<<(up->priority+1))-1);
}

static void
linkproc(Proc *p, int pri)
{
	Sched *sch;
	Schedq *rq;

	if(p->rnext != nil)
		panic("linkproc");
	sch = p->sch;
	rq = &sch->runq[pri];
	p->priority = pri;
	if(rq->tail != nil)
		rq->tail->rnext = p;
	else
		rq->head = p;
	rq->tail = p;
	rq->n++;
	sch->nrdy++;
	sch->runvec |= 1<<pri;
}

static void
unlinkproc(Schedq *rq, Proc *l, Proc *p)
{
	Sched *sch;

	sch = p->sch;
	if(l != nil)
		l->rnext = p->rnext;
	else
		rq->head = p->rnext;
	if(p->rnext == nil)
		rq->tail = l;
	p->rnext = nil;
	if(rq->head == nil)
		sch->runvec &= ~(1<<(rq - sch->runq));
	rq->n--;
	sch->nrdy--;
}

/*
 * Here once per clock tick to see if we should resched.
 * Unless preempted, get to run for at least 100ms.
 */
void
hzsched(void)
{
	if(anyhigher()
	|| (!up->fixedpri && m->ticks > m->schedticks && anyready()))
		up->delaysched++;
}

/*
 * Here at the end of non-clock interrupts to see if we should preempt the
 * current process.  Returns 1 if preempted, 0 otherwise.
 */
int
preempted(void)
{
	if(up && up->state == Running)
	if(up->preempted == 0)
	if(anyhigher())
	if(!active.exiting){
		up->preempted = 1;
		sched();
		splhi();
		up->preempted = 0;
		return 1;
	}
	return 0;
}

/*
 * ready(p) picks a new priority for a process and sticks it in the
 * runq for that priority.
 */
void
ready(Proc *p)
{
	Mreg s;
	int pri;
	void (*pt)(Proc*, int, vlong, vlong);

	s = splhi();
	if(p->edf != nil && edfready(p)){
		splx(s);
		return;
	}

	updatecpu(p);
	pri = reprioritize(p);
	p->priority = pri;
	p->state = Ready;
	if(p->trace && (pt = proctrace) != nil)
		pt(p, SReady, 0, 0);
	lock(p->sch);
	linkproc(p, pri);
	unlock(p->sch);
	splx(s);
}

/*
 *  yield the processor and drop our priority
 */
void
yield(void)
{
	if(anyready()){
		/* pretend we just used 1/2 tick */
		up->lastupdate -= Scaling/2;
		sched();
	}
}

/*
 * The process is execing a new program.
 * Time to decide which scheduler it should use.
 * As of now we just round-robin the scheds available.
 */
void
execsched(void)
{
	static Lock lastlk;
	static int last;
	int i, ni;

	if(up->wired != nil)
		return;
	lock(&lastlk);
	last++;
	for(i = 0; i < Nsched; i++){
		ni = (last+i)%Nsched;
		if(scheds[ni].mp != nil){
			up->sch = &scheds[ni];
			last = ni;
			break;
		}
	}
	unlock(&lastlk);
}

static Proc*
rqrunproc(Schedq *rq, int affinity)
{
	Proc *p, *l;

	l = nil;
	for(p = rq->head; p != nil; p = p->rnext){
		/* if p->mach is not nil, the process
		 * state is not saved and we can't run it yet.
		 */
		if(p->mach != nil)
			goto next;
		if(p->wired != nil){
			/* If the process was wired to a different
			 * scheduler, update its sch and run it now.
			 * When it moves out of the processor it will
			 * be linked to the right scheduler and stay
			 * wired there.
			 */
			if(p->sch != p->wired->sch){
				p->sch = p->wired->sch;
				break;
			}
			if(p->wired == m)
				break;
			goto next;
		}
		if(p->mp == nil || !affinity || p->mp == m)
			break;
	next:
		l = p;
	}
	if(p != nil)
		unlinkproc(rq, l, p);
	return p;
}

static void
rebalance(void)
{
	Sched *sch;
	Schedq *rq;
	Proc *p, *l;
	int pri, npri;

	sch = m->sch;
	for(pri = 0; pri < Npriq; pri++){
		rq = &sch->runq[pri];
	again:
		l = nil;
		for(p = rq->head; p != nil; p = p->rnext){
			updatecpu(p);
			npri = reprioritize(p);
			if(npri != pri){
				sch->nrebalance++;
				unlinkproc(rq, l, p);
				linkproc(p, npri);
				goto again;
			}
			l = p;
		}
	}
}

/*
 * Pick a process to run.
 * 
 * 1/Nbalance times affinity is ignored, but other times we prefer
 * a process that did run last on this processor.
 *
 * Once a second we recompute priorities.
 */
Proc*
runproc(void)
{
	Sched *sch;
	Schedq *rq;
	int i;
	Proc *p;
	ulong start, now;
	void (*pt)(Proc*, int, vlong, vlong);

	start = perfticks();
	sch = m->sch;
	do{
		p = nil;
		rq = nil;
		splhi();
		lock(sch);
		sch->nruns++;
		if(sch->mp == m && m->ticks - sch->balancetime >= HZ){
			rebalance();
			sch->balancetime = m->ticks;
		}
		for(i = Nrq-1; i >= 0; i--){
			rq = &sch->runq[i];
			if(sch->nruns%Nbalance)
				p = rqrunproc(rq, 1);
			if(p == nil)
				p = rqrunproc(rq, 0);
			if(p != nil)
				break;
		}
		unlock(sch);
		if(p == nil){
			spllo();
			while(sch->nrdy == 0){
				idlehands();
				now = perfticks();
				m->perf.inidle += now-start;
				start = now;
			}
		}
	}while(p == nil);

	if(p->state != Ready)
		iprint("runproc %s %d %s\n",
			p->text, p->pid, statename[p->state]);
	p->state = Scheding;
	p->mp = m;
	if(p->edf != nil)
		edfrun(p, rq == &sch->runq[PriEdf]);
	pt = proctrace;
	if(pt)
		pt(p, SRun, 0, 0);
	return p;
}

int
canpage(Proc *p)
{
	int ok;

	splhi();
	lock(p->sch);
	/* Only reliable way to see if we are Running */
	if(p->mach == 0) {
		p->newtlb = 1;
		ok = 1;
	}
	else
		ok = 0;
	unlock(p->sch);
	spllo();

	return ok;
}

Proc*
newproc(void)
{
	Proc *p;

	p = psalloc();

	p->state = Scheding;
	p->psstate = "New";
	p->mach = 0;
	p->qnext = 0;
	p->nchild = 0;
	p->nwait = 0;
	p->waitq = 0;
	p->parent = 0;
	p->pgrp = 0;
	p->egrp = 0;
	p->fgrp = 0;
	p->rgrp = 0;
	p->pdbg = 0;
	p->kp = 0;
	if(up != nil && up->procctl == Proc_tracesyscall)
		p->procctl = Proc_tracesyscall;
	else
		p->procctl = 0;
	p->syscalltrace = nil;
	p->notepending = 0;
	p->ureg = 0;
	p->privatemem = 0;
	p->errstr = p->errbuf0;
	p->syserrstr = p->errbuf1;
	p->errbuf0[0] = '\0';
	p->errbuf1[0] = '\0';
	p->nlocks = 0;
	p->delaysched = 0;
	p->trace = 0;
	kstrdup(&p->user, "*nouser");
	kstrdup(&p->text, "*notext");
	kstrdup(&p->args, "");
	p->nargs = 0;
	p->setargs = 0;
	memset(p->seg, 0, sizeof p->seg);
	p->pid = incref(&pidalloc);
	pshash(p);
	p->noteid = incref(&noteidalloc);
	if(p->pid <= 0 || p->noteid <= 0)
		panic("pidalloc");
	if(p->kstack == 0)
		p->kstack = smalloc(KSTACK);

	/* sched params */
	p->mp = 0;
	p->wired = 0;
	procpriority(p, PriNormal, 0);
	p->cpu = 0;
	p->lastupdate = sys->ticks*Scaling;
	p->edf = nil;
	p->sch = m->sch;
	return p;
}

/*
 * wire this proc to a machine.
 * If it's wired to one on a different scheduler it won't
 * become actually wired until it runs one more time, because
 * we don't want to lock both schedulers at the same time.
 */
void
procwired(Proc *p, int bm)
{
	Proc *pp;
	int i;
	char nwired[MACHMAX];
	Mach *wm, *mp;

	if(bm < 0){
		/* pick a machine to wire to */
		memset(nwired, 0, sizeof(nwired));
		p->wired = 0;
		for(i=0; (pp = psincref(i)) != nil; i++){
			wm = pp->wired;
			if(wm && pp->pid)
				nwired[wm->machno]++;
			psdecref(pp);
		}
		bm = 0;
		for(i=0; i<MACHMAX; i++){
			if((mp = sys->machptr[i]) == nil || !mp->online)
				continue;
			if(nwired[i] < nwired[bm])
				bm = i;
		}
	} else {
		/* use the virtual machine requested */
		bm = bm % MACHMAX;
	}

	p->wired = sys->machptr[bm];
	p->mp = p->wired;
}

void
procpriority(Proc *p, int pri, int fixed)
{
	if(pri >= Npriq)
		pri = Npriq - 1;
	else if(pri < 0)
		pri = 0;
	p->basepri = pri;
	p->priority = pri;
	if(fixed){
		p->fixedpri = 1;
	} else {
		p->fixedpri = 0;
	}
}

/*
 *  sleep if a condition is not true.  Another process will
 *  awaken us after it sets the condition.  When we awaken
 *  the condition may no longer be true.
 *
 *  we lock both the process and the rendezvous to keep r->p
 *  and p->r synchronized.
 */
void
sleep(Rendez *r, int (*f)(void*), void *arg)
{
	Mreg s;
	void (*pt)(Proc*, int, vlong, vlong);

	s = splhi();

	if(up->nlocks)
		print("process %d sleeps with %d locks held, last lock %#p locked at pc %#p, sleep called from %#p\n",
			up->pid, up->nlocks, up->lastlock, up->lastlock->pc, getcallerpc(&r));
	lock(r);
	lock(&up->rlock);
	if(r->p){
		print("double sleep called from %#p, %d %d\n",
			getcallerpc(&r), r->p->pid, up->pid);
		dumpstack();
	}

	/*
	 *  Wakeup only knows there may be something to do by testing
	 *  r->p in order to get something to lock on.
	 *  Flush that information out to memory in case the sleep is
	 *  committed.
	 */
	r->p = up;

	if((*f)(arg) || up->notepending){
		/*
		 *  if condition happened or a note is pending
		 *  never mind
		 */
		r->p = nil;
		unlock(&up->rlock);
		unlock(r);
	} else {
		/*
		 *  now we are committed to
		 *  change state and call scheduler
		 */
		up->state = Wakeme;
		up->r = r;
		if(up->trace && (pt = proctrace) != nil)
			pt(up, SSleep, 0, Wakeme|(getcallerpc(&r)<<8));

		/* statistics */
		m->cs++;

		procsave(up);
		if(setlabel(&up->sched)) {
			/*
			 *  here when the process is awakened
			 */
			procrestore(up);
			spllo();
			up->spc = 0;
		} else {
			/*
			 *  here to go to sleep (i.e. stop Running)
			 */
			up->spc = getcallerpc(&r);
			unlock(&up->rlock);
			unlock(r);
			gotolabel(&m->sched);
		}
	}

	if(up->notepending) {
		up->notepending = 0;
		splx(s);
		if(up->procctl == Proc_exitme && up->closingfgrp)
			forceclosefgrp();
		error(Eintr);
	}

	splx(s);
}

static int
tfn(void *arg)
{
	return up->trend == nil || up->tfn(arg);
}

void
twakeup(Ureg*, Timer *t)
{
	Proc *p;
	Rendez *trend;

	p = t->ta;
	trend = p->trend;
	p->trend = 0;
	if(trend)
		wakeup(trend);
}

void
tsleep(Rendez *r, int (*fn)(void*), void *arg, long ms)
{
	if (up->tt){
		print("tsleep: timer active: mode %d, tf %#p\n",
			up->tmode, up->tf);
		timerdel(up);
	}
	up->tns = MS2NS(ms);
	up->tf = twakeup;
	up->tmode = Trelative;
	up->ta = up;
	up->trend = r;
	up->tfn = fn;
	timeradd(up);

	if(waserror()){
		timerdel(up);
		nexterror();
	}
	sleep(r, tfn, arg);
	if (up->tt)
		timerdel(up);
	up->twhen = 0;
	poperror();
}

/*
 *  Expects that only one process can call wakeup for any given Rendez.
 *  We hold both locks to ensure that r->p and p->r remain consistent.
 *  Richard Miller has a better solution that doesn't require both to
 *  be held simultaneously, but I'm a paranoid - presotto.
 */
Proc*
wakeup(Rendez *r)
{
	Mreg s;
	Proc *p;

	s = splhi();

	lock(r);
	p = r->p;

	if(p != nil){
		lock(&p->rlock);
		if(p->state != Wakeme || p->r != r)
			panic("wakeup: state");
		r->p = nil;
		p->r = nil;
		ready(p);
		unlock(&p->rlock);
	}
	unlock(r);

	splx(s);

	return p;
}

/*
 *  if waking a sleeping process, this routine must hold both
 *  p->rlock and r->lock.  However, it can't know them in
 *  the same order as wakeup causing a possible lock ordering
 *  deadlock.  We break the deadlock by giving up the p->rlock
 *  lock if we can't get the r->lock and retrying.
 */
int
postnote(Proc *p, int dolock, char *n, int flag)
{
	Mreg s;
	int ret;
	Rendez *r;
	Proc *d, **l;

	if(dolock)
		qlock(&p->debug);

	if(flag != NUser && (p->notify == 0 || p->notified))
		p->nnote = 0;

	ret = 0;
	if(p->nnote < NNOTE) {
		strcpy(p->note[p->nnote].msg, n);
		p->note[p->nnote++].flag = flag;
		ret = 1;
	}
	p->notepending = 1;
	if(dolock)
		qunlock(&p->debug);

	/* this loop is to avoid lock ordering problems. */
	for(;;){
		s = splhi();
		lock(&p->rlock);
		r = p->r;

		/* waiting for a wakeup? */
		if(r == nil)
			break;	/* no */

		/* try for the second lock */
		if(canlock(r)){
			if(p->state != Wakeme || r->p != p)
				panic("postnote: state %d %d %d", r->p != p, p->r != r, p->state);
			p->r = nil;
			r->p = nil;
			ready(p);
			unlock(r);
			break;
		}

		/* give other process time to get out of critical section and try again */
		unlock(&p->rlock);
		splx(s);
		sched();
	}
	unlock(&p->rlock);
	splx(s);

	if(p->state != Rendezvous)
		return ret;

	/* Try and pull out of a rendezvous */
	lock(p->rgrp);
	if(p->state == Rendezvous) {
		p->rendval = ~0;
		l = &REND(p->rgrp, p->rendtag);
		for(d = *l; d; d = d->rendhash) {
			if(d == p) {
				*l = p->rendhash;
				break;
			}
			l = &d->rendhash;
		}
		ready(p);
	}
	unlock(p->rgrp);
	return ret;
}

/*
 * weird thing: keep at most NBROKEN around
 */
#define	NBROKEN 4
struct
{
	QLock;
	int	n;
	Proc	*p[NBROKEN];
}broken;

void
addbroken(Proc *p)
{
	qlock(&broken);
	if(broken.n == NBROKEN) {
		ready(broken.p[0]);
		memmove(&broken.p[0], &broken.p[1], sizeof(Proc*)*(NBROKEN-1));
		--broken.n;
	}
	broken.p[broken.n++] = p;
	qunlock(&broken);

	if(p->edf != nil)
		edfstop(p);
	p->state = Broken;
	p->psstate = 0;
	sched();
}

void
unbreak(Proc *p)
{
	int b;

	qlock(&broken);
	for(b=0; b < broken.n; b++)
		if(broken.p[b] == p) {
			broken.n--;
			memmove(&broken.p[b], &broken.p[b+1],
					sizeof(Proc*)*(NBROKEN-(b+1)));
			ready(p);
			break;
		}
	qunlock(&broken);
}

int
freebroken(void)
{
	int i, n;

	qlock(&broken);
	n = broken.n;
	for(i=0; i<n; i++) {
		ready(broken.p[i]);
		broken.p[i] = 0;
	}
	broken.n = 0;
	qunlock(&broken);
	return n;
}

void
pexit(char *exitstr, int freemem)
{
	Proc *p;
	Segment **s, **es;
	long utime, stime;
	Waitq *wq, *f, *next;
	Fgrp *fgrp;
	Egrp *egrp;
	Rgrp *rgrp;
	Pgrp *pgrp;
	Chan *dot, *slash;
	void (*pt)(Proc*, int, vlong, vlong);

	if(up->syscalltrace != nil)
		free(up->syscalltrace);
	up->syscalltrace = nil;
	up->alarm = 0;
	if (up->tt)
		timerdel(up);
	pt = proctrace;
	if(pt)
		pt(up, SDead, 0, 0);

	/* nil out all the resources under lock (free later) */
	qlock(&up->debug);
	fgrp = up->fgrp;
	up->fgrp = nil;
	egrp = up->egrp;
	up->egrp = nil;
	rgrp = up->rgrp;
	up->rgrp = nil;
	pgrp = up->pgrp;
	up->pgrp = nil;
	dot = up->dot;
	up->dot = nil;
	slash = up->slash;
	up->slash = nil;
	qunlock(&up->debug);

	if(fgrp)
		closefgrp(fgrp);
	if(egrp)
		closeegrp(egrp);
	if(rgrp)
		closergrp(rgrp);
	if(dot)
		cclose(dot);
	if(slash)
		cclose(slash);
	if(pgrp)
		closepgrp(pgrp);

	/*
	 * if not a kernel process and have a parent,
	 * do some housekeeping.
	 */
	if(up->kp == 0) {
		p = up->parent;
		if(p == 0) {
			if(exitstr == 0)
				exitstr = "unknown";
			panic("boot process died: %s", exitstr);
		}

		while(waserror())
			;

		wq = smalloc(sizeof(Waitq));
		poperror();

		wq->w.pid = up->pid;
		utime = up->time[TUser] + up->time[TCUser];
		stime = up->time[TSys] + up->time[TCSys];
		wq->w.time[TUser] = tk2ms(utime);
		wq->w.time[TSys] = tk2ms(stime);
		wq->w.time[TReal] = tk2ms(sys->ticks - up->time[TReal]);
		if(exitstr && exitstr[0])
			snprint(wq->w.msg, sizeof(wq->w.msg), "%s %d: %s",
				up->text, up->pid, exitstr);
		else
			wq->w.msg[0] = '\0';

		lock(&p->exl);
		/*
		 * Check that parent is still alive.
		 */
		if(p->pid == up->parentpid && p->state != Broken) {
			p->nchild--;
			p->time[TCUser] += utime;
			p->time[TCSys] += stime;
			/*
			 * If there would be more than 128 wait records
			 * processes for my parent, then don't leave a wait
			 * record behind.  This helps prevent badly written
			 * daemon processes from accumulating lots of wait
			 * records.
		 	 */
			if(p->nwait < 128) {
				wq->next = p->waitq;
				p->waitq = wq;
				p->nwait++;
				wq = nil;
				wakeup(&p->waitr);
			}
		}
		unlock(&p->exl);
		if(wq)
			free(wq);
	}

	if(!freemem)
		addbroken(up);

	qlock(&up->seglock);
	es = &up->seg[NSEG];
	for(s = up->seg; s < es; s++) {
		if(*s) {
			putseg(*s);
			*s = 0;
		}
	}
	qunlock(&up->seglock);

	lock(&up->exl);		/* Prevent my children from leaving waits */
	psunhash(up);
	up->pid = 0;
	up->sch = nil;
	wakeup(&up->waitr);
	unlock(&up->exl);

	for(f = up->waitq; f; f = next) {
		next = f->next;
		free(f);
	}

	/* release debuggers */
	qlock(&up->debug);
	if(up->pdbg) {
		wakeup(&up->pdbg->sleep);
		up->pdbg = 0;
	}
	qunlock(&up->debug);

	if(up->edf != nil){
		edfstop(up);
		edffree(up);
		up->edf = nil;
	}

	/* Sched must not loop for this lock */
	noschedlock(&procalloc);

	up->state = Moribund;
	sched();
	panic("pexit");
}

int
haswaitq(void *x)
{
	Proc *p;

	p = (Proc *)x;
	return p->waitq != 0;
}

int
pwait(Waitmsg *w)
{
	int cpid;
	Waitq *wq;

	if(!canqlock(&up->qwaitr))
		error(Einuse);

	if(waserror()) {
		qunlock(&up->qwaitr);
		nexterror();
	}

	lock(&up->exl);
	if(up->nchild == 0 && up->waitq == 0) {
		unlock(&up->exl);
		error(Enochild);
	}
	unlock(&up->exl);

	sleep(&up->waitr, haswaitq, up);

	lock(&up->exl);
	wq = up->waitq;
	up->waitq = wq->next;
	up->nwait--;
	unlock(&up->exl);

	qunlock(&up->qwaitr);
	poperror();

	if(w)
		memmove(w, &wq->w, sizeof(Waitmsg));
	cpid = wq->w.pid;
	free(wq);

	return cpid;
}

void
dumpaproc(Proc *p)
{
	uintptr bss;
	char *s;

	if(p == 0)
		return;

	bss = 0;
	if(p->seg[DSEG])
		bss = p->seg[DSEG]->top;

	s = p->psstate;
	if(s == 0)
		s = statename[p->state];
	print("%3d:%10s pc %#p dbgpc %#p  %8s (%s) ut %ld st %ld bss %#p"
		" qpc %#p nl %d nd %lud lpc %#p spc %#p pri %lud sch %ld\n",
		p->pid, p->text, p->pc, dbgpc(p), s, statename[p->state],
		p->time[0], p->time[1], bss, p->qpc, p->nlocks,
		p->delaysched, p->lastlock ? p->lastlock->pc : 0, p->spc, p->priority,
		p->sch ? (p->sch - scheds) : -1);
}

/*
 *  wait till all processes have flushed their mmu
 *  state about segement s
 */
void
procflushseg(Segment *s)
{
	int i, ns, nm, nwait;
	Proc *p;
	Mach *mp;

	/*
	 *  tell all processes with this
	 *  segment to flush their mmu's
	 */
	nwait = 0;
	for(i=0; (p = psincref(i)) != nil; i++) {
		if(p->state == Dead){
			psdecref(p);
			continue;
		}
		for(ns = 0; ns < NSEG; ns++){
			if(p->seg[ns] == s){
				p->newtlb = 1;
				for(nm = 0; nm < MACHMAX; nm++){
					if((mp = sys->machptr[nm]) == nil || !mp->online)
						continue;
					if(mp->proc == p){
						mp->mmuflush = 1;
						nwait++;
					}
				}
				break;
			}
		}
		psdecref(p);
	}

	if(nwait == 0)
		return;

	/*
	 *  wait for all processors to take a clock interrupt
	 *  and flush their mmu's
	 */
	for(i = 0; i < MACHMAX; i++){
		if((mp = sys->machptr[i]) == nil || !mp->online || mp == m)
			continue;
		while(mp->mmuflush)
			sched();
	}
}

void
scheddump(void*)
{
	Proc *p;
	Sched *sch;
	Schedq *rq;
	int i;
	Mach *mp;

	for(i = 0; i < Nsched; i++){
		sch = &scheds[i];
		if(sch->mp == nil)
			continue;
		print("sched[%ld]: nrdy %d runs %ld cs %ld dly %ld"
			" max dly %ld rbl %ld\n",
			sch - &scheds[0],
			sch->nrdy, sch->nruns, sch->ncs,
			sch->ndelayscheds, sch->nmaxdelayscheds, sch->nrebalance);
		for(rq = &sch->runq[Nrq-1]; rq >= sch->runq; rq--){
			if(rq->head == nil)
				continue;
			print("rq%ld:", rq - sch->runq);
			for(p = rq->head; p != nil; p = p->rnext)
				print(" %d(%lud)", p->pid, m->ticks - p->readytime);
			print("\n");
			delay(150);
		}
	}
	for(i = 0; i < MACHMAX; i++)
		if((mp = sys->machptr[i]) != nil && mp->online){
			p = mp->proc;
			print("m[%d] sch %ld up %d\n",
				mp->machno, mp->sch - scheds, p?p->pid:0);
		}
}

void
kproc(char *name, void (*func)(void *), void *arg)
{
	Proc *p;
	static Pgrp *kpgrp;

	p = newproc();
	p->psstate = 0;
	p->procmode = 0640;
	p->kp = 1;

	p->scallnr = up->scallnr;
	memmove(p->arg, up->arg, sizeof(up->arg));
	p->nerrlab = 0;
	p->slash = up->slash;
	if(p->slash)
		incref(p->slash);
	p->dot = up->dot;
	if(p->dot)
		incref(p->dot);

	memmove(p->note, up->note, sizeof(p->note));
	p->nnote = up->nnote;
	p->notified = 0;
	p->lastnote = up->lastnote;
	p->notify = up->notify;
	p->ureg = 0;
	p->dbgreg = 0;

	procpriority(p, PriKproc, 0);

	kprocchild(p, func, arg);

	kstrdup(&p->user, eve);
	kstrdup(&p->text, name);
	if(kpgrp == nil)
		kpgrp = duppgrp(nil, p->slash);
	p->pgrp = kpgrp;
	incref(kpgrp);

	memset(p->time, 0, sizeof(p->time));
	p->time[TReal] = sys->ticks;
	ready(p);
	/*
	 *  since the bss/data segments are now shareable,
	 *  any mmu info about this process is now stale
	 *  and has to be discarded.
	 *
	 * I don't think it's needed now. -nemo.
	p->newtlb = 1;
	mmuflush();
	 */
}

/*
 *  called splhi() by notify().  See comment in notify for the
 *  reasoning.
 */
void
procctl(Proc *p)
{
	Mreg s;
	char *state;

	switch(p->procctl) {
	case Proc_exitbig:
		spllo();
		pexit("Killed: Insufficient physical memory", 1);

	case Proc_exitme:
		spllo();		/* pexit has locks in it */
		pexit("Killed", 1);

	case Proc_traceme:
		if(p->nnote == 0)
			return;
		/* No break */

	case Proc_stopme:
		p->procctl = 0;
		state = p->psstate;
		p->psstate = "Stopped";
		/* free a waiting debugger */
		s = spllo();
		qlock(&p->debug);
		if(p->pdbg) {
			wakeup(&p->pdbg->sleep);
			p->pdbg = 0;
		}
		qunlock(&p->debug);
		splhi();
		p->state = Stopped;
		sched();
		p->psstate = state;
		splx(s);
		return;
	}
}

void
error(char *err)
{
	spllo();

	assert(up->nerrlab < NERR);
	kstrcpy(up->errstr, err, ERRMAX);
	setlabel(&up->errlab[NERR-1]);
	nexterror();
}

void
nexterror(void)
{
	gotolabel(&up->errlab[--up->nerrlab]);
}

void
exhausted(char *resource)
{
	char buf[ERRMAX];

	sprint(buf, "no free %s", resource);
	iprint("%s\n", buf);
	error(buf);
}

void
killbig(char *why)
{
	int i, x;
	Segment *s;
	ulong l, max;
	Proc *p, *kp;

	max = 0;
	kp = nil;
	for(x = 0; (p = psincref(x)) != nil; x++) {
		if(p->state == Dead || p->kp){
			psdecref(p);
			continue;
		}
		l = 0;
		for(i=1; i<NSEG; i++) {
			s = p->seg[i];
			if(s != nil)
				l += s->top - s->base;
		}
		if(l > max && ((p->procmode&0222) || strcmp(eve, p->user)!=0)) {
			if(kp != nil)
				psdecref(kp);
			kp = p;
			max = l;
		}
		else
			psdecref(p);
	}
	if(kp == nil)
		return;

	print("%d: %s killed: %s\n", kp->pid, kp->text, why);
	for(x = 0; (p = psincref(x)) != nil; x++) {
		if(p->state == Dead || p->kp){
			psdecref(p);
			continue;
		}
		if(p != kp && p->seg[DSEG] && p->seg[DSEG] == kp->seg[DSEG])
			p->procctl = Proc_exitbig;
		psdecref(p);
	}

	kp->procctl = Proc_exitbig;
	for(i = 0; i < NSEG; i++) {
		s = kp->seg[i];
		if(s != nil && canqlock(&s->lk)) {
			mfreeseg(s, s->base, s->top);
			qunlock(&s->lk);
		}
	}
	psdecref(kp);
}

/*
 *  change ownership to 'new' of all processes owned by 'old'.  Used when
 *  eve changes.
 */
void
renameuser(char *old, char *new)
{
	int i;
	Proc *p;

	for(i = 0; (p = psincref(i)) != nil; i++){
		if(p->user!=nil && strcmp(old, p->user)==0)
			kstrdup(&p->user, new);
		psdecref(p);
	}
}

/*
 *  time accounting called by clock() splhi'd
 */
void
accounttime(void)
{
	Proc *p;
	ulong n, per;
	static ulong nrun;

	p = m->proc;
	if(p) {
		nrun++;
		p->time[p->insyscall]++;
	}

	/* calculate decaying duty cycles */
	n = perfticks();
	per = n - m->perf.last;
	m->perf.last = n;
	per = (m->perf.period*(HZ-1) + per)/HZ;
	if(per != 0)
		m->perf.period = per;

	m->perf.avg_inidle = (m->perf.avg_inidle*(HZ-1)+m->perf.inidle)/HZ;
	m->perf.inidle = 0;

	m->perf.avg_inintr = (m->perf.avg_inintr*(HZ-1)+m->perf.inintr)/HZ;
	m->perf.inintr = 0;

	/* only one processor gets to compute system load averages */
	if(m->machno != 0)
		return;

	/*
	 * calculate decaying load average.
	 * if we decay by (n-1)/n then it takes
	 * n clock ticks to go from load L to .36 L once
	 * things quiet down.  it takes about 5 n clock
	 * ticks to go to zero.  so using HZ means this is
	 * approximately the load over the last second,
	 * with a tail lasting about 5 seconds.
	 */
	n = nrun;
	nrun = 0;
	n = (m->sch->nrdy+n)*1000;
	m->load = (m->load*(HZ-1)+n)/HZ;
}

