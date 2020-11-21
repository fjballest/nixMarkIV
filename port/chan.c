#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

/*
 * Chan implementation relying on a prefix mount table.
 */

static void cainit(void*);
static void caterm(void*);
static void cadump(char*, void*);
static void waterm(void*);

static Lock fidalloclk;
static int fidalloc;

Nalloc chanalloc = 
{
	"chan",
	sizeof(Chan),
	Selfchan,		/* selfish per-proc alloc id */
	10,			/* up to 10 paths in selfish */
	cainit,
	caterm,
	nil,	/* alloc */
	cadump,
};

Nalloc wqalloc =
{
	"walkqid",
	sizeof(Walkqid),
	Selfwq,
	10,
	nil,		/* init */
	waterm,
};

Nalloc nmalloc =
{
	"name",
	sizeof(Name),
	Selfname,
	2,
};

static char Eempty[] = "empty file name";

static void
cadump(char *s, void *x)
{
	Chan *nc;

	nc = x;
	if(nc->flag == CFREE)
		print("%s %#p free\n", s, nc);
	else
		print("%s %#p '%N' qid %#ullx type %C lchan %#p ref %d\n",
			s, nc, nc->path, nc->qid.path,
			nc->dev?nc->dev->dc:'-', nc->lchan, nc->ref);
}

static void
waterm(void *x)
{
	Walkqid *wq;

	wq = x;
	if(wq->clone != nil){
		ccloseq(wq->clone);
		wq->clone = nil;
	}
}

Walkqid*
newwq(int nname)
{
	Walkqid *wq;

	wq = nalloc(&wqalloc);
	if(wq->naqid < nname){
		free(wq->qid);
		wq->naqid = ROUNDUP(nname, 8) * sizeof(Qid);
		wq->qid = malloc(wq->naqid * sizeof *wq->qid);
	}
	wq->nqid = 0;
	return wq;
}

void
wqfree(Walkqid *wq)
{
	if(wq != nil)
		nfree(&wqalloc, wq);
}

/*
 * Rather than strncpy, which zeros the rest of the buffer, kstrcpy
 * truncates if necessary, always zero terminates, does not zero fill,
 * and puts ... at the end of the string if it's too long.  Usually used to
 * save a string in up->genbuf;
 */
void
kstrcpy(char *s, char *t, int ns)
{
	int nt;

	nt = strlen(t);
	if(nt+1 <= ns){
		memmove(s, t, nt+1);
		return;
	}
	/* too long */
	if(ns < 4){
		/* but very short! */
		strncpy(s, t, ns);
		return;
	}
	/* truncate with ... at character boundary (very rare case) */
	memmove(s, t, ns-4);
	ns -= 4;
	s[ns] = '\0';
	/* look for first byte of UTF-8 sequence by skipping continuation bytes */
	while(ns>0 && (s[--ns]&0xC0)==0x80)
		;
	strcpy(s+ns, "...");
}

int
emptystr(char *s)
{
	if(s == nil)
		return 1;
	if(s[0] == '\0')
		return 1;
	return 0;
}

/*
 * Atomically replace *p with copy of s
 */
void
kstrdup(char **p, char *s)
{
	int n;
	char *t, *prev;

	n = strlen(s)+1;
	/* if it's a user, we can wait for memory; if not, something's very wrong */
	if(up){
		t = smalloc(n);
		setmalloctag(t, getcallerpc(&p));
	}else{
		t = malloc(n);
		if(t == nil)
			panic("kstrdup: no memory");
	}
	memmove(t, s, n);
	prev = *p;
	*p = t;
	free(prev);
}

static int
Tabfmt(Fmt *f)
{
	int n;

	n = va_arg(f->args, int);
	for(; n > 0; n--)
		fmtstrcpy(f, "\t");
	return 0;
}

static int
Tfmt(Fmt *f)
{
	Mount *m;

	m = va_arg(f->args, Mount*);
	if(m == nil)
		return fmtstrcpy(f, "<null>");
	if(m->parent == nil)
		return fmtstrcpy(f, "/");
	if(m->parent->parent == nil)
		return fmtprint(f, "/%s", m->elem);
	else
		return fmtprint(f, "%T/%s", m->parent, m->elem);
}

static int
Nfmt(Fmt *f)
{
	Path *p;
	int i;

	p = va_arg(f->args, Path*);
	if(p == nil)
		return fmtstrcpy(f, "''");
	if(p->nels == 0)
		return fmtstrcpy(f, "/");
	for(i = 0; i < p->nels; i++){
		if(i != 0 || *p->els[0] != '#')
			fmtstrcpy(f, "/");
		fmtstrcpy(f, p->els[i]);
	}
	return 0;
}


void
chaninit(void)
{
	extern Nalloc pathalloc;

	fmtinstall('N', Nfmt);
	fmtinstall('T', Tfmt);
	fmtinstall('>', Tabfmt);
	addsummary(nasummary, &chanalloc);
	addsummary(nasummary, &pathalloc);
	addsummary(nasummary, &wqalloc);
	addsummary(nasummary, &nmalloc);
	addttescape('c', nallocdump, &chanalloc);
}

static void
cainit(void *x)
{
	Chan *c;

	c = x;
	if(c->fid == 0){
		lock(&fidalloclk);
		c->fid = ++fidalloc;
		unlock(&fidalloclk);
	}
	c->ref = 1;
	/* if you get an error before associating with a dev,
	   close calls rootclose, a nop */
	memset(&c->Chanflds, 0, sizeof c->Chanflds);
}

Chan*
newchan(void)
{
	return nalloc(&chanalloc);
}

Name*
newname(int len)
{
	Name *n;

	n = nalloc(&nmalloc);
	if(n->na < len){
		free(n->s);
		n->na = ROUNDUP(len, 64);
		n->s = malloc(n->na);
	}
	n->s[0] = 0;
	return n;
}

void
freename(Name *n)
{
	if(n != nil)
		nfree(&nmalloc, n);
}

static void
chanclr(Chan *c)
{
	if(c->dirrock != nil){
		free(c->dirrock);
		c->dirrock = 0;
		c->nrock = 0;
		c->mrock = 0;
	}
	if(c->umc != nil){
		cclose(c->umc);
		c->umc = nil;
	}
	if(c->mux != nil){
		muxclose(c->mux);
		c->mux = nil;
	}
	if(c->mchan != nil){
		cclose(c->mchan);
		c->mchan = nil;
	}
	if(c->mnt != nil){
		mntclose(c->mnt);
		c->mnt = nil;
	}
	if(c->path != nil){
		pathclose(c->path);
		c->path = nil;
	}
	if(c->spec != nil){
		free(c->spec);
		c->spec = nil;
	}
	if(c->lchan != nil){
		cclose(c->lchan);
		c->lchan = nil;
	}
	if(c->mc != nil){
		putseg(c->mc);
		c->mc = nil;
	}
	c->dev = UINT2PTR(0x6c);	/* poison */
}

static void
caterm(void *x)
{
	Chan *c;

	c = x;
	c->flag = CFREE;
	chanclr(c);
}

void
chanfree(Chan *c)
{
	nfree(&chanalloc, c);
}

void
cclose(Chan *c)
{
	if(c->flag&CFREE)
		panic("cclose %#p", getcallerpc(&c));

	DBG("cclose %#p name=%N ref=%d\n", c, c->path, c->ref);
	if(decref(c))
		return;

	if(!waserror()){
		if(c->dev != nil)
			c->dev->close(c);
		poperror();
	}
	chanfree(c);
}

/*
 * Queue a chan to be closed by one of the clunk procs.
 */
struct {
	Chan	*head;
	Chan	*tail;
	int	nqueued;
	int	nclosed;
	int 	nprocs;
	Lock	l;
	QLock	q;
	Rendez	r;
} clunkq;

enum{Ncloseprocs = 4};
static void closeproc(void*);

void
ccloseq(Chan *c)
{
	int nprocs;

	if(c->flag&CFREE)
		panic("ccloseq %#p", getcallerpc(&c));

	DBG("ccloseq %#p name=%N ref=%d\n", c, c->path, c->ref);

	if(decref(c))
		return;

	lock(&clunkq.l);
	clunkq.nqueued++;
	c->cnext = nil;
	if(clunkq.head)
		clunkq.tail->cnext = c;
	else
		clunkq.head = c;
	clunkq.tail = c;
	unlock(&clunkq.l);

	if(!wakeup(&clunkq.r)){
		lock(&clunkq.l);
		nprocs = clunkq.nprocs;
		if(nprocs >= Ncloseprocs){
			unlock(&clunkq.l);
			return;
		}
		clunkq.nprocs++;
		unlock(&clunkq.l);
		kproc("closeproc", closeproc, nil);
	}	
}

static int
clunkwork(void*)
{
	return clunkq.head != nil;
}

static void
closeproc(void*)
{
	Chan *c;

	for(;;){
		qlock(&clunkq.q);
		if(clunkq.head == nil){
			if(!waserror()){
				tsleep(&clunkq.r, clunkwork, nil, 5000);
				poperror();
			}
			if(clunkq.head == nil){
				clunkq.nprocs--;
				qunlock(&clunkq.q);
				pexit("no work", 1);
			}
		}
		lock(&clunkq.l);
		c = clunkq.head;
		clunkq.head = c->cnext;
		clunkq.nclosed++;
		unlock(&clunkq.l);
		qunlock(&clunkq.q);
		if(!waserror()){
			if(c->dev != nil)
				c->dev->close(c);
			poperror();
		}
		chanfree(c);
	}
}

int
eqqid(Qid a, Qid b)
{
	return a.path == b.path && a.vers == b.vers;
}

int
eqchanddq(Chan *c, int dc, uint devno, Qid qid, int skipvers)
{
	if(c->qid.path != qid.path)
		return 0;
	if(!skipvers && c->qid.vers != qid.vers)
		return 0;
	if(c->dev->dc != dc)
		return 0;
	if(c->devno != devno)
		return 0;
	return 1;
}

Chan*
cclone(Chan *c)
{
	Chan *nc;
	Walkqid *wq;

	wq = c->dev->walk(c, nil, nil, 0);
	if(wq == nil)
		error("clone failed");
	nc = wq->clone;
	wq->clone = nil;
	wqfree(wq);
	pathclose(nc->path);
	nc->path = c->path;
	if(nc->path)
		incref(nc->path);
	nc->mnt = c->mnt;
	if(nc->mnt)
		incref(nc->mnt);
	nc->nsvers = c->nsvers;
	nc->ismtpt = c->ismtpt;
	nc->hasmtpt = c->hasmtpt;
	return nc;
}

/*
 * Return a cleaned absolute version for name.
 * It may be either name or a dup if it must grow.
 */
static Name*
cleanpname(Name *name)
{
	Path *path;
	Name *s;
	int n;

	/* init0 calls namec without up->dot */
	if(up->dot == nil || name->s[0] == '/' || name->s[0] == '#'){
		s = name;
	}else{
		path = up->dot->path;
		n = path->slen + 1 + strlen(name->s) + 1;
		s = newname(n);
		seprint(s->s, s->s+n, "%N/%s", path, name->s);
	}
	cleanname(s->s);
	return s;
}

int
eqchan(Chan *a, Chan *b, int skipvers)
{
	if(a->qid.path != b->qid.path)
		return 0;
	if(!skipvers && a->qid.vers!=b->qid.vers)
		return 0;
	if(a->dev != b->dev)
		return 0;
	return 1;
}

static Chan*
createdir(Chan *c)
{
	Chan *nc;
	Mount *mnt;
	Mounted *m;

	mnt = c->mnt;
	if(mnt == nil)	/* not a union, use it */
		return c;
	rlock(mnt);
	if(waserror()){
		runlock(mnt);
		nexterror();
	}
	for(m = mnt->um; m != nil; m = m->next)
		if(m->to != nil && (m->flags&MCREATE)){
			if(!eqchan(m->to, c, 1)){
				nc = cclone(m->to);
				cclose(c);
				c = nc;
				incref(mnt);
				mntclose(c->mnt);
				c->mnt = mnt;
			}
			break;
		}
	if(m == nil)
		error(Enocreate);
	runlock(mnt);
	poperror();
	return c;
}

void
updatedot(void)
{
	Chan *c, *wc;
	Path *p;
	ulong vers;

	if(up->dot->path == nil)
		panic("updatedot: nil path");
	vers = up->pgrp->vers;
	if(up->dot->nsvers != vers){
		DBG("update dot %N lchan %#p vers %uld nsvers %uld\n",
			up->dot->path, up->dot->lchan, up->dot->nsvers, vers);
		p = duppath(up->dot->path);
		p->nres = 0;
		if(waserror()){
			pathclose(p);
			nexterror();
		}
		c = mntlookup(p, 0, 0);
		if(waserror()){
			cclose(c);
			nexterror();
		}
		wc = walked(c);
		poperror();
		poperror();
		pathclose(wc->path);
		p->nres = p->nels;
		wc->path = p;
		cclose(up->dot);
		up->dot = wc;
		wc->nsvers = vers;
		if(up->dot->path == nil)
			panic("updatedot: no path");
	}
}

char*
dirname(uchar *p, int *n)
{
	p += BIT16SZ+BIT16SZ+BIT32SZ+BIT8SZ+BIT32SZ+BIT64SZ
		+ BIT32SZ+BIT32SZ+BIT32SZ+BIT64SZ;
	*n = GBIT16(p);
	return (char*)p+BIT16SZ;
}

long
dirsetname(char *name, int len, uchar *p, long n, long maxn)
{
	char *oname;
	int olen;
	long nn;

	if(n == BIT16SZ)
		return BIT16SZ;

	oname = dirname(p, &olen);

	nn = n+len-olen;
	PBIT16(p, nn-BIT16SZ);
	if(nn > maxn)
		return BIT16SZ;

	if(len != olen)
		memmove(oname+len, oname+olen, p+n-(uchar*)(oname+olen));
	PBIT16((uchar*)(oname-2), len);
	memmove(oname, name, len);
	return nn;
}

/*
 * Turn a name into a channel.
 * &name[0] is known to be a valid address.  It may be a kernel address.
 *
 * The result will always be the only reference to that particular fid.
 * The path in the result will be the one given unless amode is Abind,
 * in which case it will be the one kept for the file in the mount table.
 */
Chan*
namec(char *aname, int amode, int omode, int perm)
{
	Namec arg;

	arg.omode = omode;
	arg.perm = perm;
	return nameop(aname, amode, &arg);
}

static int
cancreate9pix(Chan *c)
{
	if(c->mnt != nil)
		return 0;
	if(c->dev == nil || c->dev->ncreate == nil)
		return 0;
	if(c->lchan != nil)
		return cancreate9pix(c->lchan);
	return c->mchan != nil && c->mchan->mux != nil && c->mchan->mux->sharedtags;
}

/*
 * General interface for the old namec
 */
Chan*
nameop(char *aname, int amode, Namec *arg)
{
	Name *name, *s;
	char *last, *nm, diag[128];
	Path *p;
	Chan *nc;
	int relok, omode, isdup;
	ulong perm;
	long nd;
	static char *amstr[] = {
	[Aaccess] "access",
	[Abind] "bind",
	[Atodir] "todir",
	[Aopen]	"open",
	[Amount] "mount",
	[Acreate] "create",
	[Aremove] "remove",
	[Awstat] "wstat",
	[Astat] "stat",
	};

	if(aname[0] == 0)
		error(Eempty);
	relok = (aname[0] != '#' && aname[0] != '/');
	if(relok)
		updatedot();

	/* must dup twice: first to verify and copy; second to clean it */
	name = validnamedup(aname, 1);
	s = cleanpname(name);
	p = newnpath(s);
	if(waserror()){
		DBG("namec: err: %s\n", up->errstr);
		if(s != name)
			freename(name);
		pathclose(p);
		nexterror();
	}

	last = nil;
	omode = 0;
	perm = 0;
	isdup = 0;
	if(arg){
		omode = arg->omode;
		perm = arg->perm;
	}
	if(amode == Acreate){
		if(p->nels == 0)
			error(Eexist);
		nc = mntlookup(p, relok, 1);
		if(nc != nil){
			/* It's a mount point, can only open OTRUNC */
			if(omode&OEXCL){
				cclose(nc);
				error(Eexist);
			}
			amode = Aopen;
			omode |= OTRUNC;
			goto Resolved;
		}
		/* play the create + walk + open dance */
		last = p->els[--p->nels];
	}

	DBG("namec pid %ud %s %s for p %N '%s'\n", up->pid, up->text, amstr[amode], p, aname);
	nc = mntlookup(p, relok, 0);
Resolved:
	if(waserror()){
		cclose(nc);
		nexterror();
	}
	poperror();
	if(waserror()){
		cclose(nc);
		nexterror();
	}
	switch(amode){
	case Aaccess:
		panic("call to retired namec(Aaccess) pc=%#p", getcallerpc(&aname));
		break;
	case Abind:
	case Amount:
	case Atodir:
		nc = walked(nc);
		poperror();
		if(waserror()){
			cclose(nc);
			nexterror();
		}
		if(amode == Atodir)
			isdir(nc);
		break;
	case Aremove:
		if(nc->ismtpt)
			error(Eismtpt);
		nc->dev->remove(nc);
		cclose(nc);
		goto None;
	case Awstat:
		if(nc->ismtpt)
			error(Eismtpt);
		arg->nd = nc->dev->wstat(nc, arg->d, arg->nd);
		cclose(nc);
		goto None;
	case Astat:
		nd = arg->nd;
		arg->nd = nc->dev->stat(nc, arg->d, nd);
		nm = pathlast(p);
		if(nm != nil)
			arg->nd = dirsetname(nm, strlen(nm), arg->d, arg->nd, nd);
		arg->ismtpt = nc->ismtpt;
		cclose(nc);
		goto None;
	case Acreate:
		/*
		 * It the chan comes from 'M' or 'λ', and speaks 9P2000.ix,
		 * the create operation in the protocol implies create|walk+open(OTRUNC).
		 * But don't do it if it's a union to avoid problems.
		 */
		if(cancreate9pix(nc)){
			omode |= OTRUNC;
			nc = nc->dev->ncreate(nc, last, omode&~(OEXCL|OCEXEC|OBEHIND), perm);
			poperror();	/* replace last waserror */
			if(waserror()){	/* with new using nc */
				cclose(nc);
				nexterror();
			}
			if(omode & OCEXEC)
				nc->flag |= CCEXEC;
			if(omode & ORCLOSE)
				nc->flag |= CRCLOSE;
			p->els[p->nels++] = last;
			nc = walked(nc);
			break;
		}
		nc = walked(nc);
		poperror();		/* replace last waserror */
		if(waserror()){		/* with new using nc */
			cclose(nc);
			nexterror();
		}
		if(!waserror()){
			nc = createdir(nc);
			nc->dev->create(nc, last, omode&~(OEXCL|OCEXEC|OBEHIND), perm);
			poperror();
			if(omode & OCEXEC)
				nc->flag |= CCEXEC;
			if(omode & ORCLOSE)
				nc->flag |= CRCLOSE;
			if(omode & OBEHIND)
				nc->flag |= CBEHIND;
			p->els[p->nels++] = last;
			break;
		}
		if(omode&OEXCL)
			nexterror();
		/* create failed; can try to walk and open,
		 * but we must look in the mount table again because the
		 * file might be anywhere if it's a union.
		 */
		strecpy(diag, diag+sizeof diag, up->errstr);
		p->els[++p->nels] = last;
		cclose(nc);
		nc = nil;
		USED(nc);
		poperror();		/* nc */
		if(waserror()){		/* new nc and use the right error str */
			if(strstr(diag, "already exist"))
				nexterror();
			else
				error(diag);
		}
		p->nres = 0;
		nc = mntlookup(p, relok, 0);
		if(waserror()){
			cclose(nc);
			nexterror();
		}
		poperror();
		omode |= OTRUNC;
		/* and now Aopen... */
	case Aopen:
		/*
		 * TODO: the cache must be the one ignoring files open
		 * for execing, but for interpreted files.
		 * This check must go.
		 * Only the mount driver is caching files, removing this
		 * means that caches must ignore binary files for OEXEC access.
		 */
		if(omode == OEXEC)
			nc->flag &= ~CCACHE;

		isdup = nc->dev->dc == 'd';
		nc = nc->dev->open(nc, omode&~(OCEXEC|OBEHIND));
		if(omode&OCEXEC)
			nc->flag |= CCEXEC;
		if(omode&ORCLOSE)
			nc->flag |= CRCLOSE;
		if(omode&OBEHIND)
			nc->flag |= CBEHIND;
		break;
	}

	/* keep original path names for #d files */
	if(isdup && nc->path != nil)
		pathclose(p);
	else if(amode != Abind){
		pathclose(nc->path);
		nc->path = p;
	}else if(nc->path == nil)
		nc->path = p;
	else
		pathclose(p);
	p = nc->path;
	poperror();	// nc


	/* for exec, mostly */
	if(p->nels == 0)
		kstrcpy(up->genbuf, ".", sizeof up->genbuf);
	else
		kstrcpy(up->genbuf, p->els[p->nels-1], sizeof up->genbuf);

	DBG("namec pid %d %s %s for '%s' '%N' mnt %s path %#ullx type %C lchan %#p\n",
		up->pid, up->text, amstr[amode], s->s, nc->path,
		nc->mnt?nc->mnt->elem:"-", nc->qid.path, nc->dev->dc, nc->lchan);

	if(s != name)
		freename(name);
	poperror();
	if(nc->path == nil || (nc->dev && nc->dev->dc ==  L'λ'))
		panic("namec: %N path or later chan", nc->path);
	return nc;
None:
	pathclose(p);
	poperror();
	if(s != name)
		freename(name);
	poperror();
	return nil;
}

/*
 * Name validity utils:
 *
 * Check that the name
 *  a) is in valid memory.
 *  b) is shorter than 2^16 bytes, so it can fit in a 9P string field.
 *  c) contains no frogs.
 * The first byte is known to be addressable by the requester, so the
 * routine works for kernel and user memory both.
 * The parameter slashok flags whether a slash character is an error
 * or a valid character.
 *
 * The parameter dup flags whether the string should be copied
 * out of user space before being scanned the second time.
 * (Otherwise a malicious thread could remove the NUL, causing us
 * to access unchecked addresses.)
 */

char isfrog[256]={
	/*NUL*/	1, 1, 1, 1, 1, 1, 1, 1,
	/*BKS*/	1, 1, 1, 1, 1, 1, 1, 1,
	/*DLE*/	1, 1, 1, 1, 1, 1, 1, 1,
	/*CAN*/	1, 1, 1, 1, 1, 1, 1, 1,
	['/']	1,
	[0x7f]	1,
};

static char*
validnameend(char *name)
{
	char *ename;

	if(ISKADDR(name))				/* hmmmm */
		ename = memchr(name, 0, (1<<16));
	else
		ename = vmemchr(name, 0, (1<<16));

	if(ename==nil || ename-name>=(1<<16))
		error("name too long");
	return ename;
}

static void
nofrog(char *name, int slashok)
{
	int c;
	Rune r;

	while(*name){
		/* all characters above '~' are ok */
		c = *(uchar*)name;
		if(c >= Runeself)
			name += chartorune(&r, name);
		else{
			if(isfrog[c] && (!slashok || c!='/')){
				snprint(up->genbuf, sizeof(up->genbuf), "%s: %q", Ebadchar, name);
				error(up->genbuf);
			}
			name++;
		}
	}
}

void
validname(char *name, int slashok)
{
	uintptr pc;

	pc = getcallerpc(&name);
	if(!ISKADDR(name))
		print("validname: called with user pointer pc %#p", pc);
	validnameend(name);
	nofrog(name, slashok);
}

Name*
validnamedup(char *name, int slashok)
{
	char *ename;
	Name *nm;
	int n;

	ename = validnameend(name);
	n = ename-name;
	nm = newname(n+1);
	memmove(nm->s, name, n);
	nm->s[n] = 0;
	if(waserror()){
		freename(nm);
		nexterror();
	}
	nofrog(nm->s, slashok);
	poperror();
	return nm; 
}

void
isdir(Chan *c)
{
	if(c->qid.type & QTDIR)
		return;
	error(Enotdir);
}

static void
addpgrpop(Pgrp *pgrp, char *s)
{
	char **x;

	if(pgrp->naops == pgrp->nops){
		x = smalloc((pgrp->naops+64)*sizeof(char*));
		if(pgrp->nops > 0)
			memmove(x, pgrp->ops, pgrp->nops*sizeof(char*));
		free(pgrp->ops);
		pgrp->ops = x;
		pgrp->naops += 64;
	}
	pgrp->ops[pgrp->nops++] = s;
}

static void
int2flag(char *s, int flag)
{
	if(flag == 0){
		*s = '\0';
		return;
	}
	*s++ = ' ';
	*s++ = '-';
	if(flag & MAFTER)
		*s++ = 'a';
	if(flag & MBEFORE)
		*s++ = 'b';
	if(flag & MCREATE)
		*s++ = 'c';
	if(flag & MCACHE)
		*s++ = 'C';
	*s = '\0';
}

static void
addop(Pgrp *pgrp, Chan *old, Chan *new, int flag)
{
	static char f[] = "mount -xxx";
	char flg[10];
	char *s, *e;
	int n;

	n = strlen(f) + 1 + old->path->slen;
	if(new != nil){
		n += 1 + new->path->slen;
		if(new->spec != nil)
			n += 1 + strlen(new->spec) + 1;
	}
	n++;
	s = smalloc(n);
	e = s+n;
	if(flag < 0){
		if(new == nil)
			seprint(s, e, "unmount %N", old->path);
		else
			seprint(s, e, "unmount %N %N", old->path, new->path);
	}else{
		int2flag(flg, flag);
		if(new->spec != nil && new->spec[0] != 0)
			seprint(s, e, "mount%s %N %N '%s'", flg, new->path, old->path, new->spec);
		else
			seprint(s, e, "bind%s %N %N", flg, new->path, old->path);
	}
	addpgrpop(pgrp, s);
}

void
cunmount(Chan *mnt, Chan *mounted)
{
	static char cmd[] = "unmount";

	wlock(&up->pgrp->ns);
	if(waserror()){
		wunlock(&up->pgrp->ns);
		nexterror();
	}

	mntunmount(up->pgrp->mnt, mnt->path, mounted);
	addop(up->pgrp, mnt, mounted, -1);
	up->pgrp->vers++;
	wunlock(&up->pgrp->ns);
	poperror();
}

int
cmount(Chan **newp, Chan *old, int flag)
{
	Chan *new;
	int order;
	static char cmd[] = "mount";
	static char *ostr[] = {
	[MBEFORE] "-b",
	[MAFTER] "-a",
	[MREPL] "",
	};

	new = *newp;
	if(QTDIR & (old->qid.type^new->qid.type))
		error(Emount);

	order = flag&MORDER;
	if((old->qid.type&QTDIR)==0 && order != MREPL)
		error(Emount);
	if(eqpath(old->path, new->path))
		return 1;

	wlock(&up->pgrp->ns);
	if(waserror()){
		wunlock(&up->pgrp->ns);
		nexterror();
	}

	mntmount(up->pgrp->mnt, old->path, new, flag);
	addop(up->pgrp, old, new, flag);
	up->pgrp->vers++;
	DBG("cmount %N %N %s\n", new->path, old->path, ostr[flag&MORDER]);
	if(0)
		mntdump(up->pgrp->mnt, 0);
	wunlock(&up->pgrp->ns);
	poperror();
	return 1;
}

