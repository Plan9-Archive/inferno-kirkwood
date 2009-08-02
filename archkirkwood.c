#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"
#include "io.h"

#include "../port/netif.h"
#include "etherif.h"
#include "../port/flashif.h"

static char *
devidstr(ulong v)
{
	switch(v) {
	case 0x0:	return "88F6180";
	case 0x1:	return "88F619[02]";
	case 0x2:	return "88F6281";
	}
	return "unknown";
}

void
archconfinit(void)
{
	conf.topofmem = 512*1024*124;

	m->cpuhz = 1200*1000*1000;
	m->delayloop = m->cpuhz/6000;  /* initial estimate */
	conf.devidstr = devidstr(*(ulong*)AddrDevid);
}

static void
p16(uchar *p, ulong v)
{
	*p++ = v>>8;
	*p++ = v>>0;
	USED(p);
}

static void
p32(uchar *p, ulong v)
{
	*p++ = v>>24;
	*p++ = v>>16;
	*p++ = v>>8;
	*p++ = v>>0;
	USED(p);
}

void
archetheraddr(Ether *e, GbeReg *reg, int queue)
{
	ulong nibble, ucreg;
	ulong tbloff, regoff;

	// TODO could get/set getconf("ethaddr")

	// set ea
	p32(e->ea, reg->macah);
	p16(e->ea+4, reg->macal);

	// accept frames on ea
	nibble = e->ea[5] & 0xf;
	tbloff = nibble / 4;
	regoff = nibble % 4;
	
	ucreg = reg->dfut[tbloff];
	ucreg &= 0xff << (8 * regoff);
	ucreg |= (0x01 | queue <<1) << (8*regoff);
	reg->dfut[tbloff] = ucreg;
}		

int
archether(int ctlrno, Ether *e)
{
	GbeReg* reg;

	
	switch(ctlrno) {
	case 0:
		strcpy(e->type, "kirkwood");
		e->ctlrno = ctlrno;
		e->itype = Irqlo;
		e->irq = IRQ0gbe0sum;
		e->nopt = 0;
		e->mbps = 1000;
		
		archetheraddr(e, GBE0REG, 0);
		return 1;
	}
	return -1;
}

/* LED/USB gpios */
enum
{
	SheevaOEValLow	= 1<<29,        /* USB_PWEN low */
	SheevaOEValHigh	= 1<<17,        /* LED pin high */
	SheevaOELow	= ~0,
	SheevaOEHigh	= ~0,
};

static void
gpioconf(ulong gpp0_oe_val, ulong gpp1_oe_val, ulong gpp0_oe, ulong gpp1_oe)
{
	GPIO0REG->dataout = gpp0_oe_val;
	GPIO0REG->dataoutena = gpp0_oe;

	GPIO1REG->dataout= gpp1_oe_val;
	GPIO1REG->dataoutena= gpp1_oe;
}


void
archreset(void)
{
	/* reset devices to initial state */
	gpioconf(SheevaOEValLow, SheevaOEValHigh, SheevaOELow, SheevaOEHigh);
}

void
archreboot(void)
{
	//dcflushall();
	CPUCSREG->rstout = RstoutSoft;
	CPUCSREG->softreset = ResetSystem;
	for(;;)
		spllo();
}

void
archconsole(void)
{
	uartconsole();
}

void
kbdinit(void)
{
}


void
archflashwp(Flash*, int)
{
}

/*
 * for ../port/devflash.c:/^flashreset
 * retrieve flash type, virtual base and length and return 0;
 * return -1 on error (no flash)
 */
int
archflashreset(int bank, Flash *f)
{
	if(bank != 0)
		return -1;
	f->type = "nand";
	f->addr = (void*)PHYSNAND;
	f->size = 0;	/* done by probe */
	f->width = 1;
	f->interleave = 0;

	return 0;
}
