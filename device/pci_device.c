/*
 * This file is part of the coreboot project.
 *
 * It was originally based on the Linux kernel (drivers/pci/pci.c).
 *
 * Modifications are:
 * Copyright (C) 2003-2004 Linux Networx
 * (Written by Eric Biederman <ebiederman@lnxi.com> for Linux Networx)
 * Copyright (C) 2003-2006 Ronald G. Minnich <rminnich@gmail.com>
 * Copyright (C) 2004-2005 Li-Ta Lo <ollie@lanl.gov>
 * Copyright (C) 2005-2006 Tyan
 * (Written by Yinghai Lu <yhlu@tyan.com> for Tyan)
 * Copyright (C) 2005-2007 Stefan Reinauer <stepan@openbios.org>
 */

/*
 * PCI Bus Services, see include/linux/pci.h for further explanation.
 *
 * Copyright 1993 -- 1997 Drew Eckhardt, Frederic Potter,
 * David Mosberger-Tang
 *
 * Copyright 1997 -- 1999 Martin Mares <mj@atrey.karlin.mff.cuni.cz>
 */

#include <types.h>
#include <io.h>
#include <string.h>
#include <lib.h>
#include <console.h>
#include <device/device.h>
#include <device/pci.h>
#include <device/pci_ids.h>
#define CONFIG_HYPERTRANSPORT_PLUGIN_SUPPORT 0
#define CONFIG_PCIX_PLUGIN_SUPPORT 0
#define CONFIG_PCIE_PLUGIN_SUPPORT 0
#define CONFIG_CARDBUS_PLUGIN_SUPPORT 0
#define CONFIG_AGP_PLUGIN_SUPPORT 0
#if CONFIG_HYPERTRANSPORT_PLUGIN_SUPPORT == 1
#include <device/hypertransport.h>
#endif
#if CONFIG_PCIX_PLUGIN_SUPPORT == 1
#include <device/pcix.h>
#endif
#if CONFIG_PCIE_PLUGIN_SUPPORT == 1
#include <device/pcie.h>
#endif
#if CONFIG_AGP_PLUGIN_SUPPORT == 1
#include <device/agp.h>
#endif
#if CONFIG_CARDBUS_PLUGIN_SUPPORT == 1
#include <device/cardbus.h>
#endif
#include <statictree.h>

u8 pci_moving_config8(struct device *dev, unsigned int reg)
{
	u8 value, ones, zeroes;
	value = pci_read_config8(dev, reg);

	pci_write_config8(dev, reg, 0xff);
	ones = pci_read_config8(dev, reg);

	pci_write_config8(dev, reg, 0x00);
	zeroes = pci_read_config8(dev, reg);

	pci_write_config8(dev, reg, value);

	return ones ^ zeroes;
}

u16 pci_moving_config16(struct device *dev, unsigned int reg)
{
	u16 value, ones, zeroes;
	value = pci_read_config16(dev, reg);

	pci_write_config16(dev, reg, 0xffff);
	ones = pci_read_config16(dev, reg);

	pci_write_config16(dev, reg, 0x0000);
	zeroes = pci_read_config16(dev, reg);

	pci_write_config16(dev, reg, value);

	return ones ^ zeroes;
}

u32 pci_moving_config32(struct device *dev, unsigned int reg)
{
	u32 value, ones, zeroes;
	value = pci_read_config32(dev, reg);

	pci_write_config32(dev, reg, 0xffffffff);
	ones = pci_read_config32(dev, reg);

	pci_write_config32(dev, reg, 0x00000000);
	zeroes = pci_read_config32(dev, reg);

	pci_write_config32(dev, reg, value);

	return ones ^ zeroes;
}

unsigned int pci_find_next_capability(struct device *dev, unsigned int cap,
				      unsigned int last)
{
	unsigned int pos;
	unsigned int status;
	unsigned int reps = 48;
	pos = 0;
	status = pci_read_config16(dev, PCI_STATUS);
	if (!(status & PCI_STATUS_CAP_LIST)) {
		return 0;
	}
	switch (dev->hdr_type & 0x7f) {
	case PCI_HEADER_TYPE_NORMAL:
	case PCI_HEADER_TYPE_BRIDGE:
		pos = PCI_CAPABILITY_LIST;
		break;
	case PCI_HEADER_TYPE_CARDBUS:
		pos = PCI_CB_CAPABILITY_LIST;
		break;
	default:
		return 0;
	}
	pos = pci_read_config8(dev, pos);
	while (reps-- && (pos >= 0x40)) { /* Loop through the linked list. */
		int this_cap;
		pos &= ~3;
		this_cap = pci_read_config8(dev, pos + PCI_CAP_LIST_ID);
		printk(BIOS_SPEW, "Capability: 0x%02x @ 0x%02x\n", cap, pos);
		if (this_cap == 0xff) {
			break;
		}
		if (!last && (this_cap == cap)) {
			return pos;
		}
		if (last == pos) {
			last = 0;
		}
		pos = pci_read_config8(dev, pos + PCI_CAP_LIST_NEXT);
	}
	return 0;
}

unsigned int pci_find_capability(struct device *dev, unsigned int cap)
{
	return pci_find_next_capability(dev, cap, 0);
}

/**
 * Given a device and register, read the size of the BAR for that register. 
 *
 * @param dev Pointer to the device structure.
 * @param index Address of the PCI configuration register.
 */
struct resource *pci_get_resource(struct device *dev, unsigned long index)
{
	struct resource *resource;
	unsigned long value, attr;
	resource_t moving, limit;

	/* Initialize the resources to nothing. */
	resource = new_resource(dev, index);

	/* Get the initial value. */
	value = pci_read_config32(dev, index);

	/* See which bits move. */
	moving = pci_moving_config32(dev, index);

	/* Initialize attr to the bits that do not move. */
	attr = value & ~moving;

	/* If it is a 64bit resource look at the high half as well. */
	if (((attr & PCI_BASE_ADDRESS_SPACE_IO) == 0) &&
	    ((attr & PCI_BASE_ADDRESS_MEM_LIMIT_MASK) ==
	     PCI_BASE_ADDRESS_MEM_LIMIT_64)) {
		/* Find the high bits that move. */
		moving |=
		    ((resource_t) pci_moving_config32(dev, index + 4)) << 32;
	}

	/* Find the resource constraints.
	 * Start by finding the bits that move. From there:
	 * - Size is the least significant bit of the bits that move.
	 * - Limit is all of the bits that move plus all of the lower bits.
	 * See PCI Spec 6.2.5.1.
	 */
	limit = 0;
	if (moving) {
		resource->size = 1;
		resource->align = resource->gran = 0;
		while (!(moving & resource->size)) {
			resource->size <<= 1;
			resource->align += 1;
			resource->gran += 1;
		}
		resource->limit = limit = moving | (resource->size - 1);
	}

	/* Some broken hardware has read-only registers that do not 
	 * really size correctly.
	 * Example: the Acer M7229 has BARs 1-4 normally read-only. 
	 * so BAR1 at offset 0x10 reads 0x1f1. If you size that register
	 * by writing 0xffffffff to it, it will read back as 0x1f1 -- a 
	 * violation of the spec. 
	 * We catch this case and ignore it by observing which bits move,
	 * This also catches the common case unimplemented registers
	 * that always read back as 0.
	 */
	if (moving == 0) {
		if (value != 0) {
			printk(BIOS_DEBUG,
			       "%s register %02lx(%08lx), read-only ignoring it\n",
			       dev_path(dev), index, value);
		}
		resource->flags = 0;
	} else if (attr & PCI_BASE_ADDRESS_SPACE_IO) {
		/* An I/O mapped base address. */
		attr &= PCI_BASE_ADDRESS_IO_ATTR_MASK;
		resource->flags |= IORESOURCE_IO;
		/* I don't want to deal with 32bit I/O resources. */
		resource->limit = 0xffff;
	} else {
		/* A Memory mapped base address. */
		attr &= PCI_BASE_ADDRESS_MEM_ATTR_MASK;
		resource->flags |= IORESOURCE_MEM;
		if (attr & PCI_BASE_ADDRESS_MEM_PREFETCH) {
			resource->flags |= IORESOURCE_PREFETCH;
		}
		attr &= PCI_BASE_ADDRESS_MEM_LIMIT_MASK;
		if (attr == PCI_BASE_ADDRESS_MEM_LIMIT_32) {
			/* 32bit limit. */
			resource->limit = 0xffffffffUL;
		} else if (attr == PCI_BASE_ADDRESS_MEM_LIMIT_1M) {
			/* 1MB limit. */
			resource->limit = 0x000fffffUL;
		} else if (attr == PCI_BASE_ADDRESS_MEM_LIMIT_64) {
			/* 64bit limit. */
			resource->limit = 0xffffffffffffffffULL;
			resource->flags |= IORESOURCE_PCI64;
		} else {
			/* Invalid value. */
			resource->flags = 0;
		}
	}
	/* Don't let the limit exceed which bits can move. */
	if (resource->limit > limit) {
		resource->limit = limit;
	}

	return resource;
}

static void pci_get_rom_resource(struct device *dev, unsigned long index)
{
	struct resource *resource;
	unsigned long value;
	resource_t moving, limit;

	if ((dev->on_mainboard) && (dev->rom_address == 0)) {
		// Skip it if rom_address is not set in MB Config.lb.
		// TODO: No more Config.lb in coreboot-v3.
		return;
	}

	/* Initialize the resources to nothing. */
	resource = new_resource(dev, index);

	/* Get the initial value. */
	value = pci_read_config32(dev, index);

	/* See which bits move. */
	moving = pci_moving_config32(dev, index);
	/* Clear the Enable bit. */
	moving = moving & ~PCI_ROM_ADDRESS_ENABLE;

	/* Find the resource constraints. 
	 * Start by finding the bits that move. From there:
	 * - Size is the least significant bit of the bits that move.
	 * - Limit is all of the bits that move plus all of the lower bits.
	 * See PCI Spec 6.2.5.1.
	 */
	limit = 0;

	if (moving) {
		resource->size = 1;
		resource->align = resource->gran = 0;
		while (!(moving & resource->size)) {
			resource->size <<= 1;
			resource->align += 1;
			resource->gran += 1;
		}
		resource->limit = limit = moving | (resource->size - 1);
	}

	if (moving == 0) {
		if (value != 0) {
			printk(BIOS_DEBUG,
			       "%s register %02lx(%08lx), read-only ignoring it\n",
			       dev_path(dev), index, value);
		}
		resource->flags = 0;
	} else {
		resource->flags |= IORESOURCE_MEM | IORESOURCE_READONLY;
	}

	/* For on board device with embedded ROM image, the ROM image is at
	 * fixed address specified in the Config.lb, the dev->rom_address is
	 * inited by driver_pci_onboard_ops::enable_dev() */
	/* TODO: No more Config.lb in coreboot-v3. */
	if ((dev->on_mainboard) && (dev->rom_address != 0)) {
		resource->base = dev->rom_address;
		resource->flags |= IORESOURCE_MEM | IORESOURCE_READONLY |
		    IORESOURCE_ASSIGNED | IORESOURCE_FIXED;
	}

	compact_resources(dev);
}

/**
 * Read the base address registers for a given device.
 *
 * @param dev Pointer to the dev structure.
 * @param howmany How many registers to read (6 for device, 2 for bridge).
 */
static void pci_read_bases(struct device *dev, unsigned int howmany)
{
	unsigned long index;

	for (index = PCI_BASE_ADDRESS_0;
	     (index < PCI_BASE_ADDRESS_0 + (howmany << 2));) {
		struct resource *resource;
		resource = pci_get_resource(dev, index);
		index += (resource->flags & IORESOURCE_PCI64) ? 8 : 4;
	}

	compact_resources(dev);
}

static void pci_set_resource(struct device *dev, struct resource *resource);

static void pci_record_bridge_resource(struct device *dev, resource_t moving,
				       unsigned int index, unsigned long mask,
				       unsigned long type)
{
	/* Initialize the constraints on the current bus. */
	struct resource *resource;
	resource = 0;
	if (moving) {
		unsigned long gran;
		resource_t step;
		resource = new_resource(dev, index);
		resource->size = 0;
		gran = 0;
		step = 1;
		while ((moving & step) == 0) {
			gran += 1;
			step <<= 1;
		}
		resource->gran = gran;
		resource->align = gran;
		resource->limit = moving | (step - 1);
		resource->flags = type | IORESOURCE_PCI_BRIDGE;
		compute_allocate_resource(&dev->link[0], resource, mask, type);
		/* If there is nothing behind the resource,
		 * clear it and forget it.
		 */
		if (resource->size == 0) {
			resource->base = moving;
			resource->flags |= IORESOURCE_ASSIGNED;
			resource->flags &= ~IORESOURCE_STORED;
			pci_set_resource(dev, resource);
			resource->flags = 0;
		}
	}
	return;
}

static void pci_bridge_read_bases(struct device *dev)
{
	resource_t moving_base, moving_limit, moving;

	/* See if the bridge I/O resources are implemented. */
	moving_base = ((u32) pci_moving_config8(dev, PCI_IO_BASE)) << 8;
	moving_base |=
	    ((u32) pci_moving_config16(dev, PCI_IO_BASE_UPPER16)) << 16;

	moving_limit = ((u32) pci_moving_config8(dev, PCI_IO_LIMIT)) << 8;
	moving_limit |=
	    ((u32) pci_moving_config16(dev, PCI_IO_LIMIT_UPPER16)) << 16;

	moving = moving_base & moving_limit;

	/* Initialize the I/O space constraints on the current bus. */
	pci_record_bridge_resource(dev, moving, PCI_IO_BASE,
				   IORESOURCE_IO, IORESOURCE_IO);

	/* See if the bridge prefmem resources are implemented. */
	moving_base =
	    ((resource_t) pci_moving_config16(dev, PCI_PREF_MEMORY_BASE)) << 16;
	moving_base |= ((resource_t) pci_moving_config32(dev, PCI_PREF_BASE_UPPER32)) << 32;

	moving_limit = ((resource_t) pci_moving_config16(dev, PCI_PREF_MEMORY_LIMIT)) << 16;
	moving_limit |= ((resource_t) pci_moving_config32(dev, PCI_PREF_LIMIT_UPPER32)) << 32;

	moving = moving_base & moving_limit;
	/* Initialize the prefetchable memory constraints on the current bus. */
	pci_record_bridge_resource(dev, moving, PCI_PREF_MEMORY_BASE,
				   IORESOURCE_MEM | IORESOURCE_PREFETCH,
				   IORESOURCE_MEM | IORESOURCE_PREFETCH);

	/* See if the bridge mem resources are implemented. */
	moving_base = ((u32) pci_moving_config16(dev, PCI_MEMORY_BASE)) << 16;
	moving_limit = ((u32) pci_moving_config16(dev, PCI_MEMORY_LIMIT)) << 16;

	moving = moving_base & moving_limit;

	/* Initialize the memory resources on the current bus. */
	pci_record_bridge_resource(dev, moving, PCI_MEMORY_BASE,
				   IORESOURCE_MEM | IORESOURCE_PREFETCH,
				   IORESOURCE_MEM);

	compact_resources(dev);
}

void pci_dev_read_resources(struct device *dev)
{
	pci_read_bases(dev, 6);
	pci_get_rom_resource(dev, PCI_ROM_ADDRESS);
}

void pci_bus_read_resources(struct device *dev)
{
	pci_bridge_read_bases(dev);
	pci_read_bases(dev, 2);
	pci_get_rom_resource(dev, PCI_ROM_ADDRESS1);
}

/**
 * Set resources for the PCI domain.
 *
 * A PCI domain contains the I/O and memory resource address space below it.
 * Set up basic global ranges for I/O and memory. Allocation of sub-resources
 * draws on these top-level resources in the usual hierarchical manner.
 *
 * @param dev The northbridge device.
 */
void pci_domain_read_resources(struct device *dev)
{
	struct resource *res;

	/* Initialize the system-wide I/O space constraints. */
	res = new_resource(dev, IOINDEX_SUBTRACTIVE(0, 0));
	res->limit = 0xffffUL;
	res->flags =
	    IORESOURCE_IO | IORESOURCE_SUBTRACTIVE | IORESOURCE_ASSIGNED;

	/* Initialize the system-wide memory resources constraints. */
	res = new_resource(dev, IOINDEX_SUBTRACTIVE(1, 0));
	res->limit = 0xffffffffULL;
	res->flags =
	    IORESOURCE_MEM | IORESOURCE_SUBTRACTIVE | IORESOURCE_ASSIGNED;
}

static void pci_set_resource(struct device *dev, struct resource *resource)
{
	resource_t base, end;

	/* Make certain the resource has actually been set. */
	if (!(resource->flags & IORESOURCE_ASSIGNED)) {
		printk(BIOS_ERR,
		       "ERROR: %s %02lx %s size: 0x%010llx not assigned\n",
		       dev_path(dev), resource->index, resource_type(resource),
		       resource->size);
		return;
	}

	/* If I have already stored this resource don't worry about it. */
	if (resource->flags & IORESOURCE_STORED) {
		return;
	}

	/* If the resource is subtractive don't worry about it. */
	if (resource->flags & IORESOURCE_SUBTRACTIVE) {
		return;
	}

	/* Only handle PCI memory and I/O resources for now. */
	if (!(resource->flags & (IORESOURCE_MEM | IORESOURCE_IO)))
		return;

	/* Enable the resources in the command register. */
	if (resource->size) {
		if (resource->flags & IORESOURCE_MEM) {
			dev->command |= PCI_COMMAND_MEMORY;
		}
		if (resource->flags & IORESOURCE_IO) {
			dev->command |= PCI_COMMAND_IO;
		}
		if (resource->flags & IORESOURCE_PCI_BRIDGE) {
			dev->command |= PCI_COMMAND_MASTER;
		}
	}
	/* Get the base address. */
	base = resource->base;

	/* Get the end. */
	end = resource_end(resource);

	/* Now store the resource. */
	resource->flags |= IORESOURCE_STORED;
	if (!(resource->flags & IORESOURCE_PCI_BRIDGE)) {
		unsigned long base_lo, base_hi;
		/* Some chipsets allow us to set/clear the I/O bit
		 * (e.g. VIA 82c686a). So set it to be safe.
		 */
		base_lo = base & 0xffffffff;
		base_hi = (base >> 32) & 0xffffffff;
		if (resource->flags & IORESOURCE_IO) {
			base_lo |= PCI_BASE_ADDRESS_SPACE_IO;
		}
		pci_write_config32(dev, resource->index, base_lo);
		if (resource->flags & IORESOURCE_PCI64) {
			pci_write_config32(dev, resource->index + 4, base_hi);
		}
	} else if (resource->index == PCI_IO_BASE) {
		/* Set the I/O ranges. */
		compute_allocate_resource(&dev->link[0], resource,
					  IORESOURCE_IO, IORESOURCE_IO);
		pci_write_config8(dev, PCI_IO_BASE, base >> 8);
		pci_write_config16(dev, PCI_IO_BASE_UPPER16, base >> 16);
		pci_write_config8(dev, PCI_IO_LIMIT, end >> 8);
		pci_write_config16(dev, PCI_IO_LIMIT_UPPER16, end >> 16);
	} else if (resource->index == PCI_MEMORY_BASE) {
		/* Set the memory range. */
		compute_allocate_resource(&dev->link[0], resource,
					  IORESOURCE_MEM | IORESOURCE_PREFETCH,
					  IORESOURCE_MEM);
		pci_write_config16(dev, PCI_MEMORY_BASE, base >> 16);
		pci_write_config16(dev, PCI_MEMORY_LIMIT, end >> 16);
	} else if (resource->index == PCI_PREF_MEMORY_BASE) {
		/* Set the prefetchable memory range. */
		compute_allocate_resource(&dev->link[0], resource,
					  IORESOURCE_MEM | IORESOURCE_PREFETCH,
					  IORESOURCE_MEM | IORESOURCE_PREFETCH);
		pci_write_config16(dev, PCI_PREF_MEMORY_BASE, base >> 16);
		pci_write_config32(dev, PCI_PREF_BASE_UPPER32, base >> 32);
		pci_write_config16(dev, PCI_PREF_MEMORY_LIMIT, end >> 16);
		pci_write_config32(dev, PCI_PREF_LIMIT_UPPER32, end >> 32);
	} else {
		/* Don't let me think I stored the resource. */
		resource->flags &= ~IORESOURCE_STORED;
		printk(BIOS_ERR, "ERROR: invalid resource->index %lx\n",
		       resource->index);
	}
	report_resource_stored(dev, resource, "");
	return;
}

void pci_dev_set_resources(struct device *dev)
{
	struct resource *resource, *last;
	unsigned int link;
	u8 line;

	last = &dev->resource[dev->resources];

	for (resource = &dev->resource[0]; resource < last; resource++) {
		pci_set_resource(dev, resource);
	}
	for (link = 0; link < dev->links; link++) {
		struct bus *bus;
		bus = &dev->link[link];
		if (bus->children) {
			phase4_assign_resources(bus);
		}
	}

	/* Set a default latency timer. */
	pci_write_config8(dev, PCI_LATENCY_TIMER, 0x40);

	/* Set a default secondary latency timer. */
	if ((dev->hdr_type & 0x7f) == PCI_HEADER_TYPE_BRIDGE) {
		pci_write_config8(dev, PCI_SEC_LATENCY_TIMER, 0x40);
	}

	/* Zero the IRQ settings. */
	line = pci_read_config8(dev, PCI_INTERRUPT_PIN);
	if (line) {
		pci_write_config8(dev, PCI_INTERRUPT_LINE, 0);
	}
	/* Set the cache line size, so far 64 bytes is good for everyone. */
	pci_write_config8(dev, PCI_CACHE_LINE_SIZE, 64 >> 2);
}

/**
 * Create a RAM resource, by taking the passed-in size and creating
 * a resource record.
 *
 * @param dev The device.
 * @param index A resource index.
 * @param basek Base memory address in KB.
 * @param sizek Size of memory in KB.
 */
void ram_resource(struct device *dev, unsigned long index,
		  unsigned long basek, unsigned long sizek)
{
	struct resource *res;

	if (!sizek)
		return;

	res = new_resource(dev, index);
	res->base = ((resource_t) basek) << 10;	/* Convert to bytes. */
	res->size = ((resource_t) sizek) << 10; /* Convert to bytes. */
	res->flags = IORESOURCE_MEM | IORESOURCE_CACHEABLE |
	    IORESOURCE_FIXED | IORESOURCE_STORED | IORESOURCE_ASSIGNED;

	printk(BIOS_SPEW, "Adding RAM resource (%lld bytes)\n", res->size);
}

void pci_dev_set_subsystem_wrapper(struct device *dev)
{
	const struct pci_operations *ops;
	u16 vendor = 0;
	u16 device = 0;

#warning Per-device subsystem ID has to be set here, but for that we have to extend the dts.

#ifdef HAVE_MAINBOARD_PCI_SUBSYSTEM_ID
	/* If there's no explicit subsystem ID for this device and the device
	 * is onboard, use the board defaults. */
	if (dev->on_mainboard) {
		if (!vendor)
			vendor = mainboard_pci_subsystem_vendor;
		if (!device)
			device = mainboard_pci_subsystem_device;
	} else {
		printk(BIOS_DEBUG, "%s: Device not on_mainboard\n",
		       dev_path(dev));
	}
#endif
	/* Set the subsystem vendor and device ID for mainboard devices. */
	ops = ops_pci(dev);

	/* If either vendor or device is zero, we leave it as is. */
	if (ops && ops->set_subsystem && vendor && device) {
		printk(BIOS_DEBUG,
		       "%s: Setting subsystem VID/DID to %02x/%02x\n",
		       dev_path(dev), vendor, device);

		ops->set_subsystem(dev,	vendor, device);
	} else {
		printk(BIOS_DEBUG, "%s: Not setting subsystem VID/DID\n",
			dev_path(dev));
	}
		
}

void pci_dev_enable_resources(struct device *dev)
{
	u16 command;

	pci_dev_set_subsystem_wrapper(dev);

	command = pci_read_config16(dev, PCI_COMMAND);
	command |= dev->command;
	command |= (PCI_COMMAND_PARITY + PCI_COMMAND_SERR); // Error check.
	printk(BIOS_DEBUG, "%s: %s (%s) cmd <- %02x\n", __func__, dev->dtsname,
	       dev_path(dev), command);
	pci_write_config16(dev, PCI_COMMAND, command);
}

void pci_bus_enable_resources(struct device *dev)
{
	u16 ctrl;

	/* Enable I/O in command register if there is VGA card
	 * connected with (even it does not claim I/O resource).
	 */
	if (dev->link[0].bridge_ctrl & PCI_BRIDGE_CTL_VGA)
		dev->command |= PCI_COMMAND_IO;
	ctrl = pci_read_config16(dev, PCI_BRIDGE_CONTROL);
	ctrl |= dev->link[0].bridge_ctrl;
	ctrl |= (PCI_BRIDGE_CTL_PARITY + PCI_BRIDGE_CTL_SERR); // Error check.
	printk(BIOS_DEBUG, "%s bridge ctrl <- %04x\n", dev_path(dev), ctrl);
	pci_write_config16(dev, PCI_BRIDGE_CONTROL, ctrl);

	pci_dev_enable_resources(dev);
	enable_childrens_resources(dev);
}

void pci_bus_reset(struct bus *bus)
{
	unsigned ctl;
	ctl = pci_read_config16(bus->dev, PCI_BRIDGE_CONTROL);
	ctl |= PCI_BRIDGE_CTL_BUS_RESET;
	pci_write_config16(bus->dev, PCI_BRIDGE_CONTROL, ctl);
	mdelay(10);
	ctl &= ~PCI_BRIDGE_CTL_BUS_RESET;
	pci_write_config16(bus->dev, PCI_BRIDGE_CONTROL, ctl);
	delay(1);
}

void pci_dev_set_subsystem(struct device *dev, unsigned int vendor,
			   unsigned int device)
{
	pci_write_config32(dev, PCI_SUBSYSTEM_VENDOR_ID,
			   ((device & 0xffff) << 16) | (vendor & 0xffff));
}

void pci_dev_init(struct device *dev)
{
	printk(BIOS_SPEW, "PCI: pci_dev_init\n");
#ifdef CONFIG_PCI_OPTION_ROM_RUN
	void run_bios(struct device *dev, unsigned long addr);
	struct rom_header *rom, *ram;

	printk(BIOS_INFO, "Probing for option ROM\n");
	rom = pci_rom_probe(dev);
	if (rom == NULL)
		return;
	ram = pci_rom_load(dev, rom);
	if (ram == NULL)
		return;
	run_bios(dev, (unsigned long)ram);
#endif
}

/** Default device operation for PCI devices. */
struct pci_operations pci_dev_ops_pci = {
	.set_subsystem = pci_dev_set_subsystem,
};

struct device_operations default_pci_ops_dev = {
	.phase4_read_resources   = pci_dev_read_resources,
	.phase4_set_resources    = pci_dev_set_resources,
	.phase5_enable_resources = pci_dev_enable_resources,
	.phase6_init             = pci_dev_init,
	.phase3_scan             = 0,
	.phase4_enable_disable   = 0,
	.ops_pci                 = &pci_dev_ops_pci,
};

/** Default device operations for PCI bridges. */
struct pci_operations pci_bus_ops_pci = {
	.set_subsystem = 0,
};

struct device_operations default_pci_ops_bus = {
	.phase4_read_resources   = pci_bus_read_resources,
	.phase4_set_resources    = pci_dev_set_resources,
	.phase5_enable_resources = pci_bus_enable_resources,
	.phase6_init             = 0,
	.phase3_scan             = pci_scan_bridge,
	.phase4_enable_disable   = 0,
	.reset_bus               = pci_bus_reset,
	.ops_pci                 = &pci_bus_ops_pci,
};

/**
 * Detect the type of downstream bridge.
 *
 * This function is a heuristic to detect which type of bus is downstream
 * of a PCI-to-PCI bridge. This functions by looking for various capability
 * blocks to figure out the type of downstream bridge. PCI-X, PCI-E, and
 * Hypertransport all seem to have appropriate capabilities.
 * 
 * When only a PCI-Express capability is found the type
 * is examined to see which type of bridge we have.
 *
 * @param dev TODO
 * @return Appropriate bridge operations.
 */
static struct device_operations *get_pci_bridge_ops(struct device *dev)
{
	// unsigned int pos;

#if CONFIG_PCIX_PLUGIN_SUPPORT == 1
	pos = pci_find_capability(dev, PCI_CAP_ID_PCIX);
	if (pos) {
		printk(BIOS_DEBUG, "%s subordinate bus PCI-X\n",
		       dev_path(dev));
		return &default_pcix_ops_bus;
	}
#endif
#if CONFIG_AGP_PLUGIN_SUPPORT == 1
	/* How do I detect an PCI to AGP bridge? */
#endif
#if CONFIG_HYPERTRANSPORT_PLUGIN_SUPPORT == 1
	pos = 0;
	while ((pos = pci_find_next_capability(dev, PCI_CAP_ID_HT, pos))) {
		unsigned int flags;
		flags = pci_read_config16(dev, pos + PCI_CAP_FLAGS);
		if ((flags >> 13) == 1) {
			/* Host or Secondary Interface. */
			printk(BIOS_DEBUG,
			       "%s subordinate bus Hypertransport\n",
			       dev_path(dev));
			return &default_ht_ops_bus;
		}
	}
#endif
#if CONFIG_PCIE_PLUGIN_SUPPORT == 1
	pos = pci_find_capability(dev, PCI_CAP_ID_PCIE);
	if (pos) {
		unsigned int flags;
		flags = pci_read_config16(dev, pos + PCI_EXP_FLAGS);
		switch ((flags & PCI_EXP_FLAGS_TYPE) >> 4) {
		case PCI_EXP_TYPE_ROOT_PORT:
		case PCI_EXP_TYPE_UPSTREAM:
		case PCI_EXP_TYPE_DOWNSTREAM:
			printk(BIOS_DEBUG, "%s subordinate bus PCI Express\n",
			       dev_path(dev));
			return &default_pcie_ops_bus;
		case PCI_EXP_TYPE_PCI_BRIDGE:
			printk(BIOS_DEBUG, "%s subordinate PCI\n",
			       dev_path(dev));
			return &default_pci_ops_bus;
		default:
			break;
		}
	}
#endif
	return &default_pci_ops_bus;
}

/**
 * Set up PCI device operation.
 *
 * @param dev TODO
 * @see pci_drivers
 */
static void set_pci_ops(struct device *dev)
{
	struct device_operations *c;
	struct device_id id;

	if (dev->ops) {
		printk(BIOS_SPEW, "%s: dev %p(%s) already has ops %p\n",
		       __func__, dev, dev->dtsname, dev->ops);
		return;
	}

	id  = dev->id;

	/* Look through the list of setup drivers and find one for
	 * this PCI device.
	 */
	c = find_device_operations(&dev->id);
	if (c) {
		dev->ops = c;
		printk(BIOS_SPEW, "%s id %s %sops\n",
			dev_path(dev), dev_id_string(&id), 
			(dev->ops->phase3_scan ? "bus " : ""));
		return;
	}

	/* If I don't have a specific driver use the default operations. */
	switch (dev->hdr_type & 0x7f) {	/* Header type. */
	case PCI_HEADER_TYPE_NORMAL:	/* Standard header. */
		if ((dev->class >> 8) == PCI_CLASS_BRIDGE_PCI)
			goto bad;
		dev->ops = &default_pci_ops_dev;
		break;
	case PCI_HEADER_TYPE_BRIDGE:
		if ((dev->class >> 8) != PCI_CLASS_BRIDGE_PCI)
			goto bad;
		dev->ops = get_pci_bridge_ops(dev);
		break;
#if CONFIG_CARDBUS_PLUGIN_SUPPORT == 1
	case PCI_HEADER_TYPE_CARDBUS:
		dev->ops = &default_cardbus_ops_bus;
		break;
#endif
	default:
	      bad:
		if (dev->enabled) {
			printk(BIOS_ERR,
			       "%s [%s/%06x] has unknown header "
			       "type %02x, ignoring.\n", dev_path(dev),
			       dev_id_string(&dev->id), dev->class >> 8,
			       dev->hdr_type);
		}
	}
	printk(BIOS_INFO, "%s: dev %p(%s) set ops to %p\n", __func__, dev,
	       dev->dtsname, dev->ops);
	return;
}

/**
 * See if we have already allocated a device structure for a given devfn.
 *
 * Given a linked list of PCI device structures and a devfn number, find the
 * device structure correspond to the devfn, if present. This function also
 * removes the device structure from the linked list.
 *
 * @param list The device structure list.
 * @param devfn A device/function number.
 * @return Pointer to the device structure found or NULL of we have not
 *	   allocated a device for this devfn yet.
 */
static struct device *pci_scan_get_dev(struct device **list, unsigned int devfn)
{
	struct device *dev;
	dev = 0;
	printk(BIOS_SPEW, "%s: list is %p, *list is %p\n", __func__, list,
	       *list);
	for (; *list; list = &(*list)->sibling) {
		printk(BIOS_SPEW, "%s: check dev %s \n", __func__,
		       (*list)->dtsname);
		if ((*list)->path.type != DEVICE_PATH_PCI) {
			printk(BIOS_NOTICE,
			       "%s: child %s(%s) not a pci device: it's type %d\n",
			       __FUNCTION__, (*list)->dtsname, dev_path(*list),
			       (*list)->path.type);
			continue;
		}
		printk(BIOS_SPEW, "%s: check dev %s it has devfn 0x%02x\n",
		       __func__, (*list)->dtsname, (*list)->path.pci.devfn);
		if ((*list)->path.pci.devfn == devfn) {
			/* Unlink from the list. */
			dev = *list;
			*list = (*list)->sibling;
			dev->sibling = 0;
			break;
		}
	}

	/* Just like alloc_dev() add the device to the list of devices on the
	 * bus. When the list of devices was formed we removed all of the
	 * parents children, and now we are interleaving static and dynamic
	 * devices in order on the bus.
	 */
	if (dev) {
		struct device *child;
		/* Find the last child of our parent. */
		for (child = dev->bus->children; child && child->sibling;) {
			child = child->sibling;
		}
		/* Place the device on the list of children of its parent. */
		if (child) {
			child->sibling = dev;
		} else {
			dev->bus->children = dev;
		}
	}

	return dev;
}

/** 
 * Scan a PCI bus.
 *
 * Determine the existence of a given PCI device. Allocate a new struct device
 * if dev==NULL was passed in and the device exists in hardware.
 *
 * @param dev Pointer to the device structure if it already is in the device
 *         tree, i.e. was specified in the dts. It may not exist on hardware,
 *         however. Looking for hardware not yet in the device tree has this
 *         parameter as NULL.
 * @param bus Pointer to the bus structure.
 * @param devfn A device/function number.
 * @return The device structure for the device if it exists in hardware
 *         or the passed in device structure with enabled=0 if the device
 *         does not exist in hardware and only in the tree
 *         or NULL if no device is found and dev==NULL was passed in.
 */
struct device *pci_probe_dev(struct device *dev, struct bus *bus,
			     unsigned int devfn)
{
	u32 id, class;
	u8 hdr_type;

	/* Detect if a device is present. */
	if (!dev) {
		struct device dummy;
		struct device_id devid;
		dummy.bus = bus;
		dummy.path.type = DEVICE_PATH_PCI;
		dummy.path.pci.devfn = devfn;
		id = pci_read_config32(&dummy, PCI_VENDOR_ID);
		/* Have we found something?
		 * Some broken boards return 0 if a slot is empty.
		 */
		if ((id == 0xffffffff) || (id == 0x00000000) ||
		    (id == 0x0000ffff) || (id == 0xffff0000)) {
			printk(BIOS_SPEW, "PCI: devfn 0x%x, bad id 0x%x\n",
			       devfn, id);
			return NULL;
		}
		devid.type = DEVICE_ID_PCI;
		devid.pci.vendor = id & 0xffff;
		devid.pci.device = id >> 16;
		dev = alloc_dev(bus, &dummy.path, &devid);
	} else {
		/* Enable/disable the device. Once we have found the device
		 * specific operations this operations we will disable the
		 * device with those as well.
		 * 
		 * This is geared toward devices that have subfunctions
		 * that do not show up by default.
		 * 
		 * If a device is a stuff option on the motherboard
		 * it may be absent and enable_dev() must cope.
		 */
		/* Run the magic enable sequence for the device. */
		if (dev->ops && dev->ops->phase3_enable_scan) {
			dev->ops->phase3_enable_scan(dev);
		}
		/* Now read the vendor and device ID. */
		id = pci_read_config32(dev, PCI_VENDOR_ID);

		/* If the device does not have a PCI ID disable it. Possibly
		 * this is because we have already disabled the device. But
		 * this also handles optional devices that may not always
		 * show up.
		 */
		/* If the chain is fully enumerated quit. */
		if ((id == 0xffffffff) || (id == 0x00000000) ||
		    (id == 0x0000ffff) || (id == 0xffff0000)) {
			if (dev->enabled) {
				printk(BIOS_INFO,
				       "Disabling static device: %s\n",
				       dev_path(dev));
				dev->enabled = 0;
			}
			return dev;
		}
	}
	/* Read the rest of the PCI configuration information. */
	hdr_type = pci_read_config8(dev, PCI_HEADER_TYPE);
	class = pci_read_config32(dev, PCI_CLASS_REVISION);
	dev->status = pci_read_config16(dev, PCI_STATUS);
	dev->revision = pci_read_config8(dev, PCI_REVISION_ID);
	dev->cache_line = pci_read_config8(dev, PCI_CACHE_LINE_SIZE);
	dev->irq_line = pci_read_config8(dev, PCI_INTERRUPT_LINE);
	dev->irq_pin = pci_read_config8(dev, PCI_INTERRUPT_PIN);
	dev->min_gnt = pci_read_config8(dev, PCI_MIN_GNT);
	dev->max_lat = pci_read_config8(dev, PCI_MAX_LAT);
#warning Per-device subsystem ID should only be read from the device if none has been specified for the device in the dts.
	dev->subsystem_vendor = pci_read_config16(dev, PCI_SUBSYSTEM_VENDOR_ID);
	dev->subsystem_device = pci_read_config16(dev, PCI_SUBSYSTEM_ID);

	/* Store the interesting information in the device structure. */
	dev->id.type = DEVICE_ID_PCI;
	dev->id.pci.vendor = id & 0xffff;
	dev->id.pci.device = (id >> 16) & 0xffff;
	dev->hdr_type = hdr_type;
	/* Class code, the upper 3 bytes of PCI_CLASS_REVISION. */
	dev->class = class >> 8;

	/* Architectural/System devices always need to be bus masters. */
	if ((dev->class >> 16) == PCI_BASE_CLASS_SYSTEM) {
		dev->command |= PCI_COMMAND_MASTER;
	}
	/* Look at the vendor and device ID, or at least the header type and
	 * class and figure out which set of configuration methods to use.
	 * Unless we already have some PCI ops.
	 */
	set_pci_ops(dev);

	/* Now run the magic enable/disable sequence for the device. */
	if (dev->ops && dev->ops->phase4_enable_disable) {
		dev->ops->phase4_enable_disable(dev);
	}

	/* Display the device and error if we don't have some PCI operations
	 * for it.
	 */
	printk(BIOS_DEBUG, "%s [%s] %s%s\n",
	       dev_path(dev), dev_id_string(&dev->id),
	       dev->enabled ? "enabled" : "disabled",
	       dev->ops ? "" : " No operations");

	return dev;
}

/** 
 * Scan a PCI bus.
 *
 * Determine the existence of devices and bridges on a PCI bus. If there are
 * bridges on the bus, recursively scan the buses behind the bridges.
 *
 * This function is the default scan_bus() method for the root device
 * 'dev_root'.
 *
 * @param bus Pointer to the bus structure.
 * @param min_devfn Minimum devfn to look at in the scan usually 0x00.
 * @param max_devfn Maximum devfn to look at in the scan usually 0xff.
 * @param max Current bus number.
 * @return The maximum bus number found, after scanning all subordinate buses.
 */
unsigned int pci_scan_bus(struct bus *bus, unsigned int min_devfn,
			  unsigned int max_devfn, unsigned int max)
{
	unsigned int devfn;
	struct device *old_devices;
	struct device *child;

	printk(BIOS_DEBUG, "%s start bus %p, bus->dev %p\n", __func__, bus,
	       bus->dev);
	if (bus->dev->path.type != DEVICE_PATH_PCI_BUS)
		printk(BIOS_ERR, "ERROR: pci_scan_bus called with incorrect "
		       "bus->dev->path.type, path is %s\n", dev_path(bus->dev));

#if PCI_BUS_SEGN_BITS
	printk(BIOS_DEBUG, "PCI: pci_scan_bus for bus %04x:%02x\n",
	       bus->secondary >> 8, bus->secondary & 0xff);
#else
	printk(BIOS_DEBUG, "PCI: pci_scan_bus for bus %02x\n", bus->secondary);
#endif

	old_devices = bus->children;
	printk(BIOS_DEBUG, "%s: old_devices %p, dev for this bus %p (%s)\n",
	       __func__, old_devices, bus->dev, bus->dev->dtsname);
	bus->children = 0;

	post_code(POST_STAGE2_PCISCANBUS_ENTER);
	printk(BIOS_SPEW, "PCI: scan devfn 0x%x to 0x%x\n", min_devfn,
	       max_devfn);
	/* Probe all devices/functions on this bus with some optimization for
	 * non-existence and single function devices.
	 */
	for (devfn = min_devfn; devfn <= max_devfn; devfn++) {
		struct device *dev;
		printk(BIOS_SPEW, "PCI: devfn 0x%x\n", devfn);

		/* First thing setup the device structure. */
		dev = pci_scan_get_dev(&old_devices, devfn);

		printk(BIOS_SPEW,
		       "PCI: pci_scan_bus pci_scan_get_dev returns dev %s\n",
		       dev ? dev->dtsname : "None (no dev in tree yet)");
		/* See if a device is present and setup the device structure. */
		dev = pci_probe_dev(dev, bus, devfn);
		printk(BIOS_SPEW,
		       "PCI: pci_scan_bus pci_probe_dev returns dev %p(%s)\n",
		       dev, dev ? dev->dtsname : "None (not found)");

		/* If this is not a multi function device, or the device is
		 * not present don't waste time probing another function. 
		 * Skip to next device.
		 */
		if ((PCI_FUNC(devfn) == 0x00) &&
		    (!dev
		     || (dev->enabled && ((dev->hdr_type & 0x80) != 0x80)))) {
			printk(BIOS_SPEW, "Not a multi function device, or the "
			       "device is not present. Skip to next device.\n");
			devfn += 0x07;
		}
	}
	printk(BIOS_SPEW, "PCI: Done for loop\n");
	post_code(POST_STAGE2_PCISCANBUS_DONEFORLOOP);

	/* Die if any leftover static devices are are found.  
	 * There's probably a problem in the Config.lb.
	 * TODO: No more Config.lb in coreboot-v3.
	 */
	if (old_devices) {
		struct device *left;
		printk(BIOS_INFO, "PCI: Left over static devices:\n");
		for (left = old_devices; left; left = left->sibling) {
			printk(BIOS_INFO, "%s\n", left->dtsname);
		}
		printk(BIOS_INFO, "PCI: End of leftover list.\n");
	}

	/* For all children that implement scan_bus() (i.e. bridges)
	 * scan the bus behind that child.
	 */
	for (child = bus->children; child; child = child->sibling) {
		max = dev_phase3_scan(child, max);
	}

	/* We've scanned the bus and so we know all about what's on the other
	 * side of any bridges that may be on this bus plus any devices.
	 * Return how far we've got finding sub-buses.
	 */
	printk(BIOS_DEBUG, "PCI: pci_scan_bus returning with max=%03x\n", max);
	post_code(POST_STAGE2_PCISCANBUS_EXIT);
	return max;
}

/**
 * Support for scan bus from the "tippy top" -- i.e. the PCI domain,
 * not the 0:0.0 device.
 *
 * This function works for almost all chipsets (AMD K8 is the exception).
 *
 * @param dev The PCI domain device.
 * @param max Maximum number of devices to scan.
 * @return TODO
 */
unsigned int pci_domain_scan_bus(struct device *dev, unsigned int max)
{
	printk(BIOS_SPEW, "pci_domain_scan_bus: calling pci_scan_bus\n");
	/* There is only one link on this device, and it is always link 0. */
	return pci_scan_bus(&dev->link[0], PCI_DEVFN(0, 0), 0xff, max);
}

/**
 * Scan a PCI bridge and the buses behind the bridge.
 *
 * Determine the existence of buses behind the bridge. Set up the bridge
 * according to the result of the scan.
 *
 * This function is the default scan_bus() method for PCI bridge devices.
 *
 * @param dev Pointer to the bridge device.
 * @param max The highest bus number assigned up to now.
 * @return The maximum bus number found, after scanning all subordinate buses.
 */
unsigned int do_pci_scan_bridge(struct device *dev, unsigned int max,
				unsigned int (*do_scan_bus) (struct bus * bus,
						unsigned int min_devfn,
						unsigned int max_devfn,
						unsigned int max))
{
	struct bus *bus;
	u32 buses;
	u16 cr;

	printk(BIOS_SPEW, "%s for %s\n", __func__, dev_path(dev));

	bus = &dev->link[0];
	bus->dev = dev;
	dev->links = 1;

	/* Set up the primary, secondary and subordinate bus numbers. We have
	 * no idea how many buses are behind this bridge yet, so we set the
	 * subordinate bus number to 0xff for the moment. 
	 */
	bus->secondary = ++max;
	bus->subordinate = 0xff;

	/* Clear all status bits and turn off memory, I/O and master enables. */
	cr = pci_read_config16(dev, PCI_COMMAND);
	pci_write_config16(dev, PCI_COMMAND, 0x0000);
	pci_write_config16(dev, PCI_STATUS, 0xffff);

	/* Read the existing primary/secondary/subordinate bus
	 * number configuration.
	 */
	buses = pci_read_config32(dev, PCI_PRIMARY_BUS);

	/* Configure the bus numbers for this bridge: the configuration
	 * transactions will not be propagated by the bridge if it is not
	 * correctly configured.
	 */
	buses &= 0xff000000;
	buses |= (((unsigned int)(dev->bus->secondary) << 0) |
		  ((unsigned int)(bus->secondary) << 8) |
		  ((unsigned int)(bus->subordinate) << 16));
	pci_write_config32(dev, PCI_PRIMARY_BUS, buses);

	/* Now we can scan all subordinate buses 
	 * i.e. the bus behind the bridge.
	 */
	max = do_scan_bus(bus, 0x00, 0xff, max);

	/* We know the number of buses behind this bridge. Set the subordinate
	 * bus number to its real value.
	 */
	bus->subordinate = max;
	buses = (buses & 0xff00ffff) | ((unsigned int)(bus->subordinate) << 16);
	pci_write_config32(dev, PCI_PRIMARY_BUS, buses);
	pci_write_config16(dev, PCI_COMMAND, cr);

	printk(BIOS_DEBUG, "%s DONE\n", __func__);
	printk(BIOS_SPEW, "%s returns max %d\n", __func__, max);
	return max;
}

/**
 * Scan a PCI bridge and the buses behind the bridge.
 *
 * Determine the existence of buses behind the bridge. Set up the bridge
 * according to the result of the scan.
 *
 * This function is the default scan_bus() method for PCI bridge devices.
 *
 * @param dev Pointer to the bridge device.
 * @param max The highest bus number assigned up to now.
 * @return The maximum bus number found, after scanning all subordinate buses.
 */
unsigned int pci_scan_bridge(struct device *dev, unsigned int max)
{
	printk(BIOS_SPEW, "pci_scan_bridge: calling pci_scan_bus\n");
	return do_pci_scan_bridge(dev, max, pci_scan_bus);
}

/**
 * Tell the EISA int controller this int must be level triggered.
 *
 * THIS IS A KLUDGE -- sorry, this needs to get cleaned up.
 *
 * @param intNum TODO
 */
void pci_level_irq(unsigned char intNum)
{
	unsigned short intBits = inb(0x4d0) | (((unsigned)inb(0x4d1)) << 8);

	printk(BIOS_SPEW, "%s: current ints are 0x%x\n", __func__, intBits);
	intBits |= (1 << intNum);

	printk(BIOS_SPEW, "%s: try to set ints 0x%x\n", __func__, intBits);

	/* Write new values. */
	outb((unsigned char)intBits, 0x4d0);
	outb((unsigned char)(intBits >> 8), 0x4d1);

	/* This seems like an error but is not. */
	if (inb(0x4d0) != (intBits & 0xff)) {
		printk(BIOS_ERR,
		       "%s: lower order bits are wrong: want 0x%x, got 0x%x\n",
		       __func__, intBits & 0xff, inb(0x4d0));
	}
	if (inb(0x4d1) != ((intBits >> 8) & 0xff)) {
		printk(BIOS_ERR,
		       "%s: lower order bits are wrong: want 0x%x, got 0x%x\n",
		       __func__, (intBits >> 8) & 0xff, inb(0x4d1));
	}
}

/**
 * This function assigns IRQs for all functions contained within the
 * indicated device address. If the device does not exist or does not
 * require interrupts then this function has no effect.
 *
 * This function should be called for each PCI slot in your system.
 *
 * pIntAtoD is an array of IRQ #s that are assigned to PINTA through PINTD of
 * this slot.
 *
 * The particular irq #s that are passed in depend on the routing inside
 * your southbridge and on your motherboard.
 *
 * -kevinh@ispiri.com
 *
 * @param bus TODO
 * @param slot TODO
 * @param pIntAtoD TODO
 */
void pci_assign_irqs(unsigned int bus, unsigned int slot,
		     const unsigned char pIntAtoD[4])
{
	unsigned int functNum;
	struct device *pdev;
	unsigned char line;
	unsigned char irq;
	unsigned char readback;

	/* Each slot may contain up to eight functions. */
	for (functNum = 0; functNum < 8; functNum++) {
		pdev = dev_find_slot(bus, (slot << 3) + functNum);

		if (pdev) {
			line = pci_read_config8(pdev, PCI_INTERRUPT_PIN);

			// PCI spec says all other values are reserved.
			if ((line >= 1) && (line <= 4)) {
				irq = pIntAtoD[line - 1];

				printk(BIOS_DEBUG,
				       "Assigning IRQ %d to %d:%x.%d\n", irq,
				       bus, slot, functNum);

				pci_write_config8(pdev, PCI_INTERRUPT_LINE,
						  pIntAtoD[line - 1]);

				readback =
				    pci_read_config8(pdev, PCI_INTERRUPT_LINE);
				printk(BIOS_DEBUG, "  Readback = %d\n",
				       readback);

				// Change to level triggered.
				pci_level_irq(pIntAtoD[line - 1]);
			}
		}
	}
}
