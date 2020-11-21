#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

static void paterm(void*);

Nalloc pathalloc =
{
	"path",
	sizeof(Path),
	Selfpath,		/* selfish per-proc alloc id */
	10,			/* up to 10 paths in selfish */
	nil,			/* no init */
	paterm,
};


enum{Dontkeep, Keep};

static Path*
xaddelem(Path *path, char *name, int dupok, int keepname)
{
	char *el, **new;
	Path *npath;
	int len;

	if(path->ref > 1){
		if(!dupok)
			panic("addelem: called from %#p", getcallerpc(&path));
		npath = duppath(path);
		pathclose(path);
		path = npath;
	}
	assert(path->ref == 1);
	if(path->nels == path->naels){
		new = smalloc((path->naels+16)*sizeof(char*));
		memmove(new, path->els, path->nels*sizeof path->els[0]);
		path->naels += 16;
		path->els = new;
	}
	len = strlen(name);
	if(keepname)
		el = name;
	else{
		el = smalloc(len+1);
		memmove(el, name, len);
		el[len] = 0;
	}
	path->els[path->nels] = el;
	path->nels++;
	path->slen += len+1;	/* +1 for the "/" */
	return path;
}

Path*
addelem(Path *path, char *name, int dupok)
{
	return xaddelem(path, name, dupok, Dontkeep);
}

static void
paterm(void *x)
{
	Path *p;
	int i;

	p = x;
	free(p->buf);
	p->buf = nil;
	freename(p->name);
	p->name = nil;
	for(i = p->nbuf; i < p->nels; i++)
		free(p->els[i]);
	p->nels = p->nres = p->nbuf = 0;
	p->slen = 1;	/* "/" */

	/* poison in case someone is using it */
	if(p->naels > 0)
		memset(p->els, 0x6c, p->naels * sizeof *p->els);
}

void
pathclose(Path *path)
{
	if(path != nil && decref(path) == 0)
		nfree(&pathalloc, path);
}

/*
 * The name given must be a clean name and it's
 * both modified and kept by the returned Path.
 */
Path*
newpath(char *name)
{
	char *p;
	int n;
	Path *path;
	Rune r;

	path = nalloc(&pathalloc);
	path->ref = 1;
	setmalloctag(path, getcallerpc(&name));
	if(name == nil)
		return path;

	path->buf = name;
	if(name[0] == '#'){
		for(p = name+1; *p != 0 && (*p != '/' || p == name+1); p += n)
			n = chartorune(&r, p);
		if(*p)
			*p++ = 0;
		xaddelem(path, name, 0, Keep);
		name = p;
	}else if(name[0] == '/')
		name++;

	for(; name != nil && name[0] != 0; name = p){
		p = utfrune(name, '/');
		if(p != nil)
			*p++ = 0;
		xaddelem(path, name, 0, Keep);
	}
	path->nbuf = path->nels;
	return path;
}

Path*
newnpath(Name *n)
{
	Path *path;

	path = newpath(n->s);
	path->buf = nil;
	path->name = n;
	return path;
}

int
eqpath(Path *p0, Path *p1)
{
	int i;

	if(p0 == p1)
		return 1;
	if(p0 == nil || p1 == nil || p0->nels != p1->nels)
		return 0;
	for(i = p0->nels-1; i >= 0; i--)
		if(strcmp(p0->els[i], p1->els[i]) != 0)
			return 0;
	return 1;
}

int
prefixpath(Path *p, Path *pref)
{
	int i;

	if(pref->nels > p->nels)
		return 0;
	for(i = pref->nels-1; i>= 0; i--)
		if(strcmp(p->els[i], pref->els[i]) != 0)
			return 0;
	return 1;
}

Path*
duppath(Path *s)
{
	int i;
	Path *d;

	d = newpath(nil);
	if(s->naels > 0){
		d->els = smalloc(s->naels*sizeof(char*));
		d->naels = s->naels;
	}
	for(i = 0; i < s->nels; i++){
		d->els[i] = nil;
		kstrdup(&d->els[i], s->els[i]);
	}
	d->nels = s->nels;
	d->nres = s->nres;
	d->slen = s->slen;
	return d;
}

char*
pathlast(Path *p)
{
	if(p == nil)
		return nil;
	if(p->nels == 0 || (p->nels == 1 && strcmp(p->els[0], "/") == 0))
		return "/";
	return p->els[p->nels-1];
}

