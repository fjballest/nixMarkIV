#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

/*
 * A prefix table close to the 9 mount table semantics.
 *
 * Locking:
 *	pgrp->ns locks the entire ns (rwlock).
 *	Mount rw locks are used only to sync with external union readers.
 *
 * Notes:
 *	.. is removed by cleaning up paths; paths of the form ../... are
 *	rewritten using the path of up->dot, so the kernel never has to deal with
 *	.. as a path element.
 *
 *	paths like ../... that come again into dot are still resolved using dot.
 *
 *	We use clwalk because lib9p can't just walk.
 *	up->slash is used by RFCNAMEG to recreate a clean ns.
 *	binds of the form bind X X are no longer needed and ignored.
 *
 *	Path's have .nels elements and .nres elements already resolved.
 *	That is used to know what's left to walk in a path.
 */

static char Eunmountslash[] = "can't unmount '/'";

int mntdebug;

Mount*
newmnt(char *el)
{
	Mount *mnt;

	mnt = smalloc(sizeof *mnt);
	mnt->ref = 1;
	if(el != nil)
		kstrdup(&mnt->elem, el);
	return mnt;
}

void
mntdump(Mount *mnt, int lvl)
{
	Mounted *m;

	if(mnt == nil){
		print("%><nil mnt>\n", lvl);
		return;
	}
	rlock(mnt);
	if(mnt->ismount)
		print("%#p %>[%s] r%d mnt %d\n", mnt, lvl,
			mnt->elem, mnt->ref, mnt->ismount);
	else
		print("%#p %>[%s] r%d\n", mnt, lvl, mnt->elem, mnt->ref);
	for(m = mnt->um; m != nil; m = m->next){
		if(m->child != nil)
			mntdump(m->child, lvl+1);
		else
			print("%#p %> -> %N %C %#ullx %s\n",
				m, lvl+1, m->to->path,
				m->to->dev->dc, m->to->qid.path,
				(m->flags&MCREATE)?" c":"");
	}
	runlock(mnt);
}

/*
 * The element becomes the owner of the given to/mnt
 */
static Mounted*
newmntel(Chan *to, Mount *mnt, int flags)
{
	Mounted *newel;

	if((to != nil && mnt != nil) || (to == nil && mnt == nil))
		panic("newmntel");
	newel = smalloc(sizeof *newel);
	newel->flags = (flags&~MORDER);
	newel->child = mnt;
	newel->to = to;
	return newel;
}

/*
 * Does not update parent pointers; callers must do it.
 */
static void
dupum(Mounted **elp, Mount *mnt, int justchans)
{
	Mounted *m, *nm;

	for(m = mnt->um; m != nil; m = m->next){
		if(justchans && m->to == nil)
			continue;
		nm = newmntel(m->to, m->child, m->flags);
		if(nm->to)
			incref(nm->to);
		else
			nm->child = dupmnt(nm->child);
		nm->next = *elp;
		*elp = nm;
		elp = &nm->next;
	}
}

Mount*
dupmnt(Mount *m)
{
	Mount *nm;
	Mounted *el;

	if(m == nil)
		return nil;
	nm = newmnt(m->elem);
	dupum(&nm->um, m, 0);
	for(el = nm->um; el != nil; el = el->next)
		if(el->child != nil)
			el->child->parent = nm;
	nm->ismount = m->ismount;
	return nm;
}

static void
putum(Mount *mnt)
{
	Mounted *el;

	while((el = mnt->um) != nil){
		mnt->um = el->next;
		if(el->to != nil){
			cclose(el->to);
			el->to = nil;
			mnt->ismount--;
		}else
			mntclose(el->child);
		free(el);
	}
}

void
mntclose(Mount *mnt)
{
	if(mnt == nil || decref(mnt) > 0)
		return;
	putum(mnt);
	free(mnt->elem);
	free(mnt);
}

static Mount*
mntwalk1(Mount *mnt, Path *p, int i, Chan **ncp)
{
	Mounted *m, *nm;
	Walkqid *wq;
	int nqid;
	Chan *clone;

	if(mnt == nil)
		panic("mntwalk1");
	for(m = mnt->um; i < p->nels && m != nil; m = m->next){
		if(ncp)
			*ncp = nil;
		if(m->child != nil){
			if(m->child->elem == nil)
				panic("mntwalk1: null elem");
			if(strcmp(m->child->elem, p->els[i]) ==  0)
				break;
			continue;
		}
		/*
		 * This is a departure from a std. prefix mount table
		 * to get the semantics right for Plan 9:
		 * If we are at a union and have Chans mounted before other
		 * name elements in the prefix we are walking...
		 */
		if(ncp == nil || mnt->um->next == nil || !(m->to->qid.type&QTDIR))
			continue;
		for(nm = m->next; nm != nil; nm = nm->next)
			if(nm->child != nil)
				break;
		if(nm == nil)
			continue;
		/*
		 * Instead of resolving the longest prefix,
		 * if there is at least one element in the chan bound before,
		 * commit to that choice.
		 */
		if(waserror())
			continue;
		wq = m->to->dev->walk(m->to, nil, p->els+i, p->nels-i);
		poperror();
		if(wq == nil)
			continue;
		nqid = wq->nqid;
		clone = wq->clone;
		wq->clone = nil;
		wqfree(wq);
		if(nqid < p->nels-i){
			if(nqid > 0)
				return nil;
			continue;
		}
		p->nres = p->nels;
		mntclose(clone->mnt);
		clone->mnt = nil;
		*ncp = clone;
		return mnt;
	}
	if(m != nil)
		return m->child;
	return nil;
}

/*
 * Updates path to indicate the resolved portion and returns in
 * *mp the mount entry where the rest of the path should be walked.
 * If there's any mounted suffix for path *hasmtpt is set to 1
 * to let the callers know if there are (mount) surprises on the rest of
 * the subtree. See sysfile.c:/^read to learn why that's needed.
 */
static int
mntwalk(Mount *mnt0, Path *p, Mount **mp, int ismount, int *hasmtpt, Chan **nc)
{
	int i, r;
	Mount *cmnt, *mnt;
	Mounted *m;

	*mp = mnt0;
	mnt = mnt0;
	r = 0;
	if(hasmtpt != nil)
		*hasmtpt = 0;
	for(i = 0; i < p->nels; i++){
		cmnt = mntwalk1(mnt, p, i, nc);
		if(cmnt == nil)
			break;
		if(nc != nil && *nc != nil)
			return p->nres;
		mnt = cmnt;
		if(ismount || mnt->ismount){
			*mp = mnt;
			r = i+1;
		}
	}
	if(!ismount)
		p->nres = r;
	if(hasmtpt != nil){
		for(m = (*mp)->um; m != nil; m = m->next)
			if(m->child != nil){
				*hasmtpt = 1;
				break;
			}
	}
	assert(*mp != nil);
	return i;
}

Chan*
clwalk(Chan *c, Path *p)
{
	Chan *nc;
	Walkqid *wq;

	/*
	 * the dev walk would return nil on errors, and hide the actual
	 * error.
	 */
	kstrcpy(up->errstr, Enonexist, ERRMAX);
	wq = c->dev->walk(c, nil, p->els+p->nres, p->nels-p->nres);
	if(wq == nil || wq->nqid < p->nels-p->nres){
		wqfree(wq);
		error(up->errstr);
	}
	nc = wq->clone;
	wq->clone = nil;
	wqfree(wq);
	p->nres = p->nels;
	mntclose(nc->mnt);
	nc->mnt = nil;
	if(nc->lchan != nil)
		panic("clwalk: later");
	return nc;
}

/*
 * Walk interface for the rest of the kernel.
 * Can't clone and then walk because lib9p does not support that.
 * The safe thing to do is to always clwalk.
 */
Chan*
walk(Chan *c, char **elems, int nelems)
{
	Chan *nc;
	Path *p;
	int i;

	assert(c->path);
	p = duppath(c->path);
	for(i = 0; i < nelems; i++)
		p = addelem(p, elems[i], 1);
	p->nres = c->path->nels;
	if(waserror()){
		pathclose(p);
		error(nil);
	}
	nc = clwalk(c, p);
	pathclose(nc->path);
	nc->path = p;
	poperror();
	return nc;
}

/*
 * About to mount before or after at an empty union.
 * Create an entry for the previous state of things:
 * Locate an ancestor with a non nil um and
 * replicate it here, but adjusting paths so they refer
 * to the path for mnt.
 */
static void
initunion(Mount *mnt)
{
	Mount *pmnt;
	Mounted *el, **elp;
	Path *w;
	int i, nres;
	Chan *oc;
	char *x;

	w = newpath(nil);

	if(waserror()){
		pathclose(w);
		return;
	}
	for(pmnt = mnt; pmnt != nil; pmnt = pmnt->parent){
		if(pmnt->ismount)
			break;
		addelem(w, pmnt->elem, 0);
	}
	if(pmnt == nil){
		poperror();
		pathclose(w);
		return;
	}

	/* reverse */
	for(i = 0; i < w->nels/2; i++){
		x = w->els[i];
		w->els[i] = w->els[w->nels-i-1];
		w->els[w->nels-i-1] = x;
	}

	for(elp = &mnt->um; *elp != nil; elp = &(*elp)->next)
		;
	wlock(mnt);
	if(waserror()){
		wunlock(mnt);
		nexterror();
	}
	dupum(elp, pmnt, 1);
	nres = w->nres;
	assert(w->nres < w->nels);
	while((el = *elp) != nil){
		w->nres = nres;
		if((oc = el->to) != nil){
			if(waserror()){
				*elp = el->next;
				cclose(oc);
				free(el);
				continue;
			}
			el->to = clwalk(oc, w);
			pathclose(el->to->path);
			el->to->path = duppath(oc->path);
			for(i = nres; i < w->nres; i++)
				addelem(el->to->path, w->els[i], 0);
			mnt->ismount++;
			cclose(oc);
			poperror();
		}else
			el->child->parent = mnt;
		elp = &(*elp)->next;
	}
	wunlock(mnt);
	poperror();		/* wlock */
	poperror();		/* w */
	pathclose(w);
}

static void
addmntel(Mount *mnt, Mounted *m, int flags)
{
	Mounted **elp, *em;

	elp = &mnt->um;
	if(flags == MAFTER)
		for(; *elp != nil; elp = &(*elp)->next)
			;
	em = *elp;

	if(m->to != nil && m->to->mnt != nil){
		wlock(mnt);
		dupum(elp, m->to->mnt, 0);
		wunlock(mnt);
		assert(m->next == nil);
		cclose(m->to);
		free(m);
	}else{
		wlock(mnt);
		m->next = *elp;
		*elp = m;
		wunlock(mnt);
	}
	for(m = *elp; m != em; m = m->next)
		if(m->to != nil)
			mnt->ismount++;
		else
			m->child->parent = mnt;
}

void
mntmount(Mount *mgrp, Path *p, Chan *to, int flags)
{
	int i;
	Mount *mnt, *nm;
	Mounted *m;

	i = mntwalk(mgrp, p, &mnt, 1, nil, nil);

	/* create entries for each elem in path not yet mounted */
	for(; i < p->nels; i++){
		nm = newmnt(p->els[i]);
		m = newmntel(nil, nm, flags);
		if(waserror()){
			mntclose(nm);
			free(m);
			nexterror();
		}
		addmntel(mnt, m, MBEFORE);
		poperror();
		mnt = nm;
	}

	/* add the new entry */
	m = newmntel(to, nil, flags);
	incref(to);
	if(waserror()){
		cclose(to);
		free(m);
		nexterror();
	}

	if((flags&MORDER) == MREPL)
		putum(mnt);
	else if(!mnt->ismount)
		initunion(mnt);
	addmntel(mnt, m, flags&MORDER);
	poperror();

}

Mount*
setslash(Mount *mgrp, Chan *slash)
{
	incref(slash);
	mntclose(mgrp);
	mgrp = newmnt("/");
	mgrp->um = newmntel(slash, nil, 0);
	return mgrp;
}

void
mntunmount(Mount *mgrp, Path *p, Chan *to)
{
	int i;
	Mount *mnt, *parent;
	Mounted *m, **l;

	DBG("cunmount %N %N\n", p, to?to->path:nil);

	i = mntwalk(mgrp, p, &mnt, 1, nil, nil);
	if(i != p->nels)
		error(Eunmountslash);
	if(!mnt->ismount)
		error(Eunmount);

	if(to != nil){
		/* unmount just to */
		for(l = &mnt->um; (m = *l) != nil; l = &m->next)
			if(m->to != nil && eqchan(m->to, to, 1))
				break;
		if(m == nil)
			error(Eunmount);
		wlock(mnt);
		mnt->ismount--;
		*l = m->next;
		wunlock(mnt);
		cclose(m->to);
		free(m);
		if(mnt->um != nil)	/* more entries left; don't collect */
			return;
	}

	/* unmount by releasing from parent list,
	 * and walk up collecting empty entries
	 */
	do{
		parent = mnt->parent;
		if(parent == nil)
			error(Eunmountslash);
		for(l = &parent->um; (m = *l) != nil && m->child != mnt; l = &m->next)
			;
		assert(m != nil);
		wlock(parent);
		if(m->to != nil)
			parent->ismount--;
		*l = m->next;
		mnt->parent = nil;
		wunlock(parent);
		mntclose(mnt);
		free(m);
		if(parent->um != nil || parent->parent == nil)
			break;
		mnt = parent;
	}while(mnt != nil);
}

/*
 * Use up->dot to resolve p if possible:
 * dot >= resolved part of p and dot <= p
 */
static Chan*
relative(Path *p)
{
	int n;
	Chan *nc, *dot;

	dot = up->dot;
	/*
	 * BUG: don't use dot for unions.
	 * doesn't work there because it's already resolved.
	 * The right fix would be to change clwalk to
	 * consider unions, but that's done by the mount table
	 * and not by clwalk.
	 */
	if(dot->mnt != nil)
		return nil;

	if(!prefixpath(p, dot->path))
		return nil;
	n = p->nels;
	p->nels = p->nres;
	if(!prefixpath(dot->path, p)){
		p->nels = n;
		return nil;
	}

	up->dot = walked(dot);
	if(up->dot != dot){
		DBG("relative: walked dot %N\n", up->dot->path);
		dot = up->dot;
	}

	p->nels = n;
	p->nres = dot->path->nels;
	if(dot->mnt == nil && (nc = walklater(dot, p)) != nil)
		return nc;
	nc = clwalk(dot, p);
	nc->mnt = dot->mnt;
	if(dot->mnt != nil)
		incref(dot->mnt);
	return nc;
}

/*
 * Resolve a path using the mount table, up->dot, or devtab,
 * relok is true if we can use dot to resolve it.
 * Returns always a non shared Chan (ref == 1).
 *
 * When iscreate, we return a chan only if the file to be created
 * is a mount point; but we may return nil otherwise.
 */
Chan*
mntlookup(Path *p, int relok, int iscreate)
{
	Chan *nc, *c;
	Mount *mp, *hd;
	Mounted *m;
	Dev *dev;
	char diag[128], *x;
	int n, ismtpt, hasmtpt;
	Rune r;

	rlock(&up->pgrp->ns);
	if(waserror()){
		runlock(&up->pgrp->ns);
		nexterror();
	}

	if(p->nels > 0 && *p->els[0] == '#'){
		runlock(&up->pgrp->ns);
		poperror();
		if(iscreate)
			return nil;
		x = p->els[0];
		n = chartorune(&r, x+1) + 1;
		if(r == 0)
			panic("install a correct cleanname");
		if(r == 'M' || (up->pgrp->noattach && utfrune("|decp", r) == nil))
			error(Enoattach);
		dev = devtabget(r, 1);
		if(dev == nil)
			error(Ebadsharp);
		c = dev->attach(p->els[0]+n);
		p->nres++;
		if(waserror()){
			cclose(c);
			nexterror();
		}
		ismtpt = p->nres == p->nels;
		nc = clwalk(c, p);
		cclose(c);
		poperror();	/* c */

		nc->nsvers = up->pgrp->vers;
		goto out;
	}

	if(up->pgrp->mnt == nil || up->pgrp->mnt->um == nil)
		panic("namec: no mnt or um");
	nc = nil;
	ismtpt = 0;
	mntwalk(up->pgrp->mnt, p, &mp, 0, &hasmtpt, &nc);
	if(nc != nil)
		goto resolved;

	ismtpt = p->nres == p->nels;
	if(iscreate && !ismtpt){
		p->nres = 0;
		runlock(&up->pgrp->ns);
		poperror();
		return nil;
	}
	if(relok && (nc = relative(p)) != nil)
		goto resolved;

	/*
	 * if it's a union and p is fully resolved, will set nc->mnt for reads.
	 */
	hd = nil;
	if(mp->um != nil && mp->um->next != nil && p->nres == p->nels){
		incref(mp);
		hd = mp;
	}
	if(waserror()){
		if(hd != nil)
			mntclose(hd);
		nexterror();
	}

	nc = nil;
	diag[0] = 0;
	for(m = mp->um; m != nil; m = m->next){
		if(m->to == nil)
			continue;
		if(waserror()){
			if(diag[0] == 0)
				strecpy(diag, diag+sizeof diag, up->errstr);
			continue;
		}
		if(m->next != nil || (nc = walklater(m->to, p)) == nil)
			nc = clwalk(m->to, p);
		poperror();
		break;
	}
	if(m == nil){
		if(diag[0] == 0)
			seprint(diag, diag+sizeof diag, "%N: %s", p, Enonexist);
		error(diag);
	}
	assert(nc != nil);
	mntclose(nc->mnt);
	nc->mnt = hd;
	poperror();

resolved:
	nc->nsvers = up->pgrp->vers;
	runlock(&up->pgrp->ns);
	poperror();

out:
	nc->ismtpt = ismtpt;
	nc->hasmtpt = hasmtpt;
	if(nc->ref > 1)
		panic("mntlookup: shared %N", nc->path);
	return nc;
}

