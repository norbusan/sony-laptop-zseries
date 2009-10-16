/*
 * test.c
 *
 *  Created on: Feb 22, 2009
 *      Author: matze
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/acpi.h>

#define SONY_VAIO 1

#ifdef SONY_VAIO
static const char *vars[] = { 
	"\\SP2O", "\\SP1O", "\\IO1B", "\\IO1L", "\\IO2B",
	"\\IO2L", "\\IO3B", "\\IO3L", "\\MCHB", "\\MCHL", "\\EGPB",
	"\\EGPL", "\\DMIB", "\\DMIL", "\\IFPB", "\\IFPL", "\\PEBS",
	"\\PELN", "\\TTTB", "\\TTTL", "\\SMBS", "\\PBLK", "\\PMBS",
	"\\PMLN", "\\LVL2", "\\LVL3", "\\LVL4", "\\SMIP", "\\GPBS",
	"\\GPLN", "\\APCB", "\\APCL", "\\PM30", "\\SRCB", "\\SRCL",
	"\\SUSW", "\\ACPH", "\\ASSB", "\\AOTB", "\\AAXB", "\\PEHP",
	"\\SHPC", "\\PEPM", "\\PEER", "\\PECS", "\\ITKE", "\\TRTP",
	"\\TRTD", "\\TRTI", "\\TRTA", "\\GCDD", "\\DSTA", "\\DSLO",
	"\\DSLC", "\\PITS", "\\SBCS", "\\SALS", "\\LSSS", "\\PSSS",
	"\\SOOT", "\\ESCS", "\\PDBR", "\\SMBL", "\\STRP", "\\ECOK",
	"\\SSPS",
	"\\HPLG", "\\HPEJ", "\\HPLE", "\\HGAP", "\\HNCD", "\\HNCA",
	"\\HPND", "\\POVR", "\\HDAE", "\\HDHE", "\\ADAD", "\\SNGT",
	"\\HGDD", "\\GPIO.GP27", "\\GPIO.GP28", 
	"\\_SB.PCI0.LPC.H8EC.HPWR",
	"\\_SB.PCI0.LPC.H8EC.HLMX",
	"\\_SB.PCI0.LPC.H8EC.HCMM",
	"\\_SB.PCI0.LPC.H8EC.DLED",
	"\\_SB.PCI0.LPC.H8EC.ILED",
	"\\_SB.PCI0.LPC.H8EC.SWPS",
	"\\_SB.PCI0.LPC.H8EC.HPOK",
	"\\_SB.PCI0.LPC.H8EC.HHPD",
	"\\_SB.PCI0.LPC.H8EC.DHPD",
	NULL, 
};
#else 
static const char *vars[] = { 
	"\\SP1O", "\\IOCE", "\\IOCL", "\\IO1B", "\\IO1L",
	"\\IOEC", "\\IO4L", "\\IO5L", "\\IO2B", "\\IO2L", "\\IOPM",
	"\\IO3B", "\\IO3L", "\\SI1P", "\\MCHB", "\\MCHL", "\\EGPB",
	"\\EGPL", "\\DMIB", "\\DMIL", "\\PEBS", "\\PELN", "\\LAPB",
	"\\SMBS", "\\SMBL", 
	NULL,
};
#endif

static int dump_dsdt_variable(const char *path)
{
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	int result;
	
	result = acpi_evaluate_object(NULL, (char*)path, NULL, &output);
	if (result) {
		printk("%s failed: %d\n", path, result);
		return -1;
	}

	obj = (union acpi_object*)output.pointer;
	printk("%s type %d ", path, obj->type);	
	if (obj->type == ACPI_TYPE_PACKAGE) {
		int i;
		printk("returned package sized %d\n", obj->package.count);
		for (i = 0; i < obj->package.count; i++)
			printk("%d %08x\n", i, (int)obj->package.elements[i].integer.value);
	} else
	if (obj->type == ACPI_TYPE_INTEGER) {
		printk("int %08X\n", (int)obj->integer.value);
	} else
	if (obj->type == ACPI_TYPE_BUFFER) {
		int i;
		printk("returned buffer sized %d\n", obj->buffer.length);
		for (i = 0; i < obj->buffer.length; i++)
			printk("%02x", obj->buffer.pointer[i]);
		printk("\n");
	}

	kfree(output.pointer);
	return 0;
};

static void dump_acpi_vars(void)
{
	int i = 0;

	while (vars[i]) {
		dump_dsdt_variable(vars[i]);
		i++;
	}
}

static int __init test_init(void)
{
	dump_acpi_vars();
	return 0;
}

static void __exit test_exit(void)
{
}

module_init(test_init);
module_exit(test_exit);
