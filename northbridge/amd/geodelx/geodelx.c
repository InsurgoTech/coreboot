/*
 * This file is part of the LinuxBIOS project.
 *
 * Copyright (C) 2007 Advanced Micro Devices, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include <types.h>
#include <lib.h>
#include <console.h>
#include <post_code.h>
#include <device/device.h>
#include <device/pci.h>
#include <string.h>
#include <msr.h>
#include <io.h>
#include <amd_geodelx.h>
#include "geodelx.h"

/* here is programming for the various MSRs.*/
#define IM_QWAIT 0x100000

#define DMCF_WRITE_SERIALIZE_REQUEST (2<<12) /* 2 outstanding */	/* set in high nibl */
#define DMCF_SERIAL_LOAD_MISSES  (2)	/* enabled */

/* these are the 8-bit attributes for controlling RCONF registers
 * RCONF is Region CONFiguraiton, and controls caching and other
 * attributes of a region.  Just like MTRRs, only different.
  */
#define CACHE_DISABLE (1<<0)
#define WRITE_ALLOCATE (1<<1)
#define WRITE_PROTECT (1<<2)
#define WRITE_THROUGH (1<<3)
#define WRITE_COMBINE (1<<4)
#define WRITE_SERIALIZE (1<<5)

/* ram has none of this stuff */
#define RAM_PROPERTIES (0)
#define DEVICE_PROPERTIES (WRITE_SERIALIZE|CACHE_DISABLE)
#define ROM_PROPERTIES (WRITE_SERIALIZE|WRITE_PROTECT|CACHE_DISABLE)
#define MSR_WS_CD_DEFAULT (0x21212121)

/* RCONF registers 1810-1817 give you 8 registers with which to
 * program protection regions the are region configuration range
 * registers, or RRCF in msr terms, the are a straight base, top
 * address assign, since they are 4k aligned.
 */
/* so no left-shift needed for top or base */
#define RRCF_LOW(base,properties) (base|(1<<8)|properties)
#define RRCF_LOW_CD(base)	RRCF_LOW(base, CACHE_DISABLE)

/* build initializer for P2D MSR */
/* this is complex enough that you are going to need to RTFM if you really want to understand it. */
#define P2D_BM(msr, pdid1, bizarro, pbase, pmask) {msr, {.hi=(pdid1<<29)|(bizarro<<28)|(pbase>>24), .lo=(pbase<<8)|pmask}}
#define P2D_BMO(msr, pdid1, bizarro, poffset, pbase, pmask) {msr, {.hi=(pdid1<<29)|(bizarro<<28)|(poffset<<8)|(pbase>>24), .lo=(pbase<<8)|pmask}}
#define P2D_R(msr, pdid1, bizarro, pmax, pmin) {msr, {.hi=(pdid1<<29)|(bizarro<<28)|(pmax>>12), .lo=(pmax<<20)|pmin}}
#define P2D_RO(msr, pdid1, bizarro, poffset, pmax, pmin) {msr, {.hi=(pdid1<<29)|(bizarro<<28)|(poffset<<8)|(pmax>>12), .lo=(pmax<<20)|pmin}}
#define P2D_SC(msr, pdid1, bizarro, wen, ren,pscbase) {msr, {.hi=(pdid1<<29)|(bizarro<<28)|(wen), .lo=(ren<<16)|(pscbase>>18)}}
#define IOD_BM(msr, pdid1, bizarro, ibase, imask) {msr, {.hi=(pdid1<<29)|(bizarro<<28)|(ibase>>12), .lo=(ibase<<20)|imask}}
#define IOD_SC(msr, pdid1, bizarro, en, wen, ren, ibase) {msr, {.hi=(pdid1<<29)|(bizarro<<28), .lo=(en<<24)|(wen<<21)|(ren<<20)|(ibase<<3)}}

#define BRIDGE_IO_MASK (IORESOURCE_IO | IORESOURCE_MEM)

extern void graphics_init(void);
extern void cpu_bug(void);
extern void chipsetinit(void);
extern void print_conf(void);
extern u32 get_systop(void);

void northbridge_init_early(void);
void setup_realmode_idt(void);
void do_vsmbios(void);

struct msr_defaults {
	int msr_no;
	msr_t msr;
} msr_defaults[] = {
	{
		0x1700, {
	.hi = 0,.lo = IM_QWAIT}}, {
		0x1800, {
	.hi = DMCF_WRITE_SERIALIZE_REQUEST,.lo =
			    DMCF_SERIAL_LOAD_MISSES}},
	    /* 1808 will be done down below, so we have to do 180a->1817 (well, 1813 really) */
	    /* for 180a, for now, we assume VSM will configure it */
	    /* 180b is left at reset value,a0000-bffff is non-cacheable */
	    /* 180c, c0000-dffff is set to write serialize and non-cachable */
	    /* oops, 180c will be set by cpu bug handling in cpubug.c */
	    //{0x180c, {.hi = MSR_WS_CD_DEFAULT, .lo = MSR_WS_CD_DEFAULT}},
	    /* 180d is left at default, e0000-fffff is non-cached */
	    /* we will assume 180e, the ssm region configuration, is left at default or set by VSM */
	    /* we will not set 0x180f, the DMM,yet */
	    //{0x1810, {.hi=0xee7ff000, .lo=RRCF_LOW(0xee000000, WRITE_COMBINE|CACHE_DISABLE)}},
	    //{0x1811, {.hi = 0xefffb000, .lo = RRCF_LOW_CD(0xefff8000)}},
	    //{0x1812, {.hi = 0xefff7000, .lo = RRCF_LOW_CD(0xefff4000)}},
	    //{0x1813, {.hi = 0xefff3000, .lo = RRCF_LOW_CD(0xefff0000)}},
	    /* now for GLPCI routing */
	    /* GLIU0 */
	    P2D_BM(MSR_GLIU0_BASE1, 0x1, 0x0, 0x0, 0xfff80),
	    P2D_BM(MSR_GLIU0_BASE2, 0x1, 0x0, 0x80000, 0xfffe0),
	    P2D_SC(MSR_GLIU0_SHADOW, 0x1, 0x0, 0x0, 0xff03, 0xC0000),
	    /* GLIU1 */
	    P2D_BM(MSR_GLIU1_BASE1, 0x1, 0x0, 0x0, 0xfff80),
	    P2D_BM(MSR_GLIU1_BASE2, 0x1, 0x0, 0x80000, 0xfffe0),
	    P2D_SC(MSR_GLIU1_SHADOW, 0x1, 0x0, 0x0, 0xff03, 0xC0000), {
	0}
};

/** 
  * Size up ram. All we need to here is read the MSR for DRAM and grab
  * out the sizing bits.  Note that this code depends on initram
  * having run. It uses the MSRs, not the SPDs, and the MSRs of course
  * are set up by initram.
  */
int sizeram(void)
{
	msr_t msr;
	int sizem = 0;
	unsigned short dimm;

	/* Get the RAM size from the memory controller as calculated and set by auto_size_dimm() */
	msr = rdmsr(MC_CF07_DATA);
	printk(BIOS_DEBUG,"sizeram: _MSR MC_CF07_DATA: %08x:%08x\n", msr.hi, msr.lo);

	/* dimm 0 */
	dimm = msr.hi;
	/* installed? */
	if ((dimm & 7) != 7) {
		/* 1:8MB, 2:16MB, 3:32MB, 4:64MB, ... 7:512MB, 8:1GB */
		sizem = 4 << ((dimm >> 12) & 0x0F);	}

	/* dimm 1 */
	dimm = msr.hi >> 16;
	/* installed? */
	if ((dimm & 7) != 7) {
		/* 1:8MB, 2:16MB, 3:32MB, 4:64MB, ... 7:512MB, 8:1GB */
		sizem += 4 << ((dimm >> 12) & 0x0F);
	}

	printk(BIOS_DEBUG,"sizeram: sizem 0x%xMB\n", sizem);
	return sizem;
}

/** 
  * enable_shadow. Currently not set up. 
  * @param dev The nortbridge device. 
  */
static void enable_shadow(struct device * dev)
{
}

/**
  * init the northbridge pci device. Right now this a no op. We leave
  * it here as a hook for later use.  
  * @param dev The nortbridge
  * device.
   */
static void geodelx_northbridge_init(struct device * dev)
{
	//msr_t msr;

	printk(BIOS_SPEW,">> Entering northbridge.c: %s\n", __FUNCTION__);

	enable_shadow(dev);
	/*
	 * Swiss cheese
	 */
	//msr = rdmsr(MSR_GLIU0_SHADOW);

	//msr.hi |= 0x3;
	//msr.lo |= 0x30000;

	//printk(BIOS_DEBUG,"MSR 0x%08X is now 0x%08X:0x%08X\n", MSR_GLIU0_SHADOW, msr.hi, msr.lo);
	//printk(BIOS_DEBUG,"MSR 0x%08X is now 0x%08X:0x%08X\n", MSR_GLIU1_SHADOW, msr.hi, msr.lo);
}

/** 
  * Set resources for the PCI northbridge device. This function is
  * required due to VSA interactions.
  * @param dev The nortbridge device. 
  */
void geodelx_northbridge_set_resources(struct device *dev)
{
	struct resource *resource, *last;
	unsigned link;
	u8 line;

	last = &dev->resource[dev->resources];

	for (resource = &dev->resource[0]; resource < last; resource++) {

		// andrei: do not change the base address, it will make the VSA virtual registers unusable
		//pci_set_resource(dev, resource);
		// FIXME: static allocation may conflict with dynamic mappings!
	}

	for (link = 0; link < dev->links; link++) {
		struct bus *bus;
		bus = &dev->link[link];
		if (bus->children) {
			printk(BIOS_DEBUG, "my_dev_set_resources: phase4_assign_resources %d\n", bus);
			phase4_assign_resources(bus);
		}
	}

	/* set a default latency timer */
	pci_write_config8(dev, PCI_LATENCY_TIMER, 0x40);

	/* set a default secondary latency timer */
	if ((dev->hdr_type & 0x7f) == PCI_HEADER_TYPE_BRIDGE) {
		pci_write_config8(dev, PCI_SEC_LATENCY_TIMER, 0x40);
	}

	/* zero the irq settings */
	line = pci_read_config8(dev, PCI_INTERRUPT_PIN);
	if (line) {
		pci_write_config8(dev, PCI_INTERRUPT_LINE, 0);
	}

	/* set the cache line size, so far 64 bytes is good for everyone */
	pci_write_config8(dev, PCI_CACHE_LINE_SIZE, 64 >> 2);
}



/** 
  * Set resources for the PCI domain. Just set up basic global ranges
  * for IO and memory Allocation of sub-resources draws on these
  * top-level resources in the usual hierarchical manner.
  * @param dev The nortbridge device. 
  */
static void geodelx_pci_domain_read_resources(struct device * dev)
{
	struct resource *resource;
	printk(BIOS_SPEW,">> Entering northbridge.c: %s\n", __FUNCTION__);

	/* Initialize the system wide io space constraints */
	resource = new_resource(dev, IOINDEX_SUBTRACTIVE(0, 0));
	resource->limit = 0xffffUL;
	resource->flags =
	    IORESOURCE_IO | IORESOURCE_SUBTRACTIVE | IORESOURCE_ASSIGNED;

	/* Initialize the system wide memory resources constraints */
	resource = new_resource(dev, IOINDEX_SUBTRACTIVE(1, 0));
	resource->limit = 0xffffffffULL;
	resource->flags =
	    IORESOURCE_MEM | IORESOURCE_SUBTRACTIVE | IORESOURCE_ASSIGNED;
}

/** 
  * Create a ram resource, by taking the passed-in size and createing
  * a resource record.
  * @param dev the device
  * @param index a resource index
  * @param basek base memory address in k
  * @param sizek size of memory in k
  */
static void ram_resource(struct device * dev, unsigned long index,
			 unsigned long basek, unsigned long sizek)
{
	struct resource *resource;

	if (!sizek)
		return;

	resource = new_resource(dev, index);
	resource->base = ((resource_t) basek) << 10;
	resource->size = ((resource_t) sizek) << 10;
	resource->flags = IORESOURCE_MEM | IORESOURCE_CACHEABLE |
	    IORESOURCE_FIXED | IORESOURCE_STORED | IORESOURCE_ASSIGNED;
}

/** 
  * Set resources in the pci domain. Also, as a side effect, create a
  * ram resource in the child which, interestingly enough, is the
  * north bridge pci device, for later allocation of address space.
  * @param dev the device
  */
 static void geodelx_pci_domain_set_resources(struct device * dev)
{
	int idx;
	struct device * mc_dev;

	printk(BIOS_SPEW,">> Entering northbridge.c: %s\n", __FUNCTION__);

	mc_dev = dev->link[0].children;
	if (mc_dev) {
		/* Report the memory regions */
		idx = 10;
		ram_resource(dev, idx++, 0, 640);
		/* Systop - 1 MB -> KB*/
		ram_resource(dev, idx++, 1024, (get_systop() - 0x100000) / 1024);
	}

	phase4_assign_resources(&dev->link[0]);
}

/**
  * enable the pci domain. A littly tricky on this chipset due to the
  * VSA interactions.  This must happen before any PCI scans happen.
  * we do early northbridge init to make sure pci scans will work, but
  * the weird part is we actually have to run some code in x86 mode to
  * get the VSM installed, since the VSM actually handles some PCI bus
  * scan tasks via the System Management Interrupt.  Yes, it gets
  * tricky ...
  * @param dev the device
  */
static void geodelx_pci_domain_phase2(struct device * dev)
{

	printk(BIOS_SPEW,">> Entering northbridge.c: %s\n", __FUNCTION__);

	northbridge_init_early();
#warning cpu bug has been moved to initram stage
//	cpu_bug();
	chipsetinit();

	setup_realmode_idt();

	printk(BIOS_DEBUG,"Before VSA:\n");
	// print_conf();
#warning Not doing vsm bios -- linux will fail. 
//	do_vsmbios();		// do the magic stuff here, so prepare your tambourine ;)

	printk(BIOS_DEBUG,"After VSA:\n");
	// print_conf();

#warning graphics_init is disabled. 
//	graphics_init();
	pci_set_method(dev);
}

/**
  * Support for scan bus from the "tippy top" -- i.e. the pci domain,
  * not the 0:0.0 device.
  * @param dev The pci domain device
  * @param max max number of devices to scan. 
  */
static unsigned int geodelx_pci_domain_scan_bus(struct device * dev, unsigned int max)
{
	printk(BIOS_SPEW,">> Entering northbridge.c: %s\n", __FUNCTION__);

	max = pci_scan_bus(&dev->link[0], PCI_DEVFN(0, 0), 0xff, max);
	return max;
}


/**
  * Support for apic cluster init. TODO should we do this in phase 2?
  * It is now done in phase 6
  * @param dev The pci domain device
  */
static void cpu_bus_init(struct device * dev)
{
	printk(BIOS_SPEW,">> Entering northbridge.c: %s\n", __FUNCTION__);
	printk(BIOS_SPEW,">> Exiting northbridge.c: %s\n", __FUNCTION__);

}

static void cpu_bus_noop(struct device * dev)
{
}

/* the same hardware, being multifunction, has several roles. In this
  * case, the north is a pci domain controller, apic cluster, and the
  * traditional 0:0.0 device
  */

/* Here are the operations for when the northbridge is running a PCI
 *    domain.
 */
struct device_operations geodelx_pcidomainops = {
	.constructor		 = default_device_constructor,
	.phase2_setup_scan_bus	= geodelx_pci_domain_phase2,
	.phase3_scan		 = geodelx_pci_domain_scan_bus,
	.phase4_read_resources	 = geodelx_pci_domain_read_resources,
	.phase4_set_resources	 = geodelx_pci_domain_set_resources,
	.phase5_enable_resources = enable_childrens_resources,
	.phase6_init		 = 0,
	.ops_pci_bus		 = &pci_cf8_conf1,

};

/* Here are the operations for when the northbridge is running an APIC
 *   cluster.
 */
struct device_operations geodelx_apicops = {
	.constructor		 = default_device_constructor,
	.phase3_scan		 = 0,
	.phase4_read_resources	 = cpu_bus_noop,
	.phase4_set_resources	 = cpu_bus_noop,
	.phase5_enable_resources = cpu_bus_noop,
	.phase6_init		 = cpu_bus_init,
	.ops_pci_bus		 = &pci_cf8_conf1,
};

/* Here are the operations for when the northbridge is running a PCI
 * device.
 */
struct device_operations geodelx_pci_ops = {
	.constructor		 = default_device_constructor,
	.phase3_scan		 = geodelx_pci_domain_scan_bus,
	.phase4_read_resources	 = geodelx_pci_domain_read_resources,
	.phase4_set_resources	 = geodelx_northbridge_set_resources,
	.phase5_enable_resources = enable_childrens_resources,
	.phase6_init		 = geodelx_northbridge_init,
	.ops_pci_bus		 = &pci_cf8_conf1,

};


/* The constructor for the device. */
/* Domain ops and apic cluster ops and pci device ops are different */
struct constructor geodelx_north_constructors[] = {
  {.id = {.type = DEVICE_ID_PCI_DOMAIN,
	  .u = {.pci_domain = {.vendor = 0x8086,.device = 0x7190}}},
   &geodelx_pcidomainops},
  {.id = {.type = DEVICE_ID_APIC_CLUSTER,
	  .u = {.apic_cluster = {.vendor = 0x8086,.device = 0x7190}}},
   &geodelx_apicops},
  {.id = {.type = DEVICE_ID_PCI,
	  .u = {.pci = {.vendor = 0x8086,.device = 0x7190}}},
   &geodelx_pci_ops},
  {.ops = 0},
};
