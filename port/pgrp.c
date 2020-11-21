#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

enum
{
	Incr = 64,
};

static Ref pgrpid;

void
pgrpnote(ulong noteid, char *a, long n, int flag)
{
	int i;
	Proc *p;
	char buf[ERRMAX];

	if(n >= ERRMAX-1)
		error(Etoobig);

	memmove(buf, a, n);
	buf[n] = 0;
	for(i = 0; (p = psincref(i)) != nil; i++){
		if(p == up || p->state == Dead || p->noteid != noteid || p->kp){
			psdecref(p);
			continue;
		}
		qlock(&p->debug);
		if(p->pid == 0 || p->noteid != noteid){
			qunlock(&p->debug);
			psdecref(p);
			continue;
		}
		if(!waserror()) {
			postnote(p, 0, buf, flag);
			poperror();
		}
		qunlock(&p->debug);
		psdecref(p);
	}
}

Rgrp*
newrgrp(void)
{
	Rgrp *r;

	r = smalloc(sizeof(Rgrp));
	r->ref = 1;
	return r;
}

void
closergrp(Rgrp *r)
{
	if(decref(r) == 0)
		free(r);
}

void
closepgrp(Pgrp *p)
{
	int i;

	if(decref(p) != 0)
		return;

	qlock(&p->debug);
	wlock(&p->ns);
	p->pgrpid = -1;

	mntclose(p->mnt);
	p->mnt = nil;
	for(i = 0; i < p->nops; i++)
		free(p->ops[i]);
	if(p->naops > 0)
		free(p->ops);
	wunlock(&p->ns);
	qunlock(&p->debug);
	free(p);
}

Pgrp*
duppgrp(Pgrp *from, Chan *slash)
{
	Pgrp *to;
	int i;

	to = smalloc(sizeof(Pgrp));
	to->ref = 1;
	to->pgrpid = incref(&pgrpid);
	if(from != nil){
		rlock(&from->ns);
		to->mnt = dupmnt(from->mnt);
		to->noattach = from->noattach;
		if(from->nops > 0){
			to->ops = smalloc(from->naops * sizeof(char*));
			to->naops = from->naops;
			for(i = 0; i < from->nops; i++){
				to->ops[i] = nil;
				kstrdup(&to->ops[i], from->ops[i]);
			}
			to->nops = from->nops;
			to->vers = from->vers;
		}
		runlock(&from->ns);
	}else{
		if(slash == nil)
			to->mnt = newmnt("/");
		else
			to->mnt = setslash(to->mnt, slash);
		to->ops = smalloc(Incr * sizeof(char*));
		to->naops = Incr;
		to->ops[0] = nil;
		kstrdup(&to->ops[0], "bind #/ /");	/* BUG */
		to->nops = 1;
	}
	return to;
}

Fgrp*
dupfgrp(Fgrp *f)
{
	Fgrp *new;
	Chan *c;
	int i;

	new = smalloc(sizeof(Fgrp));
	if(f == nil){
		new->fd = smalloc(DELTAFD*sizeof(Chan*));
		new->nfd = DELTAFD;
		new->ref = 1;
		return new;
	}

	lock(f);
	/* Make new fd list shorter if possible, preserving quantization */
	new->nfd = f->maxfd+1;
	i = new->nfd%DELTAFD;
	if(i != 0)
		new->nfd += DELTAFD - i;
	new->fd = malloc(new->nfd*sizeof(Chan*));
	if(new->fd == nil){
		unlock(f);
		free(new);
		error("no memory for fgrp");
	}
	new->ref = 1;

	new->maxfd = f->maxfd;
	for(i = 0; i <= f->maxfd; i++) {
		if(c = f->fd[i]){
			incref(c);
			new->fd[i] = c;
		}
	}
	unlock(f);

	return new;
}

void
closefgrp(Fgrp *f)
{
	int i;
	Chan *c;

	if(f == 0)
		return;

	if(decref(f) != 0)
		return;

	/*
	 * If we get into trouble, forceclosefgrp
	 * will bail us out.
	 */
	up->closingfgrp = f;
	for(i = 0; i <= f->maxfd; i++){
		if(c = f->fd[i]){
			f->fd[i] = nil;
			cclose(c);
		}
	}
	up->closingfgrp = nil;

	free(f->fd);
	free(f);
}

/*
 * Called from sleep because up is in the middle
 * of closefgrp and just got a kill ctl message.
 * This usually means that up has wedged because
 * of some kind of deadly embrace with mntclose
 * trying to talk to itself.  To break free, hand the
 * unclosed channels to the close queue.  Once they
 * are finished, the blocked cclose that we've
 * interrupted will finish by itself.
 */
void
forceclosefgrp(void)
{
	int i;
	Chan *c;
	Fgrp *f;

	if(up->procctl != Proc_exitme || up->closingfgrp == nil){
		print("bad forceclosefgrp call");
		return;
	}

	f = up->closingfgrp;
	for(i = 0; i <= f->maxfd; i++){
		if(c = f->fd[i]){
			f->fd[i] = nil;
			ccloseq(c);
		}
	}
}

void
resrcwait(char *reason)
{
	char *p;

	if(up == nil)
		panic("resrcwait");

	p = up->psstate;
	if(reason) {
		up->psstate = reason;
		print("%s\n", reason);
	}

	tsleep(&up->sleep, return0, 0, 300);
	up->psstate = p;
}
