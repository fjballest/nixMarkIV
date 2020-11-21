#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"

void*
nalloc(Nalloc *na)
{
	Nlink *n;
	int id;

	id = na->selfishid;
	if(id-- != 0)
		if(up != nil && up->selfish[id] != nil && canlock(&up->selfishlk)){
			n = nil;
			if(up->selfish[id] != nil){
				n = up->selfish[id];
				up->selfish[id] = n->nnext;
				up->nselfish[id]--;
				na->nselfallocs++;	/* race; but stat only */
			}
			unlock(&up->selfishlk);
			if(n != nil)
				goto found;
		}
	lock(na);
	n = na->free;
	if(n != nil){
		na->free = n->nnext;
		na->nfree--;
		na->nused++;
		na->nallocs++;
	}
	unlock(na);
	if(n == nil){
		if(na->alloc != nil)
			n = na->alloc();
		else
			n = malloc(na->elsz);
		if(n == nil)
			panic("nalloc: no memory");
		lock(na);
		n->nlist = na->all;
		na->all = n;
		na->nused++;
		na->nallocs++;
		unlock(na);
	}
found:
	if(na->init != nil)
		na->init(n);
	n->nnext = nil;
	return n;
}

void
nfree(Nalloc *na, Nlink *n)
{
	int id;

	id = na->selfishid;
	if(na->term != nil)
		na->term(n);
	n->nnext = nil;

	if(id-- != 0 && up != nil &&
	   up->nselfish[id] < na->nselfish && canlock(&up->selfishlk)){
		n->nnext = up->selfish[id];
		up->selfish[id] = n;
		up->nselfish[id]++;
		na->nselffrees++;		/* race; but stat only */
		unlock(&up->selfishlk);
		return;
	}
	lock(na);
	n->nnext = na->free;
	na->free = n;
	na->nused--;
	na->nfree++;
	na->nfrees++;
	unlock(na);
}

char*
nasummary(char *s, char *e, void *a)
{
	Nalloc *na;

	na = a;
	return seprint(s, e, "%ud/%ud %s %ud/%ud self allocs %ud/%ud self frees\n",
		na->nused, na->nused+na->nfree, na->tag,
		na->nselfallocs, na->nselfallocs+na->nallocs,
		na->nselffrees, na->nselffrees+na->nfrees);
}

void
nallocdump(void *x)
{
	Nlink *n;
	char buf[128];
	Nalloc *na;

	na = x;
	nasummary(buf, buf+sizeof buf, na);
	print("%s", buf);
	for(n = na->all ; n != nil; n = n->nlist)
		if(na->dump != nil)
			na->dump(na->tag, n);
		else
			print("%s %#p\n", na->tag, n);
}
