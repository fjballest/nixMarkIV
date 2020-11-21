#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

void
cinit(void)
{
}

void
copen(Chan* chan)
{
	chan->flag &= ~CCACHE;
}

long
cread(Chan*, uchar*, long, vlong)
{
	return -1;
}

vlong
ceof(Chan*, vlong)
{
	return -1;
}

void
cremove(Chan*)
{
}

long
cwrite(Chan*, uchar*, long, vlong)
{
	return -1;
}

void
cflushed(Chan*)
{
}
