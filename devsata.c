/*
first attempt at driver for the sata controller.
kirkwood has two ports, on the sheevaplug with sata from newit only the second port is in use.

todo:
- keep track of supported features of ata drive.
- non-ncq dma.
- mark disk as invalid before doing identify.  make sure we check that disk is valid before operating on it.

- better support for ata commands, reading status bits after command.
- better detect ata support of drives
- detect whether packet command is accepted.  try if packet commands work.
- read ata/atapi signature in registers after reset?
- look at dcache flushes around dma
- interrupt coalescing
- support for general ata commands, e.g. smart, security
- use generic sd (or bs) interface (#S;  we need partitions)
- lba24-only drives
- abstract for multiple ports (add controller struct to functions)
- in satainit(), only start the disk init, don't wait for it to be ready?  faster booting, seems it takes controller/disk some time to init after reset.

for the future:
- power management
- support for port multiplier
- support for atapi (mcc scsi), but this chip cannot do dma for packet commands.
 */

#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

#include	"io.h"

static int satadebug = 0;
#define diprint	if(satadebug)iprint
#define dprint	if(satadebug)print

char Enodisk[] = "no disk";
char Etimeout[] = "timeout";

typedef struct Req Req;
typedef struct Resp Resp;
typedef struct Prd Prd;

/* edma request, set by us for reads & writes */
struct Req
{
	ulong	prdlo;	/* direct buffer when <= 64k, pointer to Prd for data up to 512k */
	ulong	prdhi;
	ulong	ctl;
	ulong	count;
	ulong	ata[4];
};
enum {
	/* Req.ctl */
	Rdev2mem	= 1<<0,
	Rdevtagshift	= 1,
	Rpmportshift	= 12,
	Rprdsingle	= 1<<16,
	Rhosttagshift	= 17,
};

/* edma response, set by edma with result of request */
struct Resp
{
	ulong	idflags;	/* 16 bit id, 16 bit flags */
	ulong	ts;		/* timestamp, reserved */
};

/* vectored i/o, only used for >64k requests */
struct Prd
{
	ulong	addrlo;		/* address of buffer, must be 2-byte aligned */
	ulong	flagcount;	/* low 16 bit is cound (0 means 64k), high 16 bit flags. */
	ulong	addrhi;		/* must be 0 */
	ulong	pad;
};
enum {
	/* Prd.flagcount */
	Endoftable	= 1<<31,
};

typedef struct Disk Disk;
struct Disk
{
	int	valid;
	char	serial[20+1];
	char	firmware[8+1];
	char	model[40+1];
	uvlong	sectors;
};

/*
 * with ncq we get 32 tags.  the controller has 32 slots, to write
 * commands to the device.  if a tag is available, there is also always
 * a slot available.  so we administer by tag.
 */
static uchar tags[32];
static int tagnext;
static int tagsinuse;
static int ntags;
static Rendez tagr;	/* protected by reqsl */

static Rendez reqsr[32];	/* protected by requiring to hold tag */
static volatile ulong reqsdone[32];
static QLock reqsl;

static volatile ulong atadone;	/* whether ata interrupt has occurred */
static Rendez atadoner;

static volatile ulong ataregs;	/* registers received from device */
static Rendez ataregsr;

/* for reqsdone and atadone */
enum {
	Rok,
	Rtimeout,
	Rfail,
};
static char *donemsgs[] = {
"ok",
"timeout",
"error",
};

static struct {
	ulong	serrorintrs;
} stats;

static Req *reqs;
static Resp *resps;
static Prd *prds;
static int reqnext;
static int respnext;
static Disk disk;

static Lock startil;		/* for access to startr & start, ilock */
static Rendez startr;		/* for kproc satastart to sleep on */
static volatile ulong start;	/* how to start, see below */
enum {
	StartReset	= 1<<0,
	StartIdentify	= 1<<1,
};


enum {
	/* edma config */
	ECFGncq		= 1<<5,		/* use ncq */
	ECFGqueue	= 1<<9,		/* use ata queue dma commands */

	/* edma interrupt error cause */
	Edeverr		= 1<<2,		/* device error */
	Edevdis		= 1<<3,		/* device disconnect */
	Edevcon		= 1<<4,		/* device connected */
	Eserror		= 1<<5,		/* serror set */
	Eselfdis	= 1<<7,		/* edma self disabled */
	Etransint	= 1<<8,		/* edma transport layer interrupt */
	Eiordy		= 1<<12,	/* edma iordy timeout */

	Erxlinkmask	= 0xf,		/* link rx control/data errors */
	Erxctlshift	= 13,
	Erxdatashift	= 17,
	Erxlinksatacrc	= 1<<0,		/* sata crc error */
	Erxlinkfifo	= 1<<1,		/* internal fifo error */
	Erxlinkresetsync= 1<<2,		/* link layer reset by SYNC from device */
	Erxlinkother	= 1<<3,		/* other link ctl errors */

	Etxlinkmask	= 0x1f,		/* link tx control/data errors */
	Etxctlshift	= 21,
	Etxdatashift	= 26,
	Etxlinksatacrc	= 1<<0,		/* sata crc error */
	Etxlinkfifo	= 1<<1,		/* internal fifo error */
	Etxlinkresetsync= 1<<2,		/* link layer reset by SYNC from device */
	Etxlinkdmat	= 1<<3,		/* link layer accepts DMAT from device */
	Etxcollision	= 1<<4,		/* collision with receive */

	Elinkmask	= Erxlinkmask<<Erxctlshift | Erxlinkmask<<Erxdatashift | Etxlinkmask<<Etxctlshift | Etxlinkmask<<Etxdatashift,
	Elinkerrmask	= Erxlinkmask<<Erxctlshift | Erxlinkmask<<Erxdatashift | Etxlinkmask<<Etxdatashift,

	Etransport	= 1<<31,	/* transport protocol error */

	/* edma command */
	EdmaEnable	= 1<<0,		/* enable edma */
	EdmaAbort	= 1<<1,		/* abort and disable edma */
	Atareset	= 1<<2,		/* reset sata transport, link, physical layers */
	EdmaFreeze	= 1<<4,		/* do not process new requests from queue */

	/* edma status */
	Tagmask		= (1<<5)-1,	/* of last commands */
	Dev2mem		= 1<<5,		/* direction of command */
	Cacheempty	= 1<<6,		/* edma cache empty */
	EdmaIdle	= 1<<7,		/* edma idle (but disk can have command pending) */

	/* host controller main interrupt */
	Sata0err	= 1<<0,
	Sata0done	= 1<<1,
	Sata1err	= 1<<2,
	Sata1done	= 1<<3,
	Sata0dmadone	= 1<<4,
	Sata1dmadone	= 1<<5,
	Satacoaldone	= 1<<8,

	/* host controller interrupt */
	Idma0done	= 1<<0,		/* new crpb in out queue */
	Idma1done	= 1<<1,
	Iintrcoalesc	= 1<<4,
	Idevintr0	= 1<<8,		/* ata interrupt when edma disabled */
	Idevintr1	= 1<<9,

	/* interface cfg */
	SSC		= 1<<6,		/* SSC enable */
	Gen2		= 1<<7,		/* gen2 enable */
	Comm		= 1<<8,		/* phy communication enable, override "DET" in SControl */
	Physhutdown	= 1<<9,		/* phy shutdown */
	Emphadj		= 1<<13,	/* emphasis level adjust */
	Emphtx		= 1<<14,	/* tx emphasis enable */
	Emphpre		= 1<<15,	/* pre-emphasis */
	Ignorebsy	= 1<<24,	/* ignore bsy in ata register */
	Linkrstenable	= 1<<25,

	/* SStatus */
	SDETmask	= 0xf,
	SDETnone	= 0,		/* no dev, no phy comm */
	SDETdev		= 1,		/* only dev present, no phy comm */
	SDETdevphy	= 3,		/* dev & phy comm */
	SDETnophy	= 4,		/* phy in offline mode (disabled or loopback) */
	SSPDmask	= 0xf<<4,
	SSPDgen1	= 1<<4,		/* gen 1 speed */
	SSPDgen2	= 2<<4,		/* gen 2 speed */

	/* SError */
	EM		= 1<<1,		/* recovered communication error */
	EN		= 1<<16,	/* phy ready state changed */
	EW		= 1<<18,	/* comm wake detected by phy */
	EB		= 1<<19,	/* 10 to 8 bit decode error */
	ED		= 1<<20,	/* incorrect disparity */
	EC		= 1<<21,	/* crc error */
	EH		= 1<<22,	/* handshake error */
	ES		= 1<<23,	/* link sequence error */
	ET		= 1<<24,	/* transport state transition error */
	EX		= 1<<26,	/* device presence changed */
	
	/* SControl */
	CDETmask	= 0xf<<0,
	CDETnone	= 0<<0,		/* no device detection/initialisation */
	CDETcomm	= 1<<0,		/* perform interface communication initialisation */
	CDETdisphy	= 4<<0,		/* disable sata interface, phy offline */
	CSPDmask	= 0xf<<4,
	CSPDany		= 0<<4,		/* no speed limitation */
	CSPDgen1	= 1<<4,		/* <= gen1 */
	CSPDgen2	= 2<<4,		/* <= gen2 */
	CIPMmask	= 0xf<<8,
	CIPMany		= 0<<8,		/* no interface power management state restrictions */
	CIPMnopartial	= 1<<8,		/* no transition to PARTIAL */
	CIPMnoslumber	= 1<<9,		/* no transition to SLUMBER */
	CSPMmask	= 0xf<<12,
	CSPMnone	= 0<<12,	/* no different state for select power management */
	CSPMpartial	= 1<<12,	/* to PARTIAL */
	CSPMslumber	= 2<<12,	/* to SLUMBER */
	CSPMactive	= 3<<12,	/* to active */

	/* ata status */
	Aerr		= 1<<0,
	Adrq		= 1<<3,
	Adf		= 1<<5,
	Adrdy		= 1<<6,
	Absy		= 1<<7,
};


enum {
	Qdir, Qctlr, Qctl, Qdata,
};
static Dirtab satadir[] = {
	".",		{Qdir,0,QTDIR},	0,	0555,
	"sd01",		{Qctlr,0,QTDIR}, 0,	0555,
	"ctl",		{Qctl,0,0},	0,	0660,
	"data",		{Qdata,0,0},	0,	0660,
};


/* for reading numbers in response of "identify device" */
static ulong
g16(uchar *p)
{
	return ((ulong)p[0]<<8) | ((ulong)p[1]<<0);
}

static int
isdone(void *p)
{
	ulong *v = p;
	return *v != Rtimeout;
}

static int
notzero(void *p)
{
	ulong *v = p;
	return *v != 0;
}

static int
tagfree(void *)
{
	return tagsinuse < ntags;
}

static int
tagsidle(void*)
{
	return tagsinuse == 0;
}

static long
min(long a, long b)
{
	return (a < b) ? a : b;
}

static void
satakick(ulong v)
{
	ilock(&startil);
	diprint("satakick, v %#lux\n", v);
	start = v;
	wakeup(&startr);
	iunlock(&startil);
}

static void
sataabort(void)
{
	int i;

	for(i = 0; i < nelem(reqsr); i++) {
		reqsdone[i] = Rfail;
		wakeup(&reqsr[i]);
	}
	atadone = Rfail;
	wakeup(&atadoner);

	/* xxx disable edma? */
}

/*
 * hc main intr & enable register cause interrupts.
 * main intr is read-only, the bits must be cleared in:
 * - hc intr
 * - edma error intr & enable
 *   - serror & intr enable
 *   - fis intr & enable
 */
static void
sataintr(Ureg*, void*)
{
	SatahcReg *hr = SATAHCREG;
	SataReg *sr = SATA1REG;
	ulong v, e, tag;
	ulong in, out;

	v = hr->intrmain;
	diprint("intr %#lux, main %#lux\n", hr->intr, v);
	if(v & Sata1err) {
		diprint("intre %#lux\n", sr->edma.intre);
		diprint("m 1err\n");

		e = sr->edma.intre;
		if(e & (Edevdis | Eiordy | Elinkerrmask | Etransport)) {
			/* unrecoverable error.  need ata reset to anything in future. */
			sataabort();
			/* xxx send hotplug disconnect event? */
			satakick(StartReset);
		} else if(e & Edeverr) {
			/* device to host fis, or set device bits fis received with ERR set.  during edma. */
			/* xxx propagate error, how to recover? */
			diprint("Edeverr\n");
			sataabort();
		}
		if(e & Edevcon) {
			/* device connected, hotplug */
			diprint("Edevcon\n");
			satakick(StartIdentify);
		}
		if(e & Eserror) {
			/* Serror set */
			diprint("Eserror, %08lux\n", sr->ifc.serror);
			sr->ifc.serror = ~0UL;
			stats.serrorintrs++;
		}
		if(e & Eselfdis) {
			/* edma disabled itself */
			/* xxx how to recover?  at least stop all activity and return error. */
			iprint("Eselfdis\n");
			sataabort();
			satakick(StartReset);
		}
		if(e & Etransint) {
			/* fis interrupt */
			sr->ifc.fisintr = 0;
			ataregs = 1;
			wakeup(&ataregsr);
		}

		sr->edma.intre = 0;
	}
	if(v & Sata1done) {
		if(hr->intr & Idma1done) {
			diprint("m 1dmadone\n");

			hr->intr = ~Idma1done;

			dcinv(resps, 32*sizeof resps[0]);
			in = (sr->edma.respin & MASK(8))/sizeof (Resp);
			out = (sr->edma.respout & MASK(8))/sizeof (Resp);
			for(;;) {
				if(in == out)
					break;

				/* determine which request is done, wakeup its caller. */
				tag = resps[out].idflags & MASK(5);
				/* xxx check for error in idflags?  we now handle error through Edeverr, and make all i/o fail... */

				reqsdone[tag] = Rok;
				wakeup(&reqsr[tag]);
				out = (out+1)%32;
				sr->edma.respout = (ulong)&resps[out];
			}
		}
		if(hr->intr & Idevintr1) {
			diprint("m 1ataintr\n");
			/* reading status clears the interrupt */
			regreadl(&ATA1REG->status);
			hr->intr = ~Idevintr1;
			atadone = Rok;
			wakeup(&atadoner);
		}
	}

	intrclear(Irqlo, IRQ0sata);
}

static void
pioget(uchar *p)
{
	AtaReg *a = ATA1REG;
	ulong v;
	int i;

	for(i = 0; i < 256; i++) {
		v = a->data;
		*p++ = v>>8;
		*p++ = v>>0;
	}
}

static void
pioput(uchar *p)
{
	AtaReg *a = ATA1REG;
	ulong v;
	int i;

	for(i = 0; i < 256; i++) {
		v = (ulong)*p++<<8;
		v |= (ulong)*p++<<0;
		a->data = v;
	}
}

static void
atawait(void)
{
	AtaReg *a = ATA1REG;

	while(a->status & Absy) {
		ataregs = 0;
		sleep(&ataregsr, notzero, &ataregs);
	}
}

/* properly deal with commands that do (not) generate interrupt, and do (not) read/write data. */
enum {
	Nodata, Host2dev, Dev2host,
};
static void
atacmd(uchar cmd, uchar feat, uchar sectors, ulong lba, uchar dev, int dir, uchar *data, int ms)
{
	AtaReg *a = ATA1REG;
	ulong v;
	char *msg;

	/* xxx sleep until edma is disabled or edma is idle (edma status, bit 7 (EDMAIdle).  then disable edma. */
	/* xxx assert that edma is disabled */

	dprint("ata, status %#lux\n", a->status);
	atawait();

	atadone = Rtimeout;
	a->feat = feat;
	a->sectors = sectors;
	a->lbalow = (lba>>0) & 0xff;
	a->lbamid = (lba>>8) & 0xff;
	a->lbahigh = (lba>>16) & 0xff;
	a->dev = dev;
	a->cmd = cmd;
	if(ms > 0) {
		tsleep(&atadoner, isdone, &atadone, ms);
		if(atadone != Rok) {
			msg = donemsgs[atadone];
			dprint("%s\n", msg);
			error(msg);
		}
	} else {
		sleep(&atadoner, isdone, &atadone);
	}
	v = a->status;

	if(v & Aerr)
		error("ata command failed");
	if(v & Adf)
		error("device fault");

	switch(dir) {
	case Nodata:
		break;
	case Host2dev:
		pioput(data);
		break;
	case Dev2host:
		pioget(data);
		break;
	}
}

static int
atacheck(ulong statusmask, ulong status)
{
	if((ATA1REG->status & statusmask) != status)
		return -1;
	return 0;
}

/* claim the sata controller.  must be called before doing ata commands, outside of edma. */
static void
sataclaim(void)
{
	SataReg *sr = SATA1REG;

	qlock(&reqsl);
	do {
		sleep(&tagr, tagsidle, nil);
	} while(tagsinuse > 0);
	SATA1REG->edma.cmd |= EdmaAbort;
	sr->ifc.fisintrena |= 1<<0;
}

static void
sataunclaim(void)
{
	SataReg *sr = SATA1REG;

	sr->ifc.fisintrena &= ~(1<<0);
	qunlock(&reqsl);
}

/* strip spaces in string.  at least western digital returns space-padded strings for "identify device". */
static void
strip(char *p)
{
	int i, j;

	for(i = 0; p[i] == ' '; i++)
		{}
	for(j = strlen(p)-1; j >= i && p[j] == ' '; j--)
		{}
	memmove(p, p+i, j+1-i);
	p[j+1-i] = 0;
}


/* output of "identify device", 256 16-bit words. */
enum {
	/* some words are only valid when bit 15 is 0 and bit 14 is 1. */
	Fvalidmask		= 3<<14,
	Fvalid			= 1<<14,

	/* capabilities.  these should be 1 for sata devices. */
	Fcaplba			= 1<<9,
	Fcapdma			= 1<<8,

	/* ata commands supported (word 82), enabled (word 85) */
	Feat0ServiceIntr	= 1<<8,
	Feat0Writecache		= 1<<5,
	Feat0Packet		= 1<<4,
	Feat0PowerMgmt		= 1<<3,
	Feat0SMART		= 1<<0,

	/* ata commands supported (word 83), enabled (word 86) */
	Feat1FlushCachExt	= 1<<13,
	Feat1FlushCache		= 1<<12,
	Feat1Addr48		= 1<<10,
	Feat1AAM		= 1<<9,		/* automatic acoustic management */
	Feat1SetFeatReq		= 1<<6,
	Feat1PowerupStandby	= 1<<5,
	Feat1AdvPowerMgmt	= 1<<3,

	/* ata commands supported (word 84), enabled (word 87) */
	Feat2WWName64		= 1<<8,
	Feat2Logging		= 1<<5,
	Feat2SMARTselftest	= 1<<1,
	Feat2SMARTerrorlog	= 1<<0,

	/* logical/physical sectors, word 106 */
	MultiLogicalSectors	= 1<<13,	/* multiple logical sectors per physical sector */
	LargeLogicalSectors	= 1<<12,	/* logical sector larger than 512 bytes */
	LogicalPerPhyslog2mask	= (1<<4)-1,

	/* sata capabilities, word 76 */
	SataCapNCQ		= 1<<8,
	SataCapGen2		= 1<<2,
	SataCapGen1		= 1<<1,

	/* nvcache capabilities, word 214 */
	NvcacheEnabled		= 1<<4,
	NvcachePMEnabled	= 1<<1,
	NvcachePMSup		= 1<<0,
};


typedef struct Atadev Atadev;
struct Atadev {
	ushort	major;
	ushort	minor;
	ushort	cmdset[6];	/* words 83-87.  first three are for "supported", last three for "enabled". */
	ushort	sectorflags;	/* word 106 */
	uvlong	wwn;		/* world wide name, unique.  0 if not supported. */
	ushort	sectorsize;	/* logical sector size */
	ushort	satacap;
	ushort	nvcachecap;
	ulong	nvcachelblocks;	/* in logical blocks */
	ushort	rpm;		/* 0 for unknown, 1 for non-rotating device, other for rpm */
};


static void
identify(void)
{
	uchar c;
	uchar buf[512];
	int i;
	ushort w;
	Atadev dev;

	atacmd(0xec, 0, 0, 0, 0, Dev2host, buf, 60*1000);
	if(atacheck(Absy|Adrdy|Adf|Adrq|Aerr, Adrdy) < 0)
		error("identify failed");

	c = 0;
	for(i = 0; i < 512; i++)
		c += buf[i];
	if(c != 0)
		error("check byte for 'identify device' response invalid");

	memmove(disk.serial, buf+10*2, sizeof disk.serial-1);
	memmove(disk.firmware, buf+23*2, sizeof disk.firmware-1);
	memmove(disk.model, buf+27*2, sizeof disk.model-1);
	strip(disk.serial);
	strip(disk.firmware);
	strip(disk.model);
	disk.sectors = 0;
	disk.sectors |= (uvlong)g16(buf+100*2)<<0;
	disk.sectors |= (uvlong)g16(buf+101*2)<<16;
	disk.sectors |= (uvlong)g16(buf+102*2)<<32;

	w = g16(buf+49*2);
	if((w & Fcapdma) == 0 || (w & Fcaplba) == 0)
		error("disk does not support dma and/or lba");

	w = g16(buf+75*2);
	ntags = 1 + (w&MASK(5));
	for(i = 0; i < ntags; i++)
		tags[i] = i;

	dev.major = g16(buf+80*2);
	dev.minor = g16(buf+81*2);
	for(i = 0; i < 6; i++) {
		dev.cmdset[i] = g16(buf+(82+i)*2);
	}
	if((dev.cmdset[1] & Fvalidmask) != Fvalid)
		dev.cmdset[1] = 0;
	if((dev.cmdset[2] & Fvalidmask) != Fvalid)
		dev.cmdset[2] = 0;
	if((dev.cmdset[3+2] & Fvalidmask) != Fvalid)
		dev.cmdset[3+2] = 0;

	if((dev.cmdset[3+1] & Feat1Addr48) == 0)
		error("disk does not have lba48 enabled");

	dev.sectorflags = g16(buf+106*2);
	dev.sectorsize = 512;
	if((dev.sectorflags & Fvalidmask) != Fvalid)
		dev.sectorflags = 0;
	if(dev.sectorflags & LargeLogicalSectors)
		dev.sectorsize = 2 * (g16(buf+117*2)<<16 | g16(buf+118*2)<<0);

	dev.wwn = 0;
	if((dev.cmdset[2] & Feat2WWName64) && (dev.cmdset[3+2] & Feat2WWName64))
		dev.wwn = (uvlong)g16(buf+108*2)<<48 | (uvlong)g16(buf+109*2)<<32 | (uvlong)g16(buf+110*2)<<16 | (uvlong)g16(buf+111*2)<<0;

	dev.satacap = g16(buf+76*2);
	if(dev.satacap == 0xffff)
		dev.satacap = 0;

	dev.nvcachecap = g16(buf+214*2);
	dev.nvcachelblocks = g16(buf+215*2)<<16 | g16(buf+216*2)<<0;

	dev.rpm = g16(buf+217*2);
	/* check for "reserved" range in ata8-acs, set to 0 unknown if so */
	if(dev.rpm > 1 && dev.rpm < 0x400 || dev.rpm == 0xffff)
		dev.rpm = 0;

if(satadebug) {
	dprint("model %q\n", disk.model);
	dprint("serial %q\n", disk.serial);
	dprint("firmware %q\n", disk.firmware);
	dprint("sectors %llud\n", disk.sectors);
	dprint("size %llud bytes, %llud gb\n", disk.sectors*512, disk.sectors*512/(1024*1024*1024));
	dprint("ata/atapi versions %hux/%hux\n", dev.major, dev.minor);
	dprint("sectorflags:%s%s\n",
		(dev.sectorflags & MultiLogicalSectors) ? " MultiLogicalSectors" : "",
		(dev.sectorflags & LargeLogicalSectors) ? " LargeLogicalSectors" : "");
	dprint("logical sector size %d\n", dev.sectorsize);
	dprint("sata cap:%s%s%s\n",
		(dev.satacap & SataCapNCQ) ? " ncq" : "",
		(dev.satacap & SataCapGen2) ? " 3.0gbps" : "",
		(dev.satacap & SataCapGen1) ? " 1.5gbps" : "");
	dprint("cmdset %04hux %04hux %04hux %04hux %04hux %04hux\n",
		dev.cmdset[0], dev.cmdset[1], dev.cmdset[2], dev.cmdset[3], dev.cmdset[4], dev.cmdset[5]);
	dprint("            disabled: %04hux %04hux %04hux\n",
		dev.cmdset[0] & ~dev.cmdset[3+0],
		dev.cmdset[1] & ~dev.cmdset[3+1],
		dev.cmdset[2] & ~dev.cmdset[3+2]);
	dprint("wwn %016llux\n", dev.wwn);
	dprint("nvcache cap:%s%s%s\n",
		(dev.nvcachecap & NvcacheEnabled) ? " NvcacheEnabled" : "",
		(dev.nvcachecap & NvcachePMEnabled) ? " NvcachePMEnabled" : "",
		(dev.nvcachecap & NvcachePMSup) ? " NvcachePMSup" : "");
	dprint("nvcache lblocks: %lud\n", dev.nvcachelblocks);
	dprint("rpm %hud\n", dev.rpm);
}
	satadir[Qdata].length = disk.sectors*512;
	//xxx satadir[Qdata].length = 2048000*512;
	disk.valid = 1;
}

static void
flush(void)
{
	AtaReg *a = ATA1REG;

	sataclaim();
	if(waserror()) {
		sataunclaim();
		nexterror();
	}

	atacmd(0xea, 0, 0, 0, 0, Nodata, nil, 0);
	/* xxx should log the lba48 sector that failed and perhaps try to flush the rest? */
	if(atacheck(Absy|Adrdy|Adf|Adrq|Aerr, Adrdy) < 0)
		error("flush cache ext failed");

	poperror();
	sataunclaim();
}

static void
satareset(void)
{
	SatahcReg *hr = SATAHCREG;
	SataReg *sr = SATA1REG;

	/* power up sata1 port */
	CPUCSREG->mempm &= ~Sata1mem;
	CPUCSREG->clockgate |= Sata1clock;
	regreadl(&CPUCSREG->clockgate);

	/* disable interrupts */
	hr->intrmainena = 0;
	sr->edma.intreena = 0;
	sr->ifc.serrintrena = 0;
	sr->ifc.fisintrena = 0;

	/* disable & abort edma, bdma */
	sr->edma.cmd = (sr->edma.cmd & ~EdmaEnable) | EdmaAbort;

	/* clear interrupts */
	hr->intr = ~0UL;
	sr->edma.intre = 0;
	sr->ifc.serror = ~0UL;
	sr->ifc.fisintr = 0;
	
	/* xxx more */

	/* xxx should set full register? */
	sr->ifc.ifccfg &= ~Physhutdown;

	/* xxx reset more registers */
	hr->cfg = (0xff<<0)		/* default mbus arbiter timeout value */
			| (1<<8)	/* no dma byte swap */
			| (1<<9)	/* no edma byte swap */
			| (1<<10)	/* no prdp byte swap */
			| (1<<16);	/* mbus arbiter timer disabled */
	hr->intrcoalesc = 0; /* raise interrupt after 0 completions (disable coalescing) */
	hr->intrtime = 0; /* number of clocks before asserting interrupt (disable coalescing) */
	hr->intr = 0;  /* clear */

	/* clock ticks to reach 1250ns (as specified for sata). */
	sr->edma.iordytimeout = 0xbc;
	if(CLOCKFREQ == 200*1000*1000)
		sr->edma.iordytimeout = 0xfa;
	sr->edma.cmddelaythr = 0;
	// sr->edma.haltcond = 

/* xxx should set windows correct too */
if(0) {
	hr->win[0].ctl = (1<<0)		/* enable window */
			| (0<<1)	/* mbus write burst limit.  0: no limit (max 128 bytes), 1: do not cross 32 byte boundary */
			| ((0 & 0x0f)<<4)	/* target */
			| ((0xe & 0xff)<<8)	/* target attributes */
			| ((0xfff & 0xffff)<<16);	/* size of window, number+1 64kb units */
	hr->win[0].base = 0x0 & 0xffff;
}

	reqs = xspanalloc(32*sizeof reqs[0], 32*sizeof reqs[0], 0);
	resps = xspanalloc(32*sizeof resps[0], 32*sizeof resps[0], 0);
	prds = xspanalloc(32*8*sizeof prds[0], 16, 0);
	if(reqs == nil || resps == nil || prds == nil)
		panic("satareset");
	memset(reqs, 0, 32*sizeof reqs[0]);
	memset(resps, 0, 32*sizeof resps[0]);
	memset(prds, 0, 32*8*sizeof prds[0]);
	reqnext = respnext = 0;

	intrenable(Irqlo, IRQ0sata, sataintr, nil, "sata");
}

static void
satastartreset(void)
{
	SataReg *sr = SATA1REG;

	diprint("before ata reset, sstatus %#lux\n", sr->ifc.sstatus);

	sr->edma.cmd |= Atareset;
	regreadl(&sr->edma.cmd);
	sr->edma.cmd &= ~Atareset;
	tsleep(&up->sleep, return0, nil, 200); // xxx needed?

	/* errata magic, to fix the phy.  see uboot code (no docs available).  */
	sr->ifc.phym3 = (sr->ifc.phym3 & ~0x78100000UL) | 0x28000000UL;
	sr->ifc.phym4 = (sr->ifc.phym4 & ~1) | (1<<16);
	sr->ifc.phym9g2 = (sr->ifc.phym9g2 & ~0x400fUL) | 0x00008UL; /* tx driver amplitude */
	sr->ifc.phym9g1 = (sr->ifc.phym9g1 & ~0x400fUL) | 0x00008UL; /* tx driver amplitude */
	tsleep(&up->sleep, return0, nil, 100); /* needed? */

	sr->ifc.serror = ~0UL;
	sr->ifc.serrintrena = EN|EX;

	/* get Etransint interrupt when fis "registers device to host" comes in, for ata commands waiting on Absy */
	sr->ifc.fisintrena = 0;
	sr->ifc.fiscfg = 1<<0;

	diprint("before phy init, sstatus %#lux, serror %#lux\n", sr->ifc.sstatus, sr->ifc.serror);

	sr->ifc.scontrol = CDETcomm|CSPDany|CIPMnopartial|CIPMnoslumber;
	regreadl(&sr->ifc.scontrol);
	sr->ifc.scontrol &= ~CDETcomm;
	regreadl(&sr->ifc.scontrol);
	diprint("after phy reset, sstatus %#lux\n", sr->ifc.sstatus);

	/* we'll get a connected interrupt now if something is connected */
	/* have to find out if hotplug works for sata 1.x */
}

static void
satastartidentify(void)
{
	sataclaim();
	if(waserror()) {
		sataunclaim();
		nexterror();
	}
	identify();
	poperror();
	sataunclaim();

	print("#S/sd01: %q, %lludGiB (%,llud bytes), %s Gb/s\n",
		disk.model,
		disk.sectors*512/(1024*1024*1024),
		disk.sectors*512,
		(SATA1REG->ifc.sstatus & SSPDgen2) ? "3.0" : "1.5");
}

static void
satastart(void*)
{
	ulong v;

	for(;;) {
		diprint("satastart sleep... start %#lux\n", start);
		sleep(&startr, notzero, &start);
		diprint("satastart wakeup... start %#lux\n", start);

		ilock(&startil);
		v = start;
		start = 0;
		iunlock(&startil);

		if(!waserror()) {
			if(v & StartReset)
				satastartreset();
			if(v & StartIdentify)
				satastartidentify();
			poperror();
		}
	}
}

static void
satainit(void)
{
	SatahcReg *hr = SATAHCREG;
	SataReg *sr = SATA1REG;

	diprint("satainit...\n");

	/* one tag by default, for devices without ncq.  changed in identify(). */
	tagnext = tagsinuse = 0;
	ntags = 1;
	tags[0] = 0;

	/* disable & abort edma */
	sr->edma.cmd = (sr->edma.cmd & ~EdmaEnable) | EdmaAbort;

	/* clear edma */
	sr->edma.reqbasehi = sr->edma.respbasehi = 0;
	sr->edma.reqin = 0;
	sr->edma.reqout = 0;
	sr->edma.respin = 0;
	sr->edma.respout = (ulong)&resps[0];

	/* clear & enable interrupts, to get "device connected" interrupts among others */
	hr->intrmainena = Sata1err|Sata1done;
	hr->intr = 0;
	sr->edma.intre = 0;
	sr->edma.intreena = ~(0UL | Etxlinkmask<<Etxctlshift);
	sr->ifc.serror = ~0UL;
	sr->ifc.serrintrena = EN|EX;
	sr->ifc.fisintrena = 0;
	sr->ifc.fiscfg = 0;

	start = 0;
	kproc("satastart", satastart, nil, 0);
	satakick(StartReset);
}

static void
prdfill(Prd *prd, uchar *buf, long n)
{
	long nn;

	for(;;) {
		prd->addrlo = (ulong)buf;
		nn = min(n, 64*1024);
		prd->flagcount = nn & ((1<<16)-1);
		n -= nn;
		prd->addrhi = 0;
		if(n == 0) {
			prd->flagcount |= Endoftable;
			break;
		}
		prd++;
		buf += 64*1024;
	}
}

enum {
	/* first param for io() */
	Read, Write,
};
static ulong
io(int t, void *buf, long nb, vlong off)
{
	SatahcReg *hr = SATAHCREG;
	SataReg *sr = SATA1REG;
	Req *rq;
	int i;
	ulong tag;
	ulong ns;
	ulong dev;
	ulong nslo, nshi;
	uvlong lba;
	ulong lbalo, lbahi;
	ulong cmds[] = {0x60, 0x61}; /* xxx this is for sata fpdma only, ncq */
	Prd *prd;
	char *msg;

	if(disk.valid == 0)
		error(Enodisk);

	if(nb < 0 || off < 0 || off % 512 != 0)
		error(Ebadarg);
	ns = nb/512;
	lba = off/512;
	if(nb == 0)
		return 0;
	if(nb % 512 != 0)
		error(Ebadarg);
	if((ulong)buf & 1)
		error(Ebadarg); /* fix, should alloc buffer and copy it afterwards? */

	if(lba == disk.sectors)
		return 0;
	if(lba > disk.sectors)
		error(Ebadarg);

	qlock(&reqsl);

	sleep(&tagr, tagfree, nil);
	tag = tags[tagnext];
	tagnext = (tagnext+1)%ntags;
	tagsinuse++;

	i = reqnext;
	rq = &reqs[i];
	reqnext = (reqnext+1)%32;

	rq->prdhi = 0;
	if(ns > 128) {
		prd = &prds[i*8];
		if(ns > 8*128)
			ns = 8*128;
		prdfill(prd, buf, ns*512);
		dcwb(prd, 8*sizeof prd[0]);
		rq->prdlo = (ulong)prd;
		rq->ctl = 0;
		rq->count = 0;
	} else {
		rq->prdlo = (ulong)buf;
		rq->ctl = Rprdsingle;
		rq->count = (ns*512) & ((1<<16)-1); /* 0 means 64k */
	}
	if(t == Read)
		rq->ctl |= Rdev2mem;
	rq->ctl |= tag<<Rdevtagshift;
	rq->ctl |= tag<<Rhosttagshift;

	lbalo = lba>>0 & MASK(24);
	lbahi = lba>>24 & MASK(24);
	nslo = ns>>0 & 0xff;
	nshi = ns>>8 & 0xff;
	dev = 1<<6;
	rq->ata[0] = cmds[t]<<16 | nslo<<24;  /* cmd, feat current */
	rq->ata[1] = lbalo<<0 | dev<<24;  /* 24 bit lba current, dev */
	rq->ata[2] = lbahi<<0 | nshi<<24;  /* 24 bit lba previous, feat ext/previous */
	rq->ata[3] = (tag<<3)<<0 | 0<<8;  /* sectors current (tag), previous */
	dcwbinv(rq, sizeof rq[0]);

	reqsdone[tag] = Rtimeout;
	sr->edma.reqin = (ulong)&reqs[reqnext];
	if((sr->edma.cmd & EdmaEnable) == 0) {
		/* xxx check for bsy in ata status register? */

		sr->edma.intre = 0;
		hr->intr = ~(Sata1err|Sata1done);

		sr->edma.cfg = (sr->edma.cfg & ~ECFGqueue) | ECFGncq;

		sr->ifc.fisintr = 0;

		sr->edma.cmd = EdmaEnable;
		regreadl(&sr->edma.cmd);
	}
	qunlock(&reqsl);

	/* xxx have to return tag on interrupt? */
	tsleep(&reqsr[tag], isdone, &reqsdone[tag], 60*1000);

	if(reqsdone[tag] != Rok) {
		/* xxx should do ata reset, or return the tag back to pool */
		msg = donemsgs[reqsdone[tag]];
		error(msg);
	}

	tags[(tagnext-tagsinuse+ntags) % ntags] = tag;
	tagsinuse--;
	wakeup(&tagr);

	return ns*512;
}

static long
ctl(char *buf, long n)
{
	Cmdbuf *cb;

	cb = parsecmd(buf, n);
	if(strcmp(cb->f[0], "debug") == 0) {
		if(cb->nf != 2)
			error(Ebadarg);
		satadebug = atoi(cb->f[1]);
		return n;
	}
	if(strcmp(cb->f[0], "reset") == 0) {
		if(cb->nf != 1)
			error(Ebadarg);
		satainit();
		return n;
	}
	if(strcmp(cb->f[0], "identify") == 0) {
		sataclaim();
		if(waserror()) {
			sataunclaim();
			nexterror();
		}

		identify();

		poperror();
		sataunclaim();

		return n;
	}
	if(strcmp(cb->f[0], "flush") == 0) {
		flush();
		return n;
	}
	error("bad ctl");
	return -1;
}


static int
satagen(Chan *c, char *name, Dirtab *, int, int s, Dir *dp)
{
	Dirtab *tab;
	int ntab;
	int i;

	tab = satadir;
	ntab = 1;
	if(s != DEVDOTDOT){
		tab = &tab[c->qid.path];
		if(c->qid.type & QTDIR)
			tab++;
		if(c->qid.path == Qctlr)
			ntab = 2;
		if(s >= ntab)
			return -1;
		tab += s;
	}
	if(name != nil) {
		isdir(c);
		for(i = 0; i < ntab; i++, tab++) {
			if(strcmp(name, tab->name) == 0) {
				devdir(c, tab->qid, tab->name, tab->length, eve, tab->perm, dp);
				return 1;
			}
		}
		return -1;
	}
	devdir(c, tab->qid, tab->name, tab->length, eve, tab->perm, dp);
	return 1;
}

static Chan*
sataattach(char *spec)
{
	return devattach('S', spec);
}

static Walkqid*
satawalk(Chan *c, Chan *nc, char **name, int nname)
{
	return devwalk(c, nc, name, nname, satadir, nelem(satadir), satagen);
}

static int
satastat(Chan *c, uchar *dp, int n)
{
	return devstat(c, dp, n, satadir, nelem(satadir), satagen);
}

static Chan*
sataopen(Chan *c, int omode)
{
	return devopen(c, omode, satadir, nelem(satadir), satagen);
}

static void	 
sataclose(Chan*)
{
}

static long	 
sataread(Chan *c, void *buf, long n, vlong off)
{
	char *p, *s, *e;
	long r, nn;

	if(c->qid.type & QTDIR)
		return devdirread(c, buf, n, satadir, nelem(satadir), satagen);

	switch((ulong)c->qid.path){
	case Qctl:
		if(disk.valid == 0)
			error(Enodisk);
		s = p = smalloc(1024);
		e = s+1024;
		p = seprint(p, e, "inquiry %q %q\n", "", disk.model); /* manufacturer unknown */
		p = seprint(p, e, "config serial %q firmware %q\n", disk.serial, disk.firmware);
		p = seprint(p, e, "geometry %llud %d\n", disk.sectors, 512);
		p = seprint(p, e, "part data %llud %llud\n", 0ULL, disk.sectors);
		USED(p);
		n = readstr(off, buf, n, s);
		free(s);
		return n;
	case Qdata:
		if(!iseve())
			error(Eperm);
		dcwbinv(buf, n);
		r = 0;
		while(r < n) {
			nn = io(Read, (uchar*)buf+r, n-r, off+r);
			if(nn == 0)
				break;
			r += nn;
		}
		return r;
	}
	error(Egreg);
	return 0;		/* not reached */
}

static long	 
satawrite(Chan *c, void *buf, long n, vlong off)
{
	long r, nn;

	switch((ulong)c->qid.path){
	case Qctl:
		if(!iseve())
			error(Eperm);
		return ctl(buf, n);
	case Qdata:
		if(!iseve())
			error(Eperm);
		dcwbinv(buf, n);
		r = 0;
		while(r < n) {
			nn = io(Write, (uchar*)buf+r, n-r, off+r);
			if(nn == 0)
				break;
			r += nn;
		}
		return r;
	}
	error(Egreg);
	return 0;		/* not reached */
}

static int
satawstat(Chan *c, uchar *dp, int n)
{
	Dir *d;

	if(!iseve() || c->qid.path != Qdata)
		error(Eperm);

	d = smalloc(sizeof(Dir)+n);
	if(waserror()) {
		free(d);
		nexterror();
	}

	n = convM2D(dp, n, &d[0], (char*)&d[1]);
	if(n == 0)
		error(Eshortstat);
	if(d->mode == ~0UL
		&& d->atime == ~0UL
		&& d->mtime == ~0UL
		&& d->length == ~0ULL
		&& d->name == nil
		&& d->uid == nil
		&& d->gid == nil
		&& d->muid == nil)
	{
		flush();
		return n;
	}
	error(Eperm);
	return 0;
}


Dev satadevtab = {
	'S',
	"sata",

	satareset,
	satainit,
	devshutdown,
	sataattach,
	satawalk,
	satastat,
	sataopen,
	devcreate,
	sataclose,
	sataread,
	devbread,
	satawrite,
	devbwrite,
	devremove,
	satawstat,
	devpower,
};
