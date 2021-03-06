/*
 * hot-pluggable block storage devices.  not specific to scsi or ata.
 *
 * todo:
 * - check if plan 9 part start,end sectors are offset by 2 (for bootcode & table)
 * - allow misaligned writes, by reading the data before writing?
 * - add hooks so devices can tell they have gone/arrived.
 * - understand extended partitions
 * - hotplug & events, make event-file per Qdiskpart too.
 * - think of way to update the partition table.  for now, write to the data file, then reinit the device.
 */

#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"
#include	"part.h"
#include	"bs.h"

static char Enodisk[] = "no disk";
static char Enopart[] = "no such partition";
static char Ebadalign[] = "misaligned request";
static char Estale[] = "disk changed";

typedef struct Buf Buf;
struct Buf
{
	long	n;
	char	*a;
};

#define QTYPE(v)	((int)((v)>>0 & 0xff))
#define QDISK(v)	((int)((v)>>8 & 0xff))
#define QPART(v)	((int)((v)>>16 & 0xff))
#define QPATH(p,d,t)	((p)<<16 | (d)<<8 | (t)<<0)
enum{
	Qdir, Qbsctl, Qbsevent,
	Qdiskdir, Qdiskctl, Qdiskdevctl, Qdiskevent, Qdiskraw, Qdiskpart,
};
static
Dirtab bstab[] = {
	".",		{Qdir,0,QTDIR},		0,	0555,
	"bsctl",	{Qbsctl},		0,	0660,
	"bsevent",	{Qbsevent},		0,	0440,
	"xxx",		{Qdiskdir,0,QTDIR},	0,	0555,
	"ctl",		{Qdiskctl},		0,	0660,
	"devctl",	{Qdiskdevctl},		0,	0660,
	"event",	{Qdiskevent},		0,	0440,
	"raw",		{Qdiskraw},		0,	0660,
	"xxx",		{Qdiskpart},		0,	0660,
};

static struct {
	RWlock;
	Store**	disks;		/* indexed by Store.num */
	int	maxdisk;	/* highest index of disk */
} bs;

static Store*
diskget(int num)
{
	if(num >= 0 && num <= bs.maxdisk && bs.disks[num] != nil)
		return bs.disks[num];
	return nil;
}

enum {
	/* arg "wl" */
	Readlock	= 0,
	Writelock	= 1,
};
/* returns r or w-locked Store */
static Store*
diskgetlock(int num, int wl)
{
	Store *d;

	rlock(&bs);
	d = diskget(num);
	if(d == nil) {
		runlock(&bs);
		error(Enodisk);
	}
	if(wl)
		wlock(d);
	else
		rlock(d);
	runlock(&bs);
	return d;
}

static void
partdata(Part *p, vlong size)
{
	p->index = 0;
	p->typ = 0;
	strcpy(p->name, "data");
	p->s = 0;
	p->e = size;
	p->size = size;
	strcpy(p->uid, eve);
	p->perm = 0660;
	p->isopen = 0;
}

static void
diskinit(Store *d)
{
	d->ready = 0;
	d->nparts = 0;
	d->vers++;
	d->devinit(d);
	if(d->size & d->alignmask)
		error("misaligned size");

	d->parts = realloc(d->parts, sizeof d->parts[0]);
	partdata(&d->parts[0], d->size);
	d->nparts = 1;
	d->ready = 1;

	if(waserror()) {
		print("partinit: %s\n", up->env->errstr);
		return;
	}

	d->nparts = partinit(d->io, d, d->size, &d->parts);

	poperror();
}

static Part*
partget(Store *d, int i)
{
	if(i < 0 || i >= d->nparts)
		return nil;
	return &d->parts[i];
}

static Part*
xpartget(Store *d, int i)
{
	Part *p;

	if(d->ready == 0)
		error(Enodisk);
	p = partget(d, i);
	if(p == nil)
		error(Enopart);
	return p;
}

static long
io(Store *d, Part *p, int iswrite, void *buf, long n, vlong off)
{
	vlong s, e;
	long xn;
	vlong xs;
	char *origbuf;

	if(d->ready == 0)
		error(Enodisk);
	if(off < 0 || n < 0)
		error(Ebadarg);
	if(iswrite && (off&d->alignmask || n&d->alignmask))
		error(Ebadalign);

	s = off;
	e = off+n;
	if(s > p->size)
		s = p->size;
	if(e > p->size)
		e = p->size;
	s += p->s;
	e += p->s;
	n = e-s;

	xs = s&~d->alignmask;
	xn = n+(s&d->alignmask)+d->alignmask & ~d->alignmask;

	origbuf = nil;
	if(xs != s || xn != n) {
		origbuf = buf;
		buf = smalloc(xn);
		if(waserror()) {
			free(buf);
			nexterror();
		}
	}

	xn = d->io(d, iswrite, buf, xn, xs);

	if(origbuf != nil) {
		xn -= s-xs;
		if(xn < 0)
			xn = 0;
		if(n < xn)
			xn = n;
		poperror();
		memmove(origbuf, (uchar*)buf+(s-xs), xn);
		free(buf);
	}
	return xn;
}

static int
bsgen(Chan *c, char *, Dirtab *, int, int i, Dir *dp)
{
	Dirtab *t;
	Store *d;
	Part *p;
	Qid qid;
	int qt;

//print("bsgen, c->qid.path %#llux, i %d\n", c->qid.path, i);

	if(i == DEVDOTDOT) {
		switch(QTYPE(c->qid.path)) {
		case Qdir:
		case Qdiskdir:
			t = &bstab[Qdir];
			devdir(c, t->qid, t->name, t->length, eve, t->perm, dp);
			return 1;
		}
		panic("devdotdot on non-dir qid.path %#llux\n", c->qid.path);
	}

	switch(qt = QTYPE(c->qid.path)) {
	default:
		panic("bsgen on non-dir qid.path %#llux\n", c->qid.path);

	case Qdir:
	case Qbsctl:
	case Qbsevent:
		if(i == 0 || i == 1) {
			t = &bstab[Qbsctl+i];
			devdir(c, t->qid, t->name, t->length, eve, t->perm, dp);
			return 1;
		}
		i -= 2;
		if(i > bs.maxdisk)
			return -1;
		d = diskget(i);
		if(d == nil)
			return 0;
		t = &bstab[Qdiskdir];
		rlock(d);
		mkqid(&qid, QPATH(0,d->num,Qdiskdir), 0, QTDIR);
		devdir(c, qid, d->name, t->length, eve, t->perm, dp);
		runlock(d);
		return 1;

        case Qdiskdir:
	case Qdiskctl:
	case Qdiskdevctl:
	case Qdiskevent:
	case Qdiskraw:
	case Qdiskpart:
		d = diskget(QDISK(c->qid.path));
		if(d == nil || d->ready == 0 && qt == Qdiskpart)
			return -1;
		if(i >= 0 && i < Qdiskraw-Qdiskctl+1) {
			t = &bstab[Qdiskctl+i];
			mkqid(&qid, QPATH(0,d->num,Qdiskctl+i), 0, QTFILE);
			devdir(c, qid, t->name, t->length, eve, t->perm, dp);
			return 1;
		}
		i -= Qdiskraw-Qdiskctl+1;
		if(i >= d->nparts)
			return -1;
		p = &d->parts[i];
		mkqid(&qid, QPATH(i,d->num,Qdiskpart), 0, QTFILE);
		devdir(c, qid, p->name, p->size, p->uid, p->perm, dp);
		return 1;
	}
	return -1;
}


static void
bsreset(void)
{
	print("bsreset\n");
}

static void
bsinit(void)
{
	int i;
	Store *d;

	wlock(&bs);
	if(waserror()) {
		wunlock(&bs);
		nexterror();
	}

	for(i = 0; i <= bs.maxdisk; i++) {
		d = diskget(i);
		if(d == nil || d->ready)
			continue;
		wlock(d);
		if(waserror()) {
			wunlock(d);
			continue;
		}

		d->init(d);
		diskinit(d);

		poperror();
		wunlock(d);
	}

	poperror();
	wunlock(&bs);
}

static Chan*
bsattach(char* spec)
{
	return devattach('n', spec);
}

static Walkqid*
bswalk(Chan *c, Chan *nc, char **name, int nname)
{
	Walkqid *r;
	Store *d;
	int storelock;

	rlock(&bs);
	if(waserror()) {
		runlock(&bs);
		nexterror();
	}

	storelock = QTYPE(c->qid.path) >= Qdiskdir;
	if(storelock) {
		d = diskget(QDISK(c->qid.path));
		if(d == nil)
			error(Enodisk);
		rlock(d);
		if(waserror()) {
			runlock(d);
			nexterror();
		}

		r = devwalk(c, nc, name, nname, nil, 0, bsgen);

		poperror();
		runlock(d);
	} else
		r = devwalk(c, nc, name, nname, nil, 0, bsgen);

	poperror();
	runlock(&bs);
	return r;
}

static int
bsstat(Chan* c, uchar *db, int n)
{
	Store *d;
	int storelock;

	rlock(&bs);
	if(waserror()) {
		runlock(&bs);
		nexterror();
	}

	storelock = QTYPE(c->qid.path) >= Qdiskdir;
	if(storelock) {
		d = diskget(QDISK(c->qid.path));
		if(d == nil)
			error(Enodisk);
		rlock(d);
		if(waserror()) {
			runlock(d);
			nexterror();
		}

		n = devstat(c, db, n, nil, 0, bsgen);

		poperror();
		runlock(d);
	} else
		n = devstat(c, db, n, nil, 0, bsgen);

	poperror();
	runlock(&bs);
	return n;
}

static Chan*
bsopen(Chan* c, int omode)
{
	Part *p;
	Store *d;
	int storelock;
	void (*lockf)(RWlock *) = rlock;
	void (*unlockf)(RWlock *) = runlock;

	rlock(&bs);
	if(waserror()) {
		runlock(&bs);
		nexterror();
	}

	SET(d);
	storelock = QTYPE(c->qid.path) >= Qdiskdir;
	if(storelock) {
		if(QTYPE(c->qid.path) == Qdiskdir) {
			lockf = wlock;
			unlockf = wunlock;
		}
		d = diskget(QDISK(c->qid.path));
		if(d == nil)
			error(Enodisk);
		lockf(d);
		if(waserror()) {
			unlockf(d);
			nexterror();
		}
	}

	if(QTYPE(c->qid.path) == Qdiskpart) {
		p = xpartget(d, QPART(c->qid.path));
		if(p->index == 0)
			p = nil;
		else if(p->isopen)
			error(Einuse);

		c = devopen(c, omode, nil, 0, bsgen);
		p->isopen++;
	} else
		c = devopen(c, omode, nil, 0, bsgen);

	if(storelock) {
		c->qid.vers = d->vers;
		poperror();
		unlockf(d);
	}

	poperror();
	runlock(&bs);
	return c;
}

static int
bswstat(Chan* c, uchar *dp, int n)
{
	Store *d;
	Part *p;
	Dir dir;
	int nn;

	if(QTYPE(c->qid.path) != Qdiskpart)
		error(Eperm);

	nn = convM2D(dp, n, &dir, nil);
	if(nn == 0)
		error(Eshortstat);
	if(dir.atime != ~0 || dir.mtime != ~0 || dir.length != ~0 || *dir.name || *dir.gid || *dir.muid)
		error(Eperm);

	d = diskgetlock(QDISK(c->qid.path), Writelock);
	if(waserror()) {
		wunlock(d);
		nexterror();
	}

	p = xpartget(d, 0);
	if(!iseve())
		devpermcheck(p->uid, p->perm, OWRITE);
	if(*dir.uid)
		kstrcpy(p->uid, dir.uid, sizeof p->uid);
	if(dir.mode != ~0)
		p->perm = dir.mode&0666;

	poperror();
	wunlock(d);
	return n;
}

static void
bsclose(Chan* c)
{
	Store *d;
	Part *p;
	Buf *b;

	if(QTYPE(c->qid.path) == Qdiskpart && c->flag&COPEN && QPART(c->qid.path) != 0) {
		d = diskgetlock(QDISK(c->qid.path), Writelock);
		if(waserror()) {
			wunlock(d);
			nexterror();
		}

		if(c->qid.vers == d->vers) {
			p = xpartget(d, QPART(c->qid.path));
			if(p->isopen != 1)
				panic("devclose on part.isopen != 1");
			p->isopen = 0;
		}

		poperror();
		wunlock(d);
	}

	if(QTYPE(c->qid.path) == Qdiskraw && c->aux != nil) {
		b = c->aux;
		free(b->a);
		free(b);
		c->aux = nil;
	}
}

static long
bsdevread(Chan* c, void* a, long n, vlong off)
{
	Store *d;
	Part *p;
	char *buf, *s, *e;
	int i;

	d = diskgetlock(QDISK(c->qid.path), Readlock);
	if(waserror()) {
		runlock(d);
		nexterror();
	}

	if(c->qid.vers != d->vers)
		error(Estale);

	switch(QTYPE(c->qid.path)) {
	default:
		panic("bsdevread, qtype %x unhandled", QTYPE(c->qid.path));

	case Qdiskctl:
		buf = malloc(READSTR);
		if(buf == nil)
			error(Enomem);
		if(waserror()) {
			free(buf);
			poperror();
		}

		s = buf;
		e = buf+READSTR;
		s = seprint(s, e, "devtype %q\n", d->devtype);
		if(d->ready) {
			s = seprint(s, e, "descr %q\nalign %ud\nsize %lld\n",
				d->descr, d->alignmask+1, d->size);
			for(i = 1; i < d->nparts; i++) {
				p = &d->parts[i];
				s = seprint(s, e, "part %q %lld %lld\n", p->name, p->s, p->e);
			}
		}
		n = readstr(off, a, n, buf);

		poperror();
		free(buf);
		break;

	case Qdiskdevctl:
		if(!iseve()) {
			p = xpartget(d, 0);
			devpermcheck(p->uid, p->perm, OREAD);
		}
		n = d->rctl(d, a, n, off);
		break;

	case Qdiskpart:
		p = xpartget(d, QPART(c->qid.path));
		n = io(d, p, 0, a, n, off);
		break;
	}

	poperror();
	runlock(d);
	return n;
}

static long
bsread(Chan* c, void* a, long n, vlong off)
{
	Buf *b;
	Store *d;
	int storelock;
	char *buf, *s, *e;
	int i;

	switch(QTYPE(c->qid.path)){
	case Qdir:
	case Qdiskdir:
		rlock(&bs);
		if(waserror()) {
			runlock(&bs);
			nexterror();
		}

		storelock = QTYPE(c->qid.path) >= Qdiskdir;
		if(storelock) {
			d = diskget(QDISK(c->qid.path));
			if(d == nil)
				error(Enodisk);
			rlock(d);
			if(waserror()) {
				runlock(d);
				nexterror();
			}

			n = devdirread(c, a, n, nil, 0, bsgen);

			poperror();
			runlock(d);
		} else
			n = devdirread(c, a, n, nil, 0, bsgen);

		poperror();
		runlock(&bs);
		break;

	case Qbsctl:
		buf = smalloc(READSTR);
		s = buf;
		e = s+READSTR;
		if(waserror()) {
			free(buf);
			nexterror();
		}

		rlock(&bs);
		if(waserror()) {
			runlock(&bs);
			nexterror();
		}

		for(i = 0; i <= bs.maxdisk; i++) {
			d = bs.disks[i];
			if(d == nil)
				continue;
			if(d->ready)
				s = seprint(s, e, "%q, devtype %q, size %lld, partitions %d\n",
					d->name, d->devtype, d->size, d->nparts-1);
			else
				s = seprint(s, e, "%q, devtype %q\n", d->name, d->devtype);
		}
		n = readstr(off, a, n, buf);

		poperror();
		runlock(&bs);

		poperror();
		free(buf);
		break;

	case Qbsevent:
		error("not yet");
	case Qdiskctl:
	case Qdiskdevctl:
	case Qdiskpart:
		n = bsdevread(c, a, n, off);
		break;

	case Qdiskevent:
		error("not yet");

	case Qdiskraw:
		if(c->aux == nil)
			error("no command executed");
		b = c->aux;
		if(off > b->n)
			off = b->n;
		if(off+n > b->n)
			n = b->n-off;
		memmove(a, b->a+off, n);
		break;

	default:
		n = 0;
		break;
	}
	return n;
}

enum {
	CMinit,
};
static Cmdtab bsctl[] = {
	CMinit,		"init",		2,
};
enum {
	CMdiskinit, CMpartinit,
};
static Cmdtab bsdiskctl[] = {
	CMdiskinit,	"init",		1,
	CMpartinit,	"partinit",	1,
};

static long
bsdevwrite(Chan* c, void* a, long n, vlong off)
{
	Store *d;
	Part *p;
	Buf *b;

	d = diskgetlock(QDISK(c->qid.path), Readlock);
	if(waserror()) {
		runlock(d);
		nexterror();
	}

	if(c->qid.vers != d->vers)
		error(Estale);

	switch(QTYPE(c->qid.path)) {
	default:
		panic("bsdevwrite, qtype %x unhandled", QTYPE(c->qid.path));

	case Qdiskdevctl:
		if(off != 0)
			error(Ebadarg);
		if(!iseve()) {
			p = xpartget(d, 0);
			devpermcheck(p->uid, p->perm, OWRITE);
		}
		n = d->wctl(d, a, n);
		break;

	case Qdiskraw:
		if(d->ready == 0)
			error(Enodisk);

		if(!iseve()) {
			p = xpartget(d, 0);
			devpermcheck(p->uid, p->perm, OWRITE);
		}
		if(d->raw == nil)
			error("raw commands not implemented by device");
		b = c->aux;
		if(b != nil)
			free(b->a);
		else
			c->aux = b = smalloc(sizeof b[0]);
		b->a = nil;
		b->n = d->raw(d, a, n, off, &b->a);
		if(b->n < 0)
			n = b->n;
		break;

	case Qdiskpart:
		p = xpartget(d, QPART(c->qid.path));
		n = io(d, p, 1, a, n, off);
		break;
	}

	poperror();
	runlock(d);
	return n;
}

static long
bswrite(Chan* c, void* a, long n, vlong off)
{
	Cmdbuf *cb;
	Cmdtab *ct;
	Store *d;
	Part *p;
	int num;

	switch(QTYPE(c->qid.path)){
	case Qbsctl:
		if(!iseve())
			error(Eperm);

		wlock(&bs);
		if(waserror()) {
			wunlock(&bs);
			nexterror();
		}

		cb = parsecmd(a, n);
		if(waserror()) {
			free(cb);
			nexterror();
		}
		ct = lookupcmd(cb, bsctl, nelem(bsctl));
		switch(ct->index) {
		case CMinit:
			num = atoi(cb->f[1]);
			d = diskget(num);
			if(d == nil)
				error(Enodisk);
			wlock(d);
			if(waserror()) {
				wunlock(d);
				nexterror();
			}
			diskinit(d);
			poperror();
			wunlock(d);
			break;
		}
		poperror();
		free(cb);

		poperror();
		wunlock(&bs);
		break;

	case Qbsevent:
		error("not yet");

	case Qdiskctl:
		wlock(&bs);
		if(waserror()) {
			wunlock(&bs);
			nexterror();
		}

		d = diskget(QDISK(c->qid.path));
		if(d == nil)
			error(Enodisk);
		wlock(d);
		if(waserror()) {
			wunlock(d);
			nexterror();
		}

		if(c->qid.vers != d->vers)
			error(Estale);

		if(!iseve()) {
			p = xpartget(d, 0);
			devpermcheck(p->uid, p->perm, OWRITE);
		}

		cb = parsecmd(a, n);
		if(waserror()) {
			free(cb);
			nexterror();
		}
		ct = lookupcmd(cb, bsdiskctl, nelem(bsdiskctl));
		switch(ct->index) {
		case CMdiskinit:
			diskinit(d);
			break;

		case CMpartinit:
			if(d->ready == 0)
				error(Enodisk);

			d->parts = realloc(d->parts, sizeof d->parts[0]);
			d->nparts = 1;
			d->vers++;
			d->nparts = partinit(d->io, d, d->size, &d->parts);
			break;
		}
		poperror();
		free(cb);

		poperror();
		wunlock(d);

		poperror();
		wunlock(&bs);
		break;

	case Qdiskdevctl:
	case Qdiskraw:
	case Qdiskpart:
		n = bsdevwrite(c, a, n, off);
		break;

	default:
		error(Ebadusefd);
	}
	return n;
}

Dev bsdevtab = {
	'n',
	"bs",

	bsreset,
	bsinit,
	devshutdown,
	bsattach,
	bswalk,
	bsstat,
	bsopen,
	devcreate,
	bsclose,
	bsread,
	devbread,
	bswrite,
	devbwrite,
	devremove,
	bswstat,
};

void
blockstoreadd(Store *d)
{
	wlock(&bs);
	if(d->num > 99)
		panic("bad block store num %d", d->num);
	if(diskget(d->num) != nil)
		panic("disk.num %d already registered", d->num);

	if(d->num > bs.maxdisk) {
		bs.disks = realloc(bs.disks, sizeof bs.disks[0]*(d->num+1));
		if(bs.disks == nil)
			panic("no memory");
		while(bs.maxdisk < d->num)
			bs.disks[bs.maxdisk++] = nil;
	}
		
	bs.disks[d->num] = d;
	snprint(d->name, sizeof d->name, "bs%02d", d->num);
	wunlock(&bs);
}
