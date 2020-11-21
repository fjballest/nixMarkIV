#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

void
pshash(Proc *p)
{
	int h;

	h = p->pid % nelem(procalloc.ht);
	noschedlock(&procalloc);
	p->pidhash = procalloc.ht[h];
	procalloc.ht[h] = p;
	noschedunlock(&procalloc);
}

void
psunhash(Proc *p)
{
	int h;
	Proc **l;

	h = p->pid % nelem(procalloc.ht);
	noschedlock(&procalloc);
	for(l = &procalloc.ht[h]; *l != nil; l = &(*l)->pidhash)
		if(*l == p){
			*l = p->pidhash;
			break;
		}
	noschedunlock(&procalloc);
}

int
psindex(int pid)
{
	Proc *p;
	int h;
	int s;

	s = -1;
	h = pid % nelem(procalloc.ht);
	noschedlock(&procalloc);
	for(p = procalloc.ht[h]; p != nil; p = p->pidhash)
		if(p->pid == pid){
			s = p->index;
			break;
		}
	noschedunlock(&procalloc);
	return s;
}

Proc*
psincref(int i)
{
	/*
	 * Placeholder.
	 */
	if(i >= PROCMAX)
		return nil;
	return &procalloc.arena[i];
}

void
psdecref(Proc *p)
{
	/*
	 * Placeholder.
	 */
	USED(p);
}

void
psrelease(Proc* p)
{
	p->qnext = procalloc.free;
	procalloc.free = p;
}

Proc*
psalloc(void)
{
	Proc *p;

	noschedlock(&procalloc);
	for(;;) {
		if(p = procalloc.free)
			break;

		noschedunlock(&procalloc);
		resrcwait("no procs");
		noschedlock(&procalloc);
	}
	procalloc.free = p->qnext;
	noschedunlock(&procalloc);

	return p;
}

static void
procdump(void*)
{
	Mpl s;
	int i;
	Proc *p;

	s = spllo();
	for(i=0; (p = psincref(i)) != nil; i++) {
		if(p->state != Dead)
			dumpaproc(p);
		psdecref(p);
	}
	splx(s);
}

static void
sprocdump(void*)
{
	Mpl s;

	s = splhi();
	dumpstack();
	procdump(nil);
	splx(s);
}

extern void scheddump(void*);

static void
xdumpstack(void*)
{
	dumpstack();
}

void
psinit(void)
{
	Proc *p;
	int i;

	procalloc.free = malloc(PROCMAX*sizeof(Proc));
	if(procalloc.free == nil)
		panic("cannot allocate %ud procs (%udMB)\n", PROCMAX, PROCMAX*sizeof(Proc)/MiB);
	procalloc.arena = procalloc.free;

	p = procalloc.free;
	for(i=0; i<PROCMAX-1; i++,p++){
		p->qnext = p+1;
		p->index = i;
	}
	p->qnext = 0;
	p->index = i;

	addttescape('p', procdump, nil);
	addttescape('S', sprocdump, nil);
	addttescape('s', xdumpstack, nil);
	addttescape('q', scheddump, nil);
}
