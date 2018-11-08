#include <u.h>
#include <libc.h>
#include "git.h"

Reprog *authorpat;

static int
charval(int c, int *err)
{
	if(c >= '0' && c <= '9')
		return c - '0';
	if(c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if(c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	*err = 1;
	return -1;
}

void *
emalloc(ulong n)
{
	void *v;
	
	v = mallocz(n, 1);
	if(v == nil)
		sysfatal("malloc: %r");
	setmalloctag(v, getcallerpc(&n));
	return v;
}

char*
estrdup(char *s)
{
	s = strdup(s);
	if(s == nil)
		sysfatal("strdup: %r");
	setmalloctag(s, getcallerpc(&s));
	return s;
}

int
Hfmt(Fmt *fmt)
{
	Hash h;
	int i, n, l;
	char c0, c1;

	l = 0;
	h = va_arg(fmt->args, Hash);
	for(i = 0; i < sizeof h.h; i++){
		n = (h.h[i] >> 4) & 0xf;
		c0 = (n >= 10) ? n-10 + 'a' : n + '0';
		n = h.h[i] & 0xf;
		c1 = (n >= 10) ? n-10 + 'a' : n + '0';
		l += fmtprint(fmt, "%c%c", c0, c1);
	}
	return l;
}

int
Tfmt(Fmt *fmt)
{
	Type t;
	int l;

	t = va_arg(fmt->args, Type);
	switch(t){
	case GNone:	l = fmtprint(fmt, "none");	break;
	case GCommit:	l = fmtprint(fmt, "commit");	break;
	case GTree:	l = fmtprint(fmt, "tree");	break;
	case GBlob:	l = fmtprint(fmt, "blob");	break;
	case GTag:	l = fmtprint(fmt, "tag");	break;
	case GOdelta:	l = fmtprint(fmt, "odelta");	break;
	case GRdelta:	l = fmtprint(fmt, "gdelta");	break;
	default:	l = fmtprint(fmt, "?%d?", t);	break;
	}
	return l;
}

int
Ofmt(Fmt *fmt)
{
	Object *o;
	int l;

	o = va_arg(fmt->args, Object *);
	print("== %H (%T) ==\n", o->hash, o->type);
	switch(o->type){
	case GTree:
		l = fmtprint(fmt, "tree\n");
		break;
	case GBlob:
		l = fmtprint(fmt, "blob %s\n", o->data);
		break;
	case GCommit:
		l = fmtprint(fmt, "commit\n");
		break;
	case GTag:
		l = fmtprint(fmt, "tag\n");
		break;
	default:
		l = fmtprint(fmt, "invalid: %d\n", o->type);
		break;
	}
	return l;
}

static int
objcmp(Avl *aa, Avl *bb)
{
	Object *a = (Object*)aa;
	Object *b = (Object*)bb;
	return memcmp(a->hash.h, b->hash.h, sizeof(a->hash.h));
}

void
gitinit(void)
{
	fmtinstall('H', Hfmt);
	fmtinstall('T', Tfmt);
	fmtinstall('O', Ofmt);
	inflateinit();
	authorpat = regcomp("[\t ]+(.*)[\t ]+([0-9]+)[\t ]+([\\-+][0-9]+)");
	objcache = avlcreate(objcmp);
}

int
hparse(Hash *h, char *b)
{
	int i, err;

	err = 0;
	for(i = 0; i < sizeof(h->h); i++){
		err = 0;
		h->h[i] = 0;
		h->h[i] |= ((charval(b[2*i], &err) & 0xf) << 4);
		h->h[i] |= ((charval(b[2*i+1], &err)& 0xf) << 0);
		if(err){
			werrstr("invalid hash");
			return -1;
		}
	}
	return 0;
}

int
slurpdir(char *p, Dir **d)
{
	int r, f;

	if((f = open(p, OREAD)) == -1)
		return -1;
	r = dirreadall(f, d);
	close(f);
	return r;
}

int
readall(int fd, char *buf, int nbuf)
{
	int l, n;

	l = 0;
	while(l != nbuf){
		n = read(fd, buf + l, nbuf - l);
		if(n == -1)
			return -1;
		if(n == 0)
			return l;
		l += n;
	}
	return l;
}

int
writeall(int fd, char *buf, int nbuf)
{
	int l, n;

	l = 0;
	while(l != nbuf){
		n = write(fd, buf + l, nbuf - l);
		if(n == -1)
			return -1;
		if(n == 0)
			return l;
		l += n;
	}
	return l;
}	

int
bappend(void *p, void *src, int len)
{
	Buf *b = p;
	char *n;

	while(b->len + len > b->sz){
		b->sz = b->sz*2 + 64;
		n = realloc(b->data, b->sz);
		if(n == nil)
			return -1;
		b->data = n;
	}
	memmove(b->data + b->len, src, len);
	b->len += len;
	return len;
}

int
breadc(void *p)
{
	return Bgetc(p);
}

int
decompress(void **p, Biobuf *b, vlong *csz)
{
	Buf d = {.len=0, .sz=0, .data=nil};

	if(bdecompress(&d, b, csz) == -1)
		return -1;
	*p = d.data;
	return d.len;
}

int
bdecompress(Buf *d, Biobuf *b, vlong *csz)
{
	vlong o;

	o = Boffset(b);
	if(inflatezlib(d, bappend, b, breadc) == -1){
		free(d->data);
		return -1;
	}
	if (csz)
		*csz = Boffset(b) - o;
	return d->len;
}

int
hassuffix(char *base, char *suf)
{
	int nb, ns;

	nb = strlen(base);
	ns = strlen(suf);
	if(ns > nb)
		return -1;
	if(strcmp(base + (nb - ns), suf) == 0)
		return 1;
	return 0;
}

int
swapsuffix(char *dst, int dstsz, char *base, char *oldsuf, char *suf)
{
	int bl, ol, sl, l;

	bl = strlen(base);
	ol = strlen(oldsuf);
	sl = strlen(suf);
	l = bl + sl - ol;
	if(l + 1 > dstsz || ol > bl)
		return -1;
	memmove(dst, base, bl - ol);
	memmove(dst + bl - ol, suf, sl);
	dst[l] = 0;
	return l;
}
