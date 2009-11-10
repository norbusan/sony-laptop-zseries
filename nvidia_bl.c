/*
 * Backlight driver for Nvidia graphics adapters.
 *
 * Copyright (c) 2008-2009 Mario Schwalbe <schwalbe@inf.tu-dresden.de>
 * Based on the mechanism dicovered by the author of NvClock:
 * Copyright (c) 2001-2009 Roderick Colenbrander
 *     Site: http://nvclock.sourceforge.net
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * Maybe the check against the subsystem vendor should be removed,
 * but there's no guarantee that the chip's smartdimmer signals
 * are actually connected to the display logic. Right now, these
 * are the supported (read connected) vendors according to NvClock.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/backlight.h>
#include <linux/dmi.h>
#include <linux/pci.h>
#include <linux/err.h>

/* Check the subsystem vendor ID to ignore unsupported devices */
#define CONFIG_NVIDIA_BL_CHECK_SUBSYSTEM_VENDOR

/* Check DMI system information to ignore wrong devices */
//#define CONFIG_NVIDIA_BL_CHECK_DMI

/* Check for the new backlight suspend/resume feature */
#if defined(BL_CORE_SUSPENDRESUME)
	#define USE_BACKLIGHT_SUSPEND
/* Otherwise use a platform driver if PM is enabled */
#elif defined(CONFIG_PM)
	#define USE_PLATFORM_DRIVER
#endif

/* Register constants */
#define NV5X_PDISPLAY_OFFSET				0x00610000
#define NV5X_PDISPLAY_SOR0_BRIGHTNESS			0x0000c084
#define NV5X_PDIPSLAY_SOR0_BRIGHTNESS_CONTROL_ENABLED	0x80000000

/* Driver private data structure */
struct driver_data {
	/* PCI region (BAR) the smartdimmer register is in */
	unsigned bar;
	/* Register offset into this region */
	unsigned long reg_offset;
	/* Register size in byte */
	unsigned reg_size;

	/* Number of brightness levels supported */
	unsigned levels;
	/* Backlight operations structure */
	struct backlight_ops backlight_ops;

	/* The device we drive */
	struct pci_dev *dev;
	/* Pointer to the mapped smartdimmer register */
	/* volatile */ void __iomem *smartdimmer;
};

/* Module parameters */
static int debug;
module_param_named(debug, debug, int, 0644);
MODULE_PARM_DESC(debug, "Set to one to enable debugging messages.");

/* To be used with buggy tools that do not support more than 256 levels */
static int shift;
module_param_named(shift, shift, int, 0644);
MODULE_PARM_DESC(shift, "Shift the value by n bits to reduce the range.");

/*
 * Implementation for NV4X chips
 * (NV40, NV41, NV43, NV44, NV46, NV47, NV49, NV4B, C51)
 */
static int nv4x_get_intensity(struct backlight_device *bd)
{
	const struct driver_data *dd = bl_get_data(bd);
	unsigned short intensity = ioread16(dd->smartdimmer) & 0x1f;

	intensity >>= shift;

	if (debug)
		printk(KERN_DEBUG "nvidia_bl: read brightness of %d\n",
		       intensity);

	return intensity;
}

static int nv4x_set_intensity(struct backlight_device *bd)
{
	const struct driver_data *dd = bl_get_data(bd);
	unsigned intensity = bd->props.brightness;

	if (debug)
		printk(KERN_DEBUG "nvidia_bl: setting brightness to %d\n",
		       intensity);

	intensity <<= shift;

	iowrite16((ioread16(dd->smartdimmer) & ~0x1f) | intensity,
	          dd->smartdimmer);
	return 0;
}

static const struct driver_data nv4x_driver_data = {
	.bar           = 0,
	.reg_offset    = 0x15f2,
	.reg_size      = 2,
	.levels        = 32,
	.backlight_ops = {
#ifdef USE_BACKLIGHT_SUSPEND
		.options        = BL_CORE_SUSPENDRESUME,
#endif
		.get_brightness = nv4x_get_intensity,
		.update_status  = nv4x_set_intensity,
	}
};

/*
 * Implementation for NV5X chips
 * (NV50, G84, G86, G92, G94, G96, GT200)
 */
static int nv5x_get_intensity(struct backlight_device *bd)
{
	const struct driver_data *dd = bl_get_data(bd);
	unsigned intensity = ioread32(dd->smartdimmer) &
		~NV5X_PDIPSLAY_SOR0_BRIGHTNESS_CONTROL_ENABLED;

	intensity >>= shift;

	if (debug)
		printk(KERN_DEBUG "nvidia_bl: read brightness of %d\n",
		       intensity);

	return intensity;
}

static int nv5x_set_intensity(struct backlight_device *bd)
{
	const struct driver_data *dd = bl_get_data(bd);
	unsigned intensity = bd->props.brightness;

	if (debug)
		printk(KERN_DEBUG "nvidia_bl: setting brightness to %d\n",
		       intensity);

	intensity <<= shift;

	iowrite32(intensity | NV5X_PDIPSLAY_SOR0_BRIGHTNESS_CONTROL_ENABLED,
		  dd->smartdimmer);
	return 0;
}

static const struct driver_data nv5x_driver_data = {
	.bar           = 0,
	.reg_offset    = NV5X_PDISPLAY_OFFSET + NV5X_PDISPLAY_SOR0_BRIGHTNESS,
	.reg_size      = 4,
	.levels        = 1024,
	.backlight_ops = {
#ifdef USE_BACKLIGHT_SUSPEND
		.options        = BL_CORE_SUSPENDRESUME,
#endif
		.get_brightness = nv5x_get_intensity,
		.update_status  = nv5x_set_intensity,
	}
};

/*
 * Device matching.
 * The list of supported devices was primarily taken from NvClock,
 * but only contains the mobile chips.
 */
static DEFINE_PCI_DEVICE_TABLE(nvidia_bl_device_table) = {
	/* nVidia Geforce Go 7800GTX */
	{ PCI_VDEVICE(NVIDIA, 0x0099), (kernel_ulong_t)&nv4x_driver_data },
	/* nVidia Geforce Go 6800 */
	{ PCI_VDEVICE(NVIDIA, 0x00c8), (kernel_ulong_t)&nv4x_driver_data },
	/* nVidia Geforce Go 6800Ultra */
	{ PCI_VDEVICE(NVIDIA, 0x00c9), (kernel_ulong_t)&nv4x_driver_data },
	/* nVidia QuadroFX Go 1400 */
	{ PCI_VDEVICE(NVIDIA, 0x00cc), (kernel_ulong_t)&nv4x_driver_data },
	/* nVidia Geforce Go 6600 */
	{ PCI_VDEVICE(NVIDIA, 0x0144), (kernel_ulong_t)&nv4x_driver_data },
	/* nVidia GeForce Go 6600TE/6200TE */
	{ PCI_VDEVICE(NVIDIA, 0x0146), (kernel_ulong_t)&nv4x_driver_data },
	/* nVidia Geforce Go 6600 */
	{ PCI_VDEVICE(NVIDIA, 0x0148), (kernel_ulong_t)&nv4x_driver_data },
	/* nVidia Geforce Go 6600GT */
	{ PCI_VDEVICE(NVIDIA, 0x0149), (kernel_ulong_t)&nv4x_driver_data },
	/* nVidia Geforce Go 6200 */
	{ PCI_VDEVICE(NVIDIA, 0x0164), (kernel_ulong_t)&nv4x_driver_data },
	/* nVidia Geforce Go 6400 */
	{ PCI_VDEVICE(NVIDIA, 0x0166), (kernel_ulong_t)&nv4x_driver_data },
	/* nVidia Geforce Go 6200 */
	{ PCI_VDEVICE(NVIDIA, 0x0167), (kernel_ulong_t)&nv4x_driver_data },
	/* nVidia Geforce Go 6400 */
	{ PCI_VDEVICE(NVIDIA, 0x0168), (kernel_ulong_t)&nv4x_driver_data },
	/* nVidia Geforce Go 7300 */
	{ PCI_VDEVICE(NVIDIA, 0x01d7), (kernel_ulong_t)&nv4x_driver_data },
	/* nVidia Geforce Go 7400 */
	{ PCI_VDEVICE(NVIDIA, 0x01d8), (kernel_ulong_t)&nv4x_driver_data },
	/* nVidia Geforce Go 7400GS */
	{ PCI_VDEVICE(NVIDIA, 0x01d9), (kernel_ulong_t)&nv4x_driver_data },
	/* nVidia Quadro NVS 110M */
	{ PCI_VDEVICE(NVIDIA, 0x01da), (kernel_ulong_t)&nv4x_driver_data },
	/* nVidia Quadro NVS 120M */
	{ PCI_VDEVICE(NVIDIA, 0x01db), (kernel_ulong_t)&nv4x_driver_data },
	/* nVidia QuadroFX 350M */
	{ PCI_VDEVICE(NVIDIA, 0x01dc), (kernel_ulong_t)&nv4x_driver_data },
	/* nVidia Geforce 7500LE */
	{ PCI_VDEVICE(NVIDIA, 0x01dd), (kernel_ulong_t)&nv4x_driver_data },
	/* NV44M */
	{ PCI_VDEVICE(NVIDIA, 0x0228), (kernel_ulong_t)&nv4x_driver_data },
	/* nVidia Geforce Go 7950GTX */
	{ PCI_VDEVICE(NVIDIA, 0x0297), (kernel_ulong_t)&nv4x_driver_data },
	/* nVidia Geforce Go 7900GS */
	{ PCI_VDEVICE(NVIDIA, 0x0298), (kernel_ulong_t)&nv4x_driver_data },
	/* nVidia Geforce Go 7900GTX */
	{ PCI_VDEVICE(NVIDIA, 0x0299), (kernel_ulong_t)&nv4x_driver_data },
	/* nVidia QuadroFX 2500M */
	{ PCI_VDEVICE(NVIDIA, 0x029a), (kernel_ulong_t)&nv4x_driver_data },
	/* nVidia QuadroFX 1500M */
	{ PCI_VDEVICE(NVIDIA, 0x029b), (kernel_ulong_t)&nv4x_driver_data },
	/* nVidia Geforce Go 7700 */
	{ PCI_VDEVICE(NVIDIA, 0x0397), (kernel_ulong_t)&nv4x_driver_data },
	/* nVidia Geforce Go 7600 */
	{ PCI_VDEVICE(NVIDIA, 0x0398), (kernel_ulong_t)&nv4x_driver_data },
	/* nVidia Geforce Go 7600GT */
	{ PCI_VDEVICE(NVIDIA, 0x0399), (kernel_ulong_t)&nv4x_driver_data },
	/* nVidia Quadro NVS 300M */
	{ PCI_VDEVICE(NVIDIA, 0x039a), (kernel_ulong_t)&nv4x_driver_data },
	/* nVidia Geforce Go 7900SE */
	{ PCI_VDEVICE(NVIDIA, 0x039b), (kernel_ulong_t)&nv4x_driver_data },
	/* nVidia QuadroFX 550M */
	{ PCI_VDEVICE(NVIDIA, 0x039c), (kernel_ulong_t)&nv4x_driver_data },
	/* nVidia Geforce 9500M GS */
	{ PCI_VDEVICE(NVIDIA, 0x0405), (kernel_ulong_t)&nv5x_driver_data },
	/* nVidia Geforce NB9P-GE */
	{ PCI_VDEVICE(NVIDIA, 0x0406), (kernel_ulong_t)&nv5x_driver_data },
	/* nVidia Geforce 8600M GT */
	{ PCI_VDEVICE(NVIDIA, 0x0407), (kernel_ulong_t)&nv5x_driver_data },
	/* nVidia Geforce 8600M GTS */
	{ PCI_VDEVICE(NVIDIA, 0x0408), (kernel_ulong_t)&nv5x_driver_data },
	/* nVidia Geforce 8700M GT */
	{ PCI_VDEVICE(NVIDIA, 0x0409), (kernel_ulong_t)&nv5x_driver_data },
	/* nVidia Quadro NVS 370M */
	{ PCI_VDEVICE(NVIDIA, 0x040a), (kernel_ulong_t)&nv5x_driver_data },
	/* nVidia Quadro NVS 320M */
	{ PCI_VDEVICE(NVIDIA, 0x040b), (kernel_ulong_t)&nv5x_driver_data },
	/* nVidia QuadroFX 570M */
	{ PCI_VDEVICE(NVIDIA, 0x040c), (kernel_ulong_t)&nv5x_driver_data },
	/* nVidia QuadroFX 1600M */
	{ PCI_VDEVICE(NVIDIA, 0x040d), (kernel_ulong_t)&nv5x_driver_data },
	/* nVidia Geforce 8600M GS */
	{ PCI_VDEVICE(NVIDIA, 0x0425), (kernel_ulong_t)&nv5x_driver_data },
	/* nVidia Geforce 8400M GT */
	{ PCI_VDEVICE(NVIDIA, 0x0426), (kernel_ulong_t)&nv5x_driver_data },
	/* nVidia Geforce 8400M GS */
	{ PCI_VDEVICE(NVIDIA, 0x0427), (kernel_ulong_t)&nv5x_driver_data },
	/* nVidia Geforce 8400M G */
	{ PCI_VDEVICE(NVIDIA, 0x0428), (kernel_ulong_t)&nv5x_driver_data },
	/* nVidia Quadro NVS 140M */
	{ PCI_VDEVICE(NVIDIA, 0x0429), (kernel_ulong_t)&nv5x_driver_data },
	/* nVidia Quadro NVS 130M */
	{ PCI_VDEVICE(NVIDIA, 0x042a), (kernel_ulong_t)&nv5x_driver_data },
	/* nVidia Quadro NVS 135M */
	{ PCI_VDEVICE(NVIDIA, 0x042b), (kernel_ulong_t)&nv5x_driver_data },
	/* nVidia Quadro FX 360M */
	{ PCI_VDEVICE(NVIDIA, 0x042d), (kernel_ulong_t)&nv5x_driver_data },
	/* nVidia Geforce 9300M G */
	{ PCI_VDEVICE(NVIDIA, 0x042e), (kernel_ulong_t)&nv5x_driver_data },
	/* nVidia Geforce 8800M GTS */
	{ PCI_VDEVICE(NVIDIA, 0x0609), (kernel_ulong_t)&nv5x_driver_data },
	/* nVidia Geforce 8800M GTX */
	{ PCI_VDEVICE(NVIDIA, 0x060c), (kernel_ulong_t)&nv5x_driver_data },
	/* nVidia QuadroFX 3600M */
	{ PCI_VDEVICE(NVIDIA, 0x061c), (kernel_ulong_t)&nv5x_driver_data },
	/* nVidia Geforce 9600M GT */
	{ PCI_VDEVICE(NVIDIA, 0x0647), (kernel_ulong_t)&nv5x_driver_data },
	/* nVidia Geforce 9600M GS */
	{ PCI_VDEVICE(NVIDIA, 0x0648), (kernel_ulong_t)&nv5x_driver_data },
	/* nVidia Geforce 9600M GT */
	{ PCI_VDEVICE(NVIDIA, 0x0649), (kernel_ulong_t)&nv5x_driver_data },
	/* nVidia Geforce 9500M G */
	{ PCI_VDEVICE(NVIDIA, 0x064b), (kernel_ulong_t)&nv5x_driver_data },
	/* nVidia Geforce 9300M GS */
	{ PCI_VDEVICE(NVIDIA, 0x06e5), (kernel_ulong_t)&nv5x_driver_data },
	/* nVidia Geforce 9200M GS */
	{ PCI_VDEVICE(NVIDIA, 0x06e8), (kernel_ulong_t)&nv5x_driver_data },
	/* nVidia Geforce 9300M GS */
	{ PCI_VDEVICE(NVIDIA, 0x06e9), (kernel_ulong_t)&nv5x_driver_data },
	/* nVidia Quadro NVS 150M */
	{ PCI_VDEVICE(NVIDIA, 0x06ea), (kernel_ulong_t)&nv5x_driver_data },
	/* nVidia Quadro NVS 160M */
	{ PCI_VDEVICE(NVIDIA, 0x06eb), (kernel_ulong_t)&nv5x_driver_data },
	/* nVidia Geforce G105M */
	{ PCI_VDEVICE(NVIDIA, 0x06ec), (kernel_ulong_t)&nv5x_driver_data },
	/* nVidia QuadroFX 370M */
	{ PCI_VDEVICE(NVIDIA, 0x06fb), (kernel_ulong_t)&nv5x_driver_data },
	/* nVidia Geforce 9400M */
	{ PCI_VDEVICE(NVIDIA, 0x0863), (kernel_ulong_t)&nv5x_driver_data },
	/* NVIDIA GeForce 9400M */
	{ PCI_VDEVICE(NVIDIA, 0x0870), (kernel_ulong_t)&nv5x_driver_data },
	/* NVIDIA GeForce 9200 */
	{ PCI_VDEVICE(NVIDIA, 0x0871), (kernel_ulong_t)&nv5x_driver_data },
	/* NVIDIA GeForce G102M */
	{ PCI_VDEVICE(NVIDIA, 0x0872), (kernel_ulong_t)&nv5x_driver_data },
	/* NVIDIA GeForce G102M */
	{ PCI_VDEVICE(NVIDIA, 0x0873), (kernel_ulong_t)&nv5x_driver_data },
	/* NVIDIA Quadro FX 470 */
	{ PCI_VDEVICE(NVIDIA, 0x087a), (kernel_ulong_t)&nv5x_driver_data },
	/* end of list */
	{ }
};

#ifdef CONFIG_NVIDIA_BL_CHECK_SUBSYSTEM_VENDOR

/* According to NvClock, supported subsystem vendors.
 * Defined separately to not unnecessarily enlarge the previous array. */
static const unsigned nvidia_bl_subvendors[] __devinitconst = {
	PCI_VENDOR_ID_APPLE,
	PCI_VENDOR_ID_HP,
	PCI_VENDOR_ID_SAMSUNG,
	PCI_VENDOR_ID_SONY,
	0x1a46, /* PCI_VENDOR_ID_ZEPTO not defined */
};

#endif

#ifdef CONFIG_NVIDIA_BL_CHECK_DMI

/*
 * DMI matching.
 * Used to ignore the wrong device on machines incorporating 2
 * graphics adapters, such as the Apple MacBook Pro 5.
 */
static const struct dmi_system_id *nvidia_bl_dmi_system_id;

static int nvidia_bl_dmi_match(const struct dmi_system_id *id)
{
	printk(KERN_INFO "nvidia_bl: %s detected\n", id->ident);
	nvidia_bl_dmi_system_id = id;
	return 1;
}

static const struct dmi_system_id /* __initdata */ nvidia_bl_ignore_table[] = {
	{
		.callback	= &nvidia_bl_dmi_match,
		.ident		= "MacBookPro 5,1",
		.matches	= {
			DMI_MATCH(DMI_SYS_VENDOR, "Apple Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "MacBookPro5,1"),
		},
		.driver_data	= (void *)0x0863, /* nVidia Geforce 9400M */
	},
	{
		.callback	= &nvidia_bl_dmi_match,
		.ident		= "MacBookPro 5,2",
		.matches	= {
			DMI_MATCH(DMI_SYS_VENDOR, "Apple Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "MacBookPro5,2"),
		},
		.driver_data	= (void *)0x0863, /* nVidia Geforce 9400M */
	},
	{ }
};

#endif

/*
 * Driver data implementation
 */
static const struct pci_device_id *nvidia_bl_match_id(struct pci_dev *dev)
{
	/* Search id in table */
	const struct pci_device_id *id = pci_match_id(nvidia_bl_device_table, dev);

#ifdef CONFIG_NVIDIA_BL_CHECK_SUBSYSTEM_VENDOR
	int i;
	if (id)
		/* ... and check subsystem vendor */
		for (i = 0; i < ARRAY_SIZE(nvidia_bl_subvendors); i++)
			if (dev->subsystem_vendor == nvidia_bl_subvendors[i])
				return id;
	return NULL;
#else
	return id;
#endif
}

static int nvidia_bl_find_device(struct driver_data *dd, unsigned ignore_device)
{
	struct pci_dev *dev = NULL;
	const struct pci_device_id *id;

	/* For each PCI device */
	while ((dev = pci_get_device(PCI_ANY_ID, PCI_ANY_ID, dev))) {
		/* ... lookup id struct */
		id = nvidia_bl_match_id(dev);
		if (id && (!ignore_device || (dev->device != ignore_device))) {
			printk(KERN_INFO "nvidia_bl: Supported Nvidia graphics"
			       " adapter %04x:%04x:%04x:%04x detected\n",
			       id->vendor, id->device,
			       dev->subsystem_vendor, dev->subsystem_device);

			/* Setup driver data */
			*dd = *((struct driver_data *)id->driver_data);
			dd->dev = dev;
			return 0;
		}
	}

	printk(KERN_INFO "nvidia_bl: No supported Nvidia graphics adapter"
	       " found\n");
	return -ENODEV;
}

static int nvidia_bl_map_smartdimmer(struct driver_data *dd)
{
	/* Get resource properties */
	const unsigned long bar_start = pci_resource_start(dd->dev, dd->bar),
			    bar_end   = pci_resource_end(dd->dev, dd->bar),
			    bar_flags = pci_resource_flags(dd->dev, dd->bar);
	/* Calculate register address */
	const unsigned long reg_addr  = bar_start + dd->reg_offset;

	/* Sanity check 1: Should be a memory region containing registers */
	if (!(bar_flags & IORESOURCE_MEM))
		return -ENODEV;
	if (bar_flags & IORESOURCE_PREFETCH)
		return -ENODEV;

	/* Sanity check 2: Address should not exceed the PCI BAR */
	if (reg_addr + dd->reg_size - 1 > bar_end)
		return -ENODEV;

	if (debug)
		printk(KERN_DEBUG "nvidia_bl: using BAR #%d at 0x%lx, "
		       "smartdimmer at 0x%lx\n", dd->bar, bar_start, reg_addr);

	/* Now really map (The address need not be page-aligned.) */
	dd->smartdimmer = ioremap_nocache(reg_addr, dd->reg_size);
	if (!dd->smartdimmer)
		return -ENXIO;

	return 0;
}

static void nvidia_bl_unmap_smartdimmer(struct driver_data *dd)
{
	iounmap(dd->smartdimmer);
	dd->smartdimmer = NULL;
}

/*
 * Driver implementation
 */
static struct driver_data driver_data;
static struct backlight_device *nvidia_bl_device;

#ifdef USE_PLATFORM_DRIVER
static int nvidia_bl_probe(struct platform_device *pdev)
#else
static int __init nvidia_bl_init(void)
#endif
{
	unsigned ignore_device = 0;
	int err;

	/* Bail-out if PCI subsystem is not initialized */
	if (no_pci_devices())
		return -ENODEV;

#ifdef CONFIG_NVIDIA_BL_CHECK_DMI
	/* Check DMI whether we need to ignore some device */
	dmi_check_system(nvidia_bl_ignore_table);
	if (nvidia_bl_dmi_system_id)
		ignore_device =
			(unsigned long)nvidia_bl_dmi_system_id->driver_data;
#endif

	/* Look for a supported PCI device */
	err = nvidia_bl_find_device(&driver_data, ignore_device);
	if (err)
		return err;

	/* Map smartdimmer */
	err = nvidia_bl_map_smartdimmer(&driver_data);
	if (err)
		return err;

	/* Register at backlight framework */
	nvidia_bl_device = backlight_device_register("nvidia_backlight", NULL,
	                                             &driver_data,
	                                             &driver_data.backlight_ops);
	if (IS_ERR(nvidia_bl_device)) {
		nvidia_bl_unmap_smartdimmer(&driver_data);
		return PTR_ERR(nvidia_bl_device);
	}

	/* Set up backlight device */
	nvidia_bl_device->props.max_brightness =
		(driver_data.levels >> shift) - 1;
	nvidia_bl_device->props.brightness =
		nvidia_bl_device->ops->get_brightness(nvidia_bl_device);
	backlight_update_status(nvidia_bl_device);
	return 0;
}

#ifdef USE_PLATFORM_DRIVER
static int nvidia_bl_remove(struct platform_device *pdev)
#else
static void __exit nvidia_bl_exit(void)
#endif
{
	/* Unregister at backlight framework */
	if (nvidia_bl_device)
		backlight_device_unregister(nvidia_bl_device);

	/* Unmap smartdimmer */
	if (driver_data.smartdimmer)
		nvidia_bl_unmap_smartdimmer(&driver_data);

	/* Release PCI device */
	if (driver_data.dev)
		pci_dev_put(driver_data.dev);
#ifdef USE_PLATFORM_DRIVER
	return 0;
#endif
}

/*
 * Platform driver implementation
 */
#ifdef USE_PLATFORM_DRIVER

static int nvidia_bl_resume(struct platform_device *pdev)
{
	if (debug)
		printk(KERN_DEBUG "nvidia_bl: resuming with"
		       " brightness %d\n", nvidia_bl_device->props.brightness);

	backlight_update_status(nvidia_bl_device);
	return 0;
}

static struct platform_driver nvidia_bl_driver = {
	.probe          = nvidia_bl_probe,
	.remove         = nvidia_bl_remove,
	.resume         = nvidia_bl_resume,
	.driver         = {
		.owner  = THIS_MODULE,
		.name   = "nvidia_bl"
	},
};

static struct platform_device *nvidia_bl_platform_device;

static int __init nvidia_bl_init(void)
{
	int err;

	err = platform_driver_register(&nvidia_bl_driver);
	if (err)
		return err;

	nvidia_bl_platform_device =
		platform_device_register_simple("nvidia_bl", -1, NULL, 0);
	if (!nvidia_bl_platform_device) {
		platform_driver_unregister(&nvidia_bl_driver);
		return -ENOMEM;
	}

	return 0;
}

static void __exit nvidia_bl_exit(void)
{
	platform_device_unregister(nvidia_bl_platform_device);
	platform_driver_unregister(&nvidia_bl_driver);
}

#endif /* USE_PLATFORM_DRIVER */

module_init(nvidia_bl_init);
module_exit(nvidia_bl_exit);

MODULE_AUTHOR("Mario Schwalbe <schwalbe@inf.tu-dresden.de>");
MODULE_DESCRIPTION("Nvidia-based graphics adapter backlight driver");
MODULE_LICENSE("GPL");

