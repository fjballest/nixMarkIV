#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/edf.h"

static char Enoedf[] = "edf scheduling not implemented";
void
edfinit(Proc*)
{
}

void
edffree(Proc *p)
{
	free(p->edf);
	p->edf = nil;
}

char*
edfadmit(Proc*)
{
	return Enoedf;
}

int
edfready(Proc*)
{
	return 0;
}

/*
 * Unlike in Plan 9, this should check the admitted flag
 * and do as it pleases. The scheduler does not care about
 * edf flags.
 */
void
edfrecord(Proc*)
{
}

/*
 * Unlike in Plan 9, this should edflock if it pleases,
 * the scheduler does enough calling this.
 */
void
edfrun(Proc*, int)
{
}

void
edfstop(Proc*)
{
}

void
edfyield(void)
{
	yield();
}
