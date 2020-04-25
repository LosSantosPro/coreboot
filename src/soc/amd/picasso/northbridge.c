/* SPDX-License-Identifier: GPL-2.0-only */
/* This file is part of the coreboot project. */

#include <amdblocks/biosram.h>
#include <device/pci_ops.h>
#include <arch/ioapic.h>
#include <arch/acpi.h>
#include <arch/acpigen.h>
#include <cbmem.h>
#include <console/console.h>
#include <cpu/amd/mtrr.h>
#include <cpu/x86/lapic_def.h>
#include <cpu/x86/msr.h>
#include <cpu/amd/msr.h>
#include <device/device.h>
#include <device/pci.h>
#include <device/pci_ids.h>
#include <romstage_handoff.h>
#include <soc/cpu.h>
#include <soc/northbridge.h>
#include <soc/pci_devs.h>
#include <soc/iomap.h>
#include <stdint.h>
#include <string.h>
#include <arch/bert_storage.h>

#include "chip.h"

static void set_io_addr_reg(struct device *dev, u32 nodeid, u32 linkn, u32 reg,
			u32 io_min, u32 io_max)
{
	u32 tempreg;

	/* io range allocation.  Limit */
	tempreg = (nodeid & 0xf) | ((nodeid & 0x30) << (8 - 4)) | (linkn << 4)
						| ((io_max & 0xf0) << (12 - 4));
	pci_write_config32(SOC_ADDR_DEV, reg + 4, tempreg);
	tempreg = 3 | ((io_min & 0xf0) << (12 - 4)); /* base: ISA and VGA ? */
	pci_write_config32(SOC_ADDR_DEV, reg, tempreg);
}

static void set_mmio_addr_reg(u32 nodeid, u32 linkn, u32 reg, u32 index,
						u32 mmio_min, u32 mmio_max)
{
	u32 tempreg;

	/* io range allocation.  Limit */
	tempreg = (nodeid & 0xf) | (linkn << 4) | (mmio_max & 0xffffff00);
		pci_write_config32(SOC_ADDR_DEV, reg + 4, tempreg);
	tempreg = 3 | (nodeid & 0x30) | (mmio_min & 0xffffff00);
		pci_write_config32(SOC_ADDR_DEV, reg, tempreg);
}

static void read_resources(struct device *dev)
{
	/*
	 * This MMCONF resource must be reserved in the PCI domain.
	 * It is not honored by the coreboot resource allocator if it is in
	 * the CPU_CLUSTER.
	 */
	mmconf_resource(dev, MMIO_CONF_BASE);
}

static void set_resource(struct device *dev, struct resource *res, u32 nodeid)
{
	resource_t rbase, rend;
	unsigned int reg, link_num;
	char buf[50];

	/* Make certain the resource has actually been set */
	if (!(res->flags & IORESOURCE_ASSIGNED))
		return;

	/* If I have already stored this resource don't worry about it */
	if (res->flags & IORESOURCE_STORED)
		return;

	/* Only handle PCI memory and IO resources */
	if (!(res->flags & (IORESOURCE_MEM | IORESOURCE_IO)))
		return;

	/* Ensure I am actually looking at a resource of function 1 */
	if ((res->index & 0xffff) < 0x1000)
		return;

	/* Get the base address */
	rbase = res->base;

	/* Get the limit (rounded up) */
	rend  = resource_end(res);

	/* Get the register and link */
	reg  = res->index & 0xfff; /* 4k */
	link_num = IOINDEX_LINK(res->index);

	if (res->flags & IORESOURCE_IO)
		set_io_addr_reg(dev, nodeid, link_num, reg, rbase>>8, rend>>8);
	else if (res->flags & IORESOURCE_MEM)
		set_mmio_addr_reg(nodeid, link_num, reg,
				(res->index >> 24), rbase >> 8, rend >> 8);

	res->flags |= IORESOURCE_STORED;
	snprintf(buf, sizeof(buf), " <node %x link %x>",
			nodeid, link_num);
	report_resource_stored(dev, res, buf);
}

/**
 * I tried to reuse the resource allocation code in set_resource()
 * but it is too difficult to deal with the resource allocation magic.
 */

static void create_vga_resource(struct device *dev)
{
	struct bus *link;

	/* find out which link the VGA card is connected,
	 * we only deal with the 'first' vga card */
	for (link = dev->link_list ; link ; link = link->next)
		if (link->bridge_ctrl & PCI_BRIDGE_CTL_VGA)
			break;

	/* no VGA card installed */
	if (link == NULL)
		return;

	printk(BIOS_DEBUG, "VGA: %s has VGA device\n",	dev_path(dev));
	/* Route A0000-BFFFF, IO 3B0-3BB 3C0-3DF */
	pci_write_config32(SOC_ADDR_DEV, D18F1_VGAEN, VGA_ADDR_ENABLE);
}

static void set_resources(struct device *dev)
{
	struct bus *bus;
	struct resource *res;


	/* do we need this? */
	create_vga_resource(dev);

	/* Set each resource we have found */
	for (res = dev->resource_list ; res ; res = res->next)
		set_resource(dev, res, 0);

	for (bus = dev->link_list ; bus ; bus = bus->next)
		if (bus->children)
			assign_resources(bus);
}

unsigned long acpi_fill_mcfg(unsigned long current)
{

	current += acpi_create_mcfg_mmconfig((acpi_mcfg_mmconfig_t *)current,
					     CONFIG_MMCONF_BASE_ADDRESS,
					     0,
					     0,
					     CONFIG_MMCONF_BUS_NUMBER);

	return current;
}

static void northbridge_fill_ssdt_generator(const struct device *device)
{
	msr_t msr;
	char pscope[] = "\\_SB.PCI0";

	acpigen_write_scope(pscope);
	msr = rdmsr(TOP_MEM);
	acpigen_write_name_dword("TOM1", msr.lo);
	msr = rdmsr(TOP_MEM2);
	/*
	 * Since XP only implements parts of ACPI 2.0, we can't use a qword
	 * here.
	 * See http://www.acpi.info/presentations/S01USMOBS169_OS%2520new.ppt
	 * slide 22ff.
	 * Shift value right by 20 bit to make it fit into 32bit,
	 * giving us 1MB granularity and a limit of almost 4Exabyte of memory.
	 */
	acpigen_write_name_dword("TOM2", (msr.hi << 12) | msr.lo >> 20);
	acpigen_pop_len();
}

static unsigned long agesa_write_acpi_tables(const struct device *device,
					     unsigned long current,
					     acpi_rsdp_t *rsdp)
{
	/* TODO - different mechanism to collect this info for Family 17h */
	return current;
}

static struct device_operations northbridge_operations = {
	.read_resources	  = read_resources,
	.set_resources	  = set_resources,
	.enable_resources = pci_dev_enable_resources,
	.acpi_fill_ssdt   = northbridge_fill_ssdt_generator,
	.write_acpi_tables = agesa_write_acpi_tables,
};

static const struct pci_driver family15_northbridge __pci_driver = {
	.ops	= &northbridge_operations,
	.vendor = PCI_VENDOR_ID_AMD,
	.device = PCI_DEVICE_ID_AMD_15H_MODEL_707F_NB_HT,
};

/*
 * Enable VGA cycles.  Set memory ranges of the FCH legacy devices (TPM, HPET,
 * BIOS RAM, Watchdog Timer, IOAPIC and ACPI) as non-posted.  Set remaining
 * MMIO to posted.  Route all I/O to the southbridge.
 */
void amd_initcpuio(void)
{
	uintptr_t topmem = bsp_topmem();
	uintptr_t base, limit;

	/* Enable legacy video routing: D18F1xF4 VGA Enable */
	pci_write_config32(SOC_ADDR_DEV, D18F1_VGAEN, VGA_ADDR_ENABLE);

	/* Non-posted: range(HPET-LAPIC) or 0xfed00000 through 0xfee00000-1 */
	base = (HPET_BASE_ADDRESS >> 8) | MMIO_WE | MMIO_RE;
	limit = (ALIGN_DOWN(LOCAL_APIC_ADDR - 1, 64 * KiB) >> 8) | MMIO_NP;
	pci_write_config32(SOC_ADDR_DEV, NB_MMIO_LIMIT_LO(0), limit);
	pci_write_config32(SOC_ADDR_DEV, NB_MMIO_BASE_LO(0), base);

	/* Remaining PCI hole posted MMIO: TOM-HPET (TOM through 0xfed00000-1 */
	base = (topmem >> 8) | MMIO_WE | MMIO_RE;
	limit = ALIGN_DOWN(HPET_BASE_ADDRESS - 1, 64 * KiB) >> 8;
	pci_write_config32(SOC_ADDR_DEV, NB_MMIO_LIMIT_LO(1), limit);
	pci_write_config32(SOC_ADDR_DEV, NB_MMIO_BASE_LO(1), base);

	/* Route all I/O downstream */
	base = 0 | IO_WE | IO_RE;
	limit = ALIGN_DOWN(0xffff, 4 * KiB);
	pci_write_config32(SOC_ADDR_DEV, NB_IO_LIMIT(0), limit);
	pci_write_config32(SOC_ADDR_DEV, NB_IO_BASE(0), base);
}

void fam15_finalize(void *chip_info)
{
	u32 value;

	/* disable No Snoop */
	value = pci_read_config32(SOC_HDA0_DEV, HDA_DEV_CTRL_STATUS);
	value &= ~HDA_NO_SNOOP_EN;
	pci_write_config32(SOC_HDA0_DEV, HDA_DEV_CTRL_STATUS, value);
}

void domain_set_resources(struct device *dev)
{
	uint64_t uma_base = get_uma_base();
	uint32_t uma_size = get_uma_size();
	uint32_t mem_useable = (uintptr_t)cbmem_top();
	msr_t tom = rdmsr(TOP_MEM);
	msr_t high_tom = rdmsr(TOP_MEM2);
	uint64_t high_mem_useable;
	int idx = 0x10;

	/* 0x0 -> 0x9ffff */
	ram_resource(dev, idx++, 0, 0xa0000 / KiB);

	/* 0xa0000 -> 0xbffff: legacy VGA */
	mmio_resource(dev, idx++, 0xa0000 / KiB, 0x20000 / KiB);

	/* 0xc0000 -> 0xfffff: Option ROM */
	reserved_ram_resource(dev, idx++, 0xc0000 / KiB, 0x40000 / KiB);

	/*
	 * 0x100000 (1MiB) -> low top useable RAM
	 * cbmem_top() accounts for low UMA and TSEG if they are used.
	 */
	ram_resource(dev, idx++, (1 * MiB) / KiB,
			(mem_useable - (1 * MiB)) / KiB);

	/* Low top useable RAM -> Low top RAM (bottom pci mmio hole) */
	reserved_ram_resource(dev, idx++, mem_useable / KiB,
					(tom.lo - mem_useable) / KiB);

	/* If there is memory above 4GiB */
	if (high_tom.hi) {
		/* 4GiB -> high top useable */
		if (uma_base >= (4ull * GiB))
			high_mem_useable = uma_base;
		else
			high_mem_useable = ((uint64_t)high_tom.lo |
						((uint64_t)high_tom.hi << 32));

		ram_resource(dev, idx++, (4ull * GiB) / KiB,
				((high_mem_useable - (4ull * GiB)) / KiB));

		/* High top useable RAM -> high top RAM */
		if (uma_base >= (4ull * GiB)) {
			reserved_ram_resource(dev, idx++, uma_base / KiB,
						uma_size / KiB);
		}
	}

	assign_resources(dev->link_list);
}
