/*
 * ACPI Sony Notebook Control Driver (SNC and SPIC)
 *
 * Copyright (C) 2004-2005 Stelian Pop <stelian@popies.net>
 * Copyright (C) 2007-2009 Mattia Dongili <malattia@linux.it>
 *
 * Parts of this driver inspired from asus_acpi.c and ibm_acpi.c
 * which are copyrighted by their respective authors.
 *
 * The SNY6001 driver part is based on the sonypi driver which includes
 * material from:
 *
 * Copyright (C) 2001-2005 Stelian Pop <stelian@popies.net>
 *
 * Copyright (C) 2005 Narayanan R S <nars@kadamba.org>
 *
 * Copyright (C) 2001-2002 Alcôve <www.alcove.com>
 *
 * Copyright (C) 2001 Michael Ashley <m.ashley@unsw.edu.au>
 *
 * Copyright (C) 2001 Junichi Morita <jun1m@mars.dti.ne.jp>
 *
 * Copyright (C) 2000 Takaya Kinjo <t-kinjo@tc4.so-net.ne.jp>
 *
 * Copyright (C) 2000 Andrew Tridgell <tridge@valinux.com>
 *
 * Earlier work by Werner Almesberger, Paul `Rusty' Russell and Paul Mackerras.
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
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/backlight.h>
#include <linux/platform_device.h>
#include <linux/err.h>
#include <linux/dmi.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/kfifo.h>
#include <linux/workqueue.h>
#include <linux/acpi.h>
#include <acpi/acpi_drivers.h>
#include <acpi/acpi_bus.h>
#include <asm/uaccess.h>
#include <linux/sony-laptop.h>
#include <linux/rfkill.h>
#ifdef CONFIG_SONYPI_COMPAT
#include <linux/poll.h>
#include <linux/miscdevice.h>
#endif
#include <linux/version.h>
#include "sonypi.h"

#define DRV_PFX			"sony-laptop: "
#define dprintk(msg...)		do {			\
	if (debug) printk(KERN_WARNING DRV_PFX  msg);	\
} while (0)

#define SONY_LAPTOP_DRIVER_VERSION	"0.9np3dev"

#define SONY_NC_CLASS		"sony-nc"
#define SONY_NC_HID		"SNY5001"
#define SONY_NC_DRIVER_NAME	"Sony Notebook Control Driver"

#define SONY_PIC_CLASS		"sony-pic"
#define SONY_PIC_HID		"SNY6001"
#define SONY_PIC_DRIVER_NAME	"Sony Programmable IO Control Driver"

MODULE_AUTHOR("Stelian Pop, Mattia Dongili");
MODULE_DESCRIPTION("Sony laptop extras driver (SPIC and SNC ACPI device)");
MODULE_LICENSE("GPL");
MODULE_VERSION(SONY_LAPTOP_DRIVER_VERSION);

static int debug;
module_param(debug, int, 0);
MODULE_PARM_DESC(debug, "set this to 1 (and RTFM) if you want to help "
		 "the development of this driver");

static int no_spic;		/* = 0 */
module_param(no_spic, int, 0444);
MODULE_PARM_DESC(no_spic,
		 "set this if you don't want to enable the SPIC device");

static int compat;		/* = 0 */
module_param(compat, int, 0444);
MODULE_PARM_DESC(compat,
		 "set this if you want to enable backward compatibility mode");

static unsigned long mask = 0xffffffff;
module_param(mask, ulong, 0644);
MODULE_PARM_DESC(mask,
		 "set this to the mask of event you want to enable (see doc)");

static int camera;		/* = 0 */
module_param(camera, int, 0444);
MODULE_PARM_DESC(camera,
                 "set this to 1 to enable Motion Eye camera controls "
                 "(only use it if you have a C1VE or C1VN model)");

static int speed_stamina;
module_param(speed_stamina, int, 0444);
MODULE_PARM_DESC(speed_stamina,
                 "Set this to 1 to enable SPEED mode on module load (EXPERIMENTAL)");

#ifdef CONFIG_SONYPI_COMPAT
static int minor = -1;
module_param(minor, int, 0);
MODULE_PARM_DESC(minor,
		 "minor number of the misc device for the SPIC compatibility code, "
		 "default is -1 (automatic)");
#endif

enum sony_nc_rfkill {
	SONY_WIFI,
	SONY_BLUETOOTH,
	SONY_WWAN,
	SONY_WIMAX,
	N_SONY_RFKILL
};

static struct rfkill *sony_rfkill_devices[N_SONY_RFKILL];
static int sony_rfkill_address[N_SONY_RFKILL] = {0x300, 0x500, 0x700, 0x900};
static void sony_nc_rfkill_update(void);

/*********** Input Devices ***********/

#define SONY_LAPTOP_BUF_SIZE	128
struct sony_laptop_input_s {
	atomic_t		users;
	struct input_dev	*jog_dev;
	struct input_dev	*key_dev;
	struct kfifo		*fifo;
	spinlock_t		fifo_lock;
	struct workqueue_struct	*wq;
};

static struct sony_laptop_input_s sony_laptop_input = {
	.users = ATOMIC_INIT(0),
};

struct sony_laptop_keypress {
	struct input_dev *dev;
	int key;
};

/* Correspondance table between sonypi events
 * and input layer indexes in the keymap
 */
static int sony_laptop_input_index[] = {
	-1,	/*  0 no event */
	-1,	/*  1 SONYPI_EVENT_JOGDIAL_DOWN */
	-1,	/*  2 SONYPI_EVENT_JOGDIAL_UP */
	-1,	/*  3 SONYPI_EVENT_JOGDIAL_DOWN_PRESSED */
	-1,	/*  4 SONYPI_EVENT_JOGDIAL_UP_PRESSED */
	-1,	/*  5 SONYPI_EVENT_JOGDIAL_PRESSED */
	-1,	/*  6 SONYPI_EVENT_JOGDIAL_RELEASED */
	 0,	/*  7 SONYPI_EVENT_CAPTURE_PRESSED */
	 1,	/*  8 SONYPI_EVENT_CAPTURE_RELEASED */
	 2,	/*  9 SONYPI_EVENT_CAPTURE_PARTIALPRESSED */
	 3,	/* 10 SONYPI_EVENT_CAPTURE_PARTIALRELEASED */
	 4,	/* 11 SONYPI_EVENT_FNKEY_ESC */
	 5,	/* 12 SONYPI_EVENT_FNKEY_F1 */
	 6,	/* 13 SONYPI_EVENT_FNKEY_F2 */
	 7,	/* 14 SONYPI_EVENT_FNKEY_F3 */
	 8,	/* 15 SONYPI_EVENT_FNKEY_F4 */
	 9,	/* 16 SONYPI_EVENT_FNKEY_F5 */
	10,	/* 17 SONYPI_EVENT_FNKEY_F6 */
	11,	/* 18 SONYPI_EVENT_FNKEY_F7 */
	12,	/* 19 SONYPI_EVENT_FNKEY_F8 */
	13,	/* 20 SONYPI_EVENT_FNKEY_F9 */
	14,	/* 21 SONYPI_EVENT_FNKEY_F10 */
	15,	/* 22 SONYPI_EVENT_FNKEY_F11 */
	16,	/* 23 SONYPI_EVENT_FNKEY_F12 */
	17,	/* 24 SONYPI_EVENT_FNKEY_1 */
	18,	/* 25 SONYPI_EVENT_FNKEY_2 */
	19,	/* 26 SONYPI_EVENT_FNKEY_D */
	20,	/* 27 SONYPI_EVENT_FNKEY_E */
	21,	/* 28 SONYPI_EVENT_FNKEY_F */
	22,	/* 29 SONYPI_EVENT_FNKEY_S */
	23,	/* 30 SONYPI_EVENT_FNKEY_B */
	24,	/* 31 SONYPI_EVENT_BLUETOOTH_PRESSED */
	25,	/* 32 SONYPI_EVENT_PKEY_P1 */
	26,	/* 33 SONYPI_EVENT_PKEY_P2 */
	27,	/* 34 SONYPI_EVENT_PKEY_P3 */
	28,	/* 35 SONYPI_EVENT_BACK_PRESSED */
	-1,	/* 36 SONYPI_EVENT_LID_CLOSED */
	-1,	/* 37 SONYPI_EVENT_LID_OPENED */
	29,	/* 38 SONYPI_EVENT_BLUETOOTH_ON */
	30,	/* 39 SONYPI_EVENT_BLUETOOTH_OFF */
	31,	/* 40 SONYPI_EVENT_HELP_PRESSED */
	32,	/* 41 SONYPI_EVENT_FNKEY_ONLY */
	33,	/* 42 SONYPI_EVENT_JOGDIAL_FAST_DOWN */
	34,	/* 43 SONYPI_EVENT_JOGDIAL_FAST_UP */
	35,	/* 44 SONYPI_EVENT_JOGDIAL_FAST_DOWN_PRESSED */
	36,	/* 45 SONYPI_EVENT_JOGDIAL_FAST_UP_PRESSED */
	37,	/* 46 SONYPI_EVENT_JOGDIAL_VFAST_DOWN */
	38,	/* 47 SONYPI_EVENT_JOGDIAL_VFAST_UP */
	39,	/* 48 SONYPI_EVENT_JOGDIAL_VFAST_DOWN_PRESSED */
	40,	/* 49 SONYPI_EVENT_JOGDIAL_VFAST_UP_PRESSED */
	41,	/* 50 SONYPI_EVENT_ZOOM_PRESSED */
	42,	/* 51 SONYPI_EVENT_THUMBPHRASE_PRESSED */
	43,	/* 52 SONYPI_EVENT_MEYE_FACE */
	44,	/* 53 SONYPI_EVENT_MEYE_OPPOSITE */
	45,	/* 54 SONYPI_EVENT_MEMORYSTICK_INSERT */
	46,	/* 55 SONYPI_EVENT_MEMORYSTICK_EJECT */
	-1,	/* 56 SONYPI_EVENT_ANYBUTTON_RELEASED */
	-1,	/* 57 SONYPI_EVENT_BATTERY_INSERT */
	-1,	/* 58 SONYPI_EVENT_BATTERY_REMOVE */
	-1,	/* 59 SONYPI_EVENT_FNKEY_RELEASED */
	47,	/* 60 SONYPI_EVENT_WIRELESS_ON */
	48,	/* 61 SONYPI_EVENT_WIRELESS_OFF */
	49,	/* 62 SONYPI_EVENT_ZOOM_IN_PRESSED */
	50,	/* 63 SONYPI_EVENT_ZOOM_OUT_PRESSED */
	51,	/* 64 SONYPI_EVENT_CD_EJECT_PRESSED */
	52,	/* 65 SONYPI_EVENT_MODEKEY_PRESSED */
	53,	/* 66 SONYPI_EVENT_PKEY_P4 */
	54,	/* 67 SONYPI_EVENT_PKEY_P5 */
	55,	/* 68 SONYPI_EVENT_SETTINGKEY_PRESSED */
	56,	/* 69 SONYPI_EVENT_VOLUME_INC_PRESSED */
	57,	/* 70 SONYPI_EVENT_VOLUME_DEC_PRESSED */
	-1,	/* 71 SONYPI_EVENT_BRIGHTNESS_PRESSED */
};

static int sony_laptop_input_keycode_map[] = {
	KEY_CAMERA,	/*  0 SONYPI_EVENT_CAPTURE_PRESSED */
	KEY_RESERVED,	/*  1 SONYPI_EVENT_CAPTURE_RELEASED */
	KEY_RESERVED,	/*  2 SONYPI_EVENT_CAPTURE_PARTIALPRESSED */
	KEY_RESERVED,	/*  3 SONYPI_EVENT_CAPTURE_PARTIALRELEASED */
	KEY_FN_ESC,	/*  4 SONYPI_EVENT_FNKEY_ESC */
	KEY_FN_F1,	/*  5 SONYPI_EVENT_FNKEY_F1 */
	KEY_FN_F2,	/*  6 SONYPI_EVENT_FNKEY_F2 */
	KEY_FN_F3,	/*  7 SONYPI_EVENT_FNKEY_F3 */
	KEY_FN_F4,	/*  8 SONYPI_EVENT_FNKEY_F4 */
	KEY_FN_F5,	/*  9 SONYPI_EVENT_FNKEY_F5 */
	KEY_FN_F6,	/* 10 SONYPI_EVENT_FNKEY_F6 */
	KEY_FN_F7,	/* 11 SONYPI_EVENT_FNKEY_F7 */
	KEY_FN_F8,	/* 12 SONYPI_EVENT_FNKEY_F8 */
	KEY_FN_F9,	/* 13 SONYPI_EVENT_FNKEY_F9 */
	KEY_FN_F10,	/* 14 SONYPI_EVENT_FNKEY_F10 */
	KEY_FN_F11,	/* 15 SONYPI_EVENT_FNKEY_F11 */
	KEY_FN_F12,	/* 16 SONYPI_EVENT_FNKEY_F12 */
	KEY_FN_F1,	/* 17 SONYPI_EVENT_FNKEY_1 */
	KEY_FN_F2,	/* 18 SONYPI_EVENT_FNKEY_2 */
	KEY_FN_D,	/* 19 SONYPI_EVENT_FNKEY_D */
	KEY_FN_E,	/* 20 SONYPI_EVENT_FNKEY_E */
	KEY_FN_F,	/* 21 SONYPI_EVENT_FNKEY_F */
	KEY_FN_S,	/* 22 SONYPI_EVENT_FNKEY_S */
	KEY_FN_B,	/* 23 SONYPI_EVENT_FNKEY_B */
	KEY_BLUETOOTH,	/* 24 SONYPI_EVENT_BLUETOOTH_PRESSED */
	KEY_PROG1,	/* 25 SONYPI_EVENT_PKEY_P1 */
	KEY_PROG2,	/* 26 SONYPI_EVENT_PKEY_P2 */
	KEY_PROG3,	/* 27 SONYPI_EVENT_PKEY_P3 */
	KEY_BACK,	/* 28 SONYPI_EVENT_BACK_PRESSED */
	KEY_BLUETOOTH,	/* 29 SONYPI_EVENT_BLUETOOTH_ON */
	KEY_BLUETOOTH,	/* 30 SONYPI_EVENT_BLUETOOTH_OFF */
	KEY_HELP,	/* 31 SONYPI_EVENT_HELP_PRESSED */
	KEY_FN,		/* 32 SONYPI_EVENT_FNKEY_ONLY */
	KEY_RESERVED,	/* 33 SONYPI_EVENT_JOGDIAL_FAST_DOWN */
	KEY_RESERVED,	/* 34 SONYPI_EVENT_JOGDIAL_FAST_UP */
	KEY_RESERVED,	/* 35 SONYPI_EVENT_JOGDIAL_FAST_DOWN_PRESSED */
	KEY_RESERVED,	/* 36 SONYPI_EVENT_JOGDIAL_FAST_UP_PRESSED */
	KEY_RESERVED,	/* 37 SONYPI_EVENT_JOGDIAL_VFAST_DOWN */
	KEY_RESERVED,	/* 38 SONYPI_EVENT_JOGDIAL_VFAST_UP */
	KEY_RESERVED,	/* 39 SONYPI_EVENT_JOGDIAL_VFAST_DOWN_PRESSED */
	KEY_RESERVED,	/* 40 SONYPI_EVENT_JOGDIAL_VFAST_UP_PRESSED */
	KEY_ZOOM,	/* 41 SONYPI_EVENT_ZOOM_PRESSED */
	BTN_THUMB,	/* 42 SONYPI_EVENT_THUMBPHRASE_PRESSED */
	KEY_RESERVED,	/* 43 SONYPI_EVENT_MEYE_FACE */
	KEY_RESERVED,	/* 44 SONYPI_EVENT_MEYE_OPPOSITE */
	KEY_RESERVED,	/* 45 SONYPI_EVENT_MEMORYSTICK_INSERT */
	KEY_RESERVED,	/* 46 SONYPI_EVENT_MEMORYSTICK_EJECT */
	KEY_WLAN,	/* 47 SONYPI_EVENT_WIRELESS_ON */
	KEY_WLAN,	/* 48 SONYPI_EVENT_WIRELESS_OFF */
	KEY_ZOOMIN,	/* 49 SONYPI_EVENT_ZOOM_IN_PRESSED */
	KEY_ZOOMOUT,	/* 50 SONYPI_EVENT_ZOOM_OUT_PRESSED */
	KEY_EJECTCD,	/* 51 SONYPI_EVENT_CD_EJECT_PRESSED */
	KEY_F13,	/* 52 SONYPI_EVENT_MODEKEY_PRESSED */
	KEY_PROG4,	/* 53 SONYPI_EVENT_PKEY_P4 */
	KEY_F14,	/* 54 SONYPI_EVENT_PKEY_P5 */
	KEY_F15,	/* 55 SONYPI_EVENT_SETTINGKEY_PRESSED */
	KEY_VOLUMEUP,	/* 56 SONYPI_EVENT_VOLUME_INC_PRESSED */
	KEY_VOLUMEDOWN,	/* 57 SONYPI_EVENT_VOLUME_DEC_PRESSED */
};

/* release buttons after a short delay if pressed */
static void do_sony_laptop_release_key(struct work_struct *work)
{
	struct sony_laptop_keypress kp;

	while (kfifo_get(sony_laptop_input.fifo, (unsigned char *)&kp,
			 sizeof(kp)) == sizeof(kp)) {
		msleep(10);
		input_report_key(kp.dev, kp.key, 0);
		input_sync(kp.dev);
	}
}
static DECLARE_WORK(sony_laptop_release_key_work,
		do_sony_laptop_release_key);

/* forward event to the input subsystem */
static void sony_laptop_report_input_event(u8 event)
{
	struct input_dev *jog_dev = sony_laptop_input.jog_dev;
	struct input_dev *key_dev = sony_laptop_input.key_dev;
	struct sony_laptop_keypress kp = { NULL };

	if (event == SONYPI_EVENT_FNKEY_RELEASED) {
		/* Nothing, not all VAIOs generate this event */
		return;
	}

	/* report events */
	switch (event) {
	/* jog_dev events */
	case SONYPI_EVENT_JOGDIAL_UP:
	case SONYPI_EVENT_JOGDIAL_UP_PRESSED:
		input_report_rel(jog_dev, REL_WHEEL, 1);
		input_sync(jog_dev);
		return;

	case SONYPI_EVENT_JOGDIAL_DOWN:
	case SONYPI_EVENT_JOGDIAL_DOWN_PRESSED:
		input_report_rel(jog_dev, REL_WHEEL, -1);
		input_sync(jog_dev);
		return;

	/* key_dev events */
	case SONYPI_EVENT_JOGDIAL_PRESSED:
		kp.key = BTN_MIDDLE;
		kp.dev = jog_dev;
		break;

	default:
		if (event >= ARRAY_SIZE(sony_laptop_input_index)) {
			dprintk("sony_laptop_report_input_event, event not known: %d\n", event);
			break;
		}
		if (sony_laptop_input_index[event] != -1) {
			kp.key = sony_laptop_input_keycode_map[sony_laptop_input_index[event]];
			if (kp.key != KEY_UNKNOWN)
				kp.dev = key_dev;
		}
		break;
	}

	if (kp.dev) {
		input_report_key(kp.dev, kp.key, 1);
		/* we emit the scancode so we can always remap the key */
		input_event(kp.dev, EV_MSC, MSC_SCAN, event);
		input_sync(kp.dev);
		kfifo_put(sony_laptop_input.fifo,
			  (unsigned char *)&kp, sizeof(kp));

		if (!work_pending(&sony_laptop_release_key_work))
			queue_work(sony_laptop_input.wq,
					&sony_laptop_release_key_work);
	} else
		dprintk("unknown input event %.2x\n", event);
}

static int sony_laptop_setup_input(struct acpi_device *acpi_device)
{
	struct input_dev *jog_dev;
	struct input_dev *key_dev;
	int i;
	int error;

	/* don't run again if already initialized */
	if (atomic_add_return(1, &sony_laptop_input.users) > 1)
		return 0;

	/* kfifo */
	spin_lock_init(&sony_laptop_input.fifo_lock);
	sony_laptop_input.fifo =
		kfifo_alloc(SONY_LAPTOP_BUF_SIZE, GFP_KERNEL,
			    &sony_laptop_input.fifo_lock);
	if (IS_ERR(sony_laptop_input.fifo)) {
		printk(KERN_ERR DRV_PFX "kfifo_alloc failed\n");
		error = PTR_ERR(sony_laptop_input.fifo);
		goto err_dec_users;
	}

	/* init workqueue */
	sony_laptop_input.wq = create_singlethread_workqueue("sony-laptop");
	if (!sony_laptop_input.wq) {
		printk(KERN_ERR DRV_PFX
				"Unabe to create workqueue.\n");
		error = -ENXIO;
		goto err_free_kfifo;
	}

	/* input keys */
	key_dev = input_allocate_device();
	if (!key_dev) {
		error = -ENOMEM;
		goto err_destroy_wq;
	}

	key_dev->name = "Sony Vaio Keys";
	key_dev->id.bustype = BUS_ISA;
	key_dev->id.vendor = PCI_VENDOR_ID_SONY;
	key_dev->dev.parent = &acpi_device->dev;

	/* Initialize the Input Drivers: special keys */
	set_bit(EV_KEY, key_dev->evbit);
	set_bit(EV_MSC, key_dev->evbit);
	set_bit(MSC_SCAN, key_dev->mscbit);
	key_dev->keycodesize = sizeof(sony_laptop_input_keycode_map[0]);
	key_dev->keycodemax = ARRAY_SIZE(sony_laptop_input_keycode_map);
	key_dev->keycode = &sony_laptop_input_keycode_map;
	for (i = 0; i < ARRAY_SIZE(sony_laptop_input_keycode_map); i++) {
		if (sony_laptop_input_keycode_map[i] != KEY_RESERVED) {
			set_bit(sony_laptop_input_keycode_map[i],
				key_dev->keybit);
		}
	}

	error = input_register_device(key_dev);
	if (error)
		goto err_free_keydev;

	sony_laptop_input.key_dev = key_dev;

	/* jogdial */
	jog_dev = input_allocate_device();
	if (!jog_dev) {
		error = -ENOMEM;
		goto err_unregister_keydev;
	}

	jog_dev->name = "Sony Vaio Jogdial";
	jog_dev->id.bustype = BUS_ISA;
	jog_dev->id.vendor = PCI_VENDOR_ID_SONY;
	key_dev->dev.parent = &acpi_device->dev;

	jog_dev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_REL);
	jog_dev->keybit[BIT_WORD(BTN_MOUSE)] = BIT_MASK(BTN_MIDDLE);
	jog_dev->relbit[0] = BIT_MASK(REL_WHEEL);

	error = input_register_device(jog_dev);
	if (error)
		goto err_free_jogdev;

	sony_laptop_input.jog_dev = jog_dev;

	return 0;

err_free_jogdev:
	input_free_device(jog_dev);

err_unregister_keydev:
	input_unregister_device(key_dev);
	/* to avoid kref underflow below at input_free_device */
	key_dev = NULL;

err_free_keydev:
	input_free_device(key_dev);

err_destroy_wq:
	destroy_workqueue(sony_laptop_input.wq);

err_free_kfifo:
	kfifo_free(sony_laptop_input.fifo);

err_dec_users:
	atomic_dec(&sony_laptop_input.users);
	return error;
}

static void sony_laptop_remove_input(void)
{
	/* cleanup only after the last user has gone */
	if (!atomic_dec_and_test(&sony_laptop_input.users))
		return;

	/* flush workqueue first */
	flush_workqueue(sony_laptop_input.wq);

	/* destroy input devs */
	input_unregister_device(sony_laptop_input.key_dev);
	sony_laptop_input.key_dev = NULL;

	if (sony_laptop_input.jog_dev) {
		input_unregister_device(sony_laptop_input.jog_dev);
		sony_laptop_input.jog_dev = NULL;
	}

	destroy_workqueue(sony_laptop_input.wq);
	kfifo_free(sony_laptop_input.fifo);
}

/*********** Platform Device ***********/
static int sony_ovga_dsm(int func, int arg)
{
        static char *path = "\\_SB.PCI0.OVGA._DSM";
        static char muid[] = {
                /*00*/  0xA0, 0xA0, 0x95, 0x9D, 0x60, 0x00, 0x48, 0x4D,         /* MUID */
                /*08*/  0xB3, 0x4D, 0x7E, 0x5F, 0xEA, 0x12, 0x9F, 0xD4,
        };

        struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
        struct acpi_object_list input;
        union acpi_object params[4];
        int result;

        input.count = 4;
        input.pointer = params;
        params[0].type = ACPI_TYPE_BUFFER;
        params[0].buffer.length = sizeof(muid);
        params[0].buffer.pointer = (char*)muid;
        params[1].type = ACPI_TYPE_INTEGER;
        params[1].integer.value = 0x00000102;
        params[2].type = ACPI_TYPE_INTEGER;
        params[2].integer.value = func;
        params[3].type = ACPI_TYPE_INTEGER;
        params[3].integer.value = arg;

        result = acpi_evaluate_object(NULL, (char*)path, &input, &output);
        if (result) {
                printk("%s failed: %d\n", path, result);
                return -1;
        }

#ifdef DEBUG
        {
                union acpi_object *obj = (union acpi_object*)output.pointer;
                if (obj->type == ACPI_TYPE_PACKAGE) {
                        int i;
                        printk("returned package sized %d\n", obj->package.count);
                        for (i = 0; i < obj->package.count; i++)
                                printk("%d %08x\n", i, obj->package.elements[i].integer.value);
                } else
                if (obj->type == ACPI_TYPE_INTEGER) {
                        printk("returned integer %08X\n", obj->integer.value);
                } else
                if (obj->type == ACPI_TYPE_BUFFER) {
                        int i;
                        printk("returned buffer sized %d\n", obj->buffer.length);
                        for (i = 0; i < obj->buffer.length; i++)
                                printk("%d %02x\n", i, obj->buffer.pointer[i]);
                }
        }
#endif
        kfree(output.pointer);
        return 0;
}

static int sony_led_stamina(void)
{
        return sony_ovga_dsm(2, 0x11);
}

static int sony_led_speed(void)
{
        return sony_ovga_dsm(2, 0x12);
}

#ifdef DEBUG
static int sony_led_off(void)
{
        return sony_ovga_dsm(2, 0x13);
}

static int sony_dgpu_sta(void)
{
        return sony_ovga_dsm(3, 0x00);
}
#endif

static int sony_dgpu_off(void)
{
        return sony_ovga_dsm(3, 0x02);
}

static int sony_dgpu_on(void)
{
        return sony_ovga_dsm(3, 0x01);
}

static ssize_t sony_pf_store_speed_stamina(struct device *dev,
                               struct device_attribute *attr,
                               const char *buffer, size_t count)
{
        if (!strncmp(buffer, "speed", strlen("speed"))) {
                sony_dgpu_on();
                sony_led_speed();
                speed_stamina = 1;
        } else
        if (!strncmp(buffer, "stamina", strlen("stamina"))) {
                sony_dgpu_off();
                sony_led_stamina();
                speed_stamina = 0;
        } else
                return -EINVAL;

        return count;
}

static ssize_t sony_pf_show_speed_stamina(struct device *dev,
                struct device_attribute *attr, char *buffer)
{
        return snprintf(buffer, PAGE_SIZE, "%s\n", speed_stamina ? "speed":"stamina");
}

static struct device_attribute sony_pf_speed_stamina_attr =
        __ATTR(speed_stamina, S_IWUSR|S_IRUGO,
                sony_pf_show_speed_stamina, sony_pf_store_speed_stamina);

static int sony_pf_probe(struct platform_device *pdev)
{
        int result;

        result = device_create_file(&pdev->dev, &sony_pf_speed_stamina_attr);
        if (result)
                printk(KERN_DEBUG "sony_pf_probe: failed to add speed/stamina switch\n");

        /* initialize default, look at module param speed_stamina */
        if (speed_stamina == 1) {
                sony_dgpu_on();
                sony_led_speed();
        } else  if (speed_stamina == 0){
                sony_dgpu_off();
                sony_led_stamina();
        }

        return 0;
}

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,31)
static int sony_resume_noirq(struct device *pdev)
#else
static int sony_pf_resume(struct platform_device *pdev)
#endif
{
        /* on resume, restore previous state */
        if (speed_stamina == 1) {
                sony_dgpu_on();
                sony_led_speed();
        } else if (speed_stamina == 0){
                sony_dgpu_off();
                sony_led_stamina();
        }
        return 0;
}

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,31)
static struct dev_pm_ops sony_dev_pm_ops = {
	.resume_noirq = sony_resume_noirq,
};
#endif

static atomic_t sony_pf_users = ATOMIC_INIT(0);
static struct platform_driver sony_pf_driver = {
        .probe  = sony_pf_probe,
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,31)
#else
	.resume_early = sony_pf_resume,
#endif
        .driver = {
                   .name = "sony-laptop",
                   .owner = THIS_MODULE,
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,31)
#ifdef CONFIG_PM
        	   .pm = &sony_dev_pm_ops,
#endif
#endif
                   }
};
static struct platform_device *sony_pf_device;

static int sony_pf_add(void)
{
	int ret = 0;

	/* don't run again if already initialized */
	if (atomic_add_return(1, &sony_pf_users) > 1)
		return 0;

	ret = platform_driver_register(&sony_pf_driver);
	if (ret)
		goto out;

	sony_pf_device = platform_device_alloc("sony-laptop", -1);
	if (!sony_pf_device) {
		ret = -ENOMEM;
		goto out_platform_registered;
	}

	ret = platform_device_add(sony_pf_device);
	if (ret)
		goto out_platform_alloced;

	return 0;

      out_platform_alloced:
	platform_device_put(sony_pf_device);
	sony_pf_device = NULL;
      out_platform_registered:
	platform_driver_unregister(&sony_pf_driver);
      out:
	atomic_dec(&sony_pf_users);
	return ret;
}

static void sony_pf_remove(void)
{
	/* deregister only after the last user has gone */
	if (!atomic_dec_and_test(&sony_pf_users))
		return;

	platform_device_del(sony_pf_device);
	platform_device_put(sony_pf_device);
	platform_driver_unregister(&sony_pf_driver);
}

/*********** SNC (SNY5001) Device ***********/

/* the device uses 1-based values, while the backlight subsystem uses
   0-based values */
#define SONY_MAX_BRIGHTNESS	8

#define SNC_VALIDATE_IN		0
#define SNC_VALIDATE_OUT	1

static ssize_t sony_nc_sysfs_show(struct device *, struct device_attribute *,
			      char *);
static ssize_t sony_nc_sysfs_store(struct device *, struct device_attribute *,
			       const char *, size_t);
static int boolean_validate(const int, const int);
static int brightness_default_validate(const int, const int);

struct sony_nc_value {
	char *name;		/* name of the entry */
	char **acpiget;		/* names of the ACPI get function */
	char **acpiset;		/* names of the ACPI set function */
	int (*validate)(const int, const int);	/* input/output validation */
	int value;		/* current setting */
	int valid;		/* Has ever been set */
	int debug;		/* active only in debug mode ? */
	struct device_attribute devattr;	/* sysfs atribute */
};

#define SNC_HANDLE_NAMES(_name, _values...) \
	static char *snc_##_name[] = { _values, NULL }

#define SNC_HANDLE(_name, _getters, _setters, _validate, _debug) \
	{ \
		.name		= __stringify(_name), \
		.acpiget	= _getters, \
		.acpiset	= _setters, \
		.validate	= _validate, \
		.debug		= _debug, \
		.devattr	= __ATTR(_name, 0, sony_nc_sysfs_show, sony_nc_sysfs_store), \
	}

#define SNC_HANDLE_NULL	{ .name = NULL }

SNC_HANDLE_NAMES(fnkey_get, "GHKE");

SNC_HANDLE_NAMES(brightness_def_get, "GPBR");
SNC_HANDLE_NAMES(brightness_def_set, "SPBR");

SNC_HANDLE_NAMES(cdpower_get, "GCDP");
SNC_HANDLE_NAMES(cdpower_set, "SCDP", "CDPW");

SNC_HANDLE_NAMES(audiopower_get, "GAZP");
SNC_HANDLE_NAMES(audiopower_set, "AZPW");

SNC_HANDLE_NAMES(lanpower_get, "GLNP");
SNC_HANDLE_NAMES(lanpower_set, "LNPW");

SNC_HANDLE_NAMES(lidstate_get, "GLID");

SNC_HANDLE_NAMES(indicatorlamp_get, "GILS");
SNC_HANDLE_NAMES(indicatorlamp_set, "SILS");

SNC_HANDLE_NAMES(gainbass_get, "GMGB");
SNC_HANDLE_NAMES(gainbass_set, "CMGB");

SNC_HANDLE_NAMES(PID_get, "GPID");

SNC_HANDLE_NAMES(CTR_get, "GCTR");
SNC_HANDLE_NAMES(CTR_set, "SCTR");

SNC_HANDLE_NAMES(PCR_get, "GPCR");
SNC_HANDLE_NAMES(PCR_set, "SPCR");

SNC_HANDLE_NAMES(CMI_get, "GCMI");
SNC_HANDLE_NAMES(CMI_set, "SCMI");

/* NORB */
SNC_HANDLE_NAMES(SN_get, "SNIN");
SNC_HANDLE_NAMES(SN_set, "SNNE", "SNCF");

SNC_HANDLE_NAMES(HSC0_get, "HSC0");
SNC_HANDLE_NAMES(HSC1_get, "HSC1");
SNC_HANDLE_NAMES(HSC2_get, "HSC2");
SNC_HANDLE_NAMES(HSC3_set, "HSC3");
SNC_HANDLE_NAMES(HSC4_set, "HSC4");

SNC_HANDLE_NAMES(F100_get, "F100");
SNC_HANDLE_NAMES(F113_get, "F113");
SNC_HANDLE_NAMES(F101_get, "F101");
SNC_HANDLE_NAMES(F105_get, "F105");
SNC_HANDLE_NAMES(F114_get, "F114");
SNC_HANDLE_NAMES(F115_get, "F115");
SNC_HANDLE_NAMES(F11D_get, "F11D");
SNC_HANDLE_NAMES(F119_get, "F119");
SNC_HANDLE_NAMES(F121_get, "F121");
SNC_HANDLE_NAMES(F122_get, "F122");
SNC_HANDLE_NAMES(F124_get, "F124");
SNC_HANDLE_NAMES(F125_get, "F125");
SNC_HANDLE_NAMES(F126_get, "F126");
SNC_HANDLE_NAMES(F128_get, "F128");
SNC_HANDLE_NAMES(HOMP_get, "HOMP");

SNC_HANDLE_NAMES(SN01_get, "SNO1");
SNC_HANDLE_NAMES(SN03_set, "SNO3");
SNC_HANDLE_NAMES(SN04_get, "SNO4");
SNC_HANDLE_NAMES(SN05_set, "SNO5");
SNC_HANDLE_NAMES(SN06_set, "SNO6");

SNC_HANDLE_NAMES(PWAK_get, "PWAK");
SNC_HANDLE_NAMES(EAWK_set, "EAWK");


static struct sony_nc_value sony_nc_values[] = {
	SNC_HANDLE(brightness_default, snc_brightness_def_get,
			snc_brightness_def_set, brightness_default_validate, 0),
	SNC_HANDLE(fnkey, snc_fnkey_get, NULL, NULL, 0),
	SNC_HANDLE(cdpower, snc_cdpower_get, snc_cdpower_set, boolean_validate, 0),
	SNC_HANDLE(audiopower, snc_audiopower_get, snc_audiopower_set,
			boolean_validate, 0),
	SNC_HANDLE(lanpower, snc_lanpower_get, snc_lanpower_set,
			boolean_validate, 1),
	SNC_HANDLE(lidstate, snc_lidstate_get, NULL,
			boolean_validate, 0),
	SNC_HANDLE(indicatorlamp, snc_indicatorlamp_get, snc_indicatorlamp_set,
			boolean_validate, 0),
	SNC_HANDLE(gainbass, snc_gainbass_get, snc_gainbass_set,
			boolean_validate, 0),
	/* unknown methods */
	SNC_HANDLE(PID, snc_PID_get, NULL, NULL, 1),
	SNC_HANDLE(CTR, snc_CTR_get, snc_CTR_set, NULL, 1),
	SNC_HANDLE(PCR, snc_PCR_get, snc_PCR_set, NULL, 1),
	SNC_HANDLE(CMI, snc_CMI_get, snc_CMI_set, NULL, 1),
	/* NORB */
	SNC_HANDLE(SN, snc_SN_get, snc_SN_set, NULL, 1),

	SNC_HANDLE(HSC0, snc_HSC0_get, NULL, NULL, 1),
	SNC_HANDLE(HSC3, NULL, snc_HSC3_set, NULL, 1),
	SNC_HANDLE(HSC1, snc_HSC1_get, NULL, NULL, 1),
	SNC_HANDLE(HSC4, NULL, snc_HSC4_set, NULL, 1),
	SNC_HANDLE(HSC2,  snc_HSC2_get, NULL, NULL, 1),

	SNC_HANDLE(F100,  snc_F100_get, NULL, NULL, 1),
	SNC_HANDLE(F113,  snc_F113_get, NULL, NULL, 1),
	SNC_HANDLE(F101,  snc_F101_get, NULL, NULL, 1),
	SNC_HANDLE(F114,  snc_F114_get, NULL, NULL, 1),
	SNC_HANDLE(F115,  snc_F115_get, NULL, NULL, 1),
	SNC_HANDLE(F11D,  snc_F11D_get, NULL, NULL, 1),
	SNC_HANDLE(F119,  snc_F119_get, NULL, NULL, 1),
	SNC_HANDLE(F121,  snc_F121_get, NULL, NULL, 1),
	SNC_HANDLE(F122,  snc_F122_get, NULL, NULL, 1),
	SNC_HANDLE(F124,  snc_F124_get, NULL, NULL, 1),
	SNC_HANDLE(F125,  snc_F125_get, NULL, NULL, 1),
	SNC_HANDLE(F126,  snc_F126_get, NULL, NULL, 1),
	SNC_HANDLE(F128,  snc_F128_get, NULL, NULL, 1),
	SNC_HANDLE(F105,  snc_F105_get, NULL, NULL, 1),
	SNC_HANDLE(HOMP,  snc_HOMP_get, NULL, NULL, 1),
	SNC_HANDLE(SN01,  snc_SN01_get, NULL, NULL, 1),
	SNC_HANDLE(SN03,  NULL, snc_SN03_set, NULL, 1),
	SNC_HANDLE(SN04,  snc_SN04_get, NULL, NULL, 1),
	SNC_HANDLE(SN05,  NULL, snc_SN05_set, NULL, 1),
	SNC_HANDLE(SN06,  NULL, snc_SN06_set, NULL, 1),

	SNC_HANDLE(PWAK,  snc_PWAK_get, NULL, NULL, 1),
	SNC_HANDLE(EAWK,  NULL, snc_EAWK_set, NULL, 1),
	SNC_HANDLE_NULL
};

static acpi_handle sony_nc_acpi_handle;
static struct acpi_device *sony_nc_acpi_device = NULL;

/*
 * acpi_evaluate_object wrappers
 */
static int acpi_callgetfunc(acpi_handle handle, char *name, int *result)
{
	struct acpi_buffer output;
	union acpi_object out_obj;
	acpi_status status;

	output.length = sizeof(out_obj);
	output.pointer = &out_obj;

	status = acpi_evaluate_object(handle, name, NULL, &output);
	if ((status == AE_OK) && (out_obj.type == ACPI_TYPE_INTEGER)) {
		*result = out_obj.integer.value;
		return 0;
	}

	printk(KERN_WARNING DRV_PFX "acpi_callreadfunc failed\n");

	return -1;
}

static int acpi_callsetfunc(acpi_handle handle, char *name, int value,
			    int *result)
{
	struct acpi_object_list params;
	union acpi_object in_obj;
	struct acpi_buffer output;
	union acpi_object out_obj;
	acpi_status status;

	params.count = 1;
	params.pointer = &in_obj;
	in_obj.type = ACPI_TYPE_INTEGER;
	in_obj.integer.value = value;

	output.length = sizeof(out_obj);
	output.pointer = &out_obj;

	status = acpi_evaluate_object(handle, name, &params, &output);
	if (status == AE_OK) {
		if (result != NULL) {
			if (out_obj.type != ACPI_TYPE_INTEGER) {
				printk(KERN_WARNING DRV_PFX "acpi_evaluate_object bad "
				       "return type\n");
				return -1;
			}
			*result = out_obj.integer.value;
		}
		return 0;
	}

	printk(KERN_WARNING DRV_PFX "acpi_evaluate_object failed\n");

	return -1;
}

static int sony_find_snc_handle(int handle)
{
	int i;
	int result;

	for (i = 0x20; i < 0x30; i++) {
		acpi_callsetfunc(sony_nc_acpi_handle, "SN00", i, &result);
		if (result == handle)
			return i-0x20;
	}

	return -1;
}

static int sony_call_snc_handle(int handle, int argument, int *result)
{
	int offset = sony_find_snc_handle(handle);

	if (offset < 0)
		return -1;

	return acpi_callsetfunc(sony_nc_acpi_handle, "SN07", offset | argument,
				result);
}

/*
 * sony_nc_values input/output validate functions
 */

/* brightness_default_validate:
 *
 * manipulate input output values to keep consistency with the
 * backlight framework for which brightness values are 0-based.
 */
static int brightness_default_validate(const int direction, const int value)
{
	switch (direction) {
		case SNC_VALIDATE_OUT:
			return value - 1;
		case SNC_VALIDATE_IN:
			if (value >= 0 && value < SONY_MAX_BRIGHTNESS)
				return value + 1;
	}
	return -EINVAL;
}

/* boolean_validate:
 *
 * on input validate boolean values 0/1, on output just pass the
 * received value.
 */
static int boolean_validate(const int direction, const int value)
{
	if (direction == SNC_VALIDATE_IN) {
		if (value != 0 && value != 1)
			return -EINVAL;
	}
	return value;
}

/*
 * Sysfs show/store common to all sony_nc_values
 */
static ssize_t sony_nc_sysfs_show(struct device *dev, struct device_attribute *attr,
			      char *buffer)
{
	int value;
	struct sony_nc_value *item =
	    container_of(attr, struct sony_nc_value, devattr);

	if (!*item->acpiget)
		return -EIO;

	if (acpi_callgetfunc(sony_nc_acpi_handle, *item->acpiget, &value) < 0)
		return -EIO;

	if (item->validate)
		value = item->validate(SNC_VALIDATE_OUT, value);

	return snprintf(buffer, PAGE_SIZE, "%d\n", value);
}

static ssize_t sony_nc_sysfs_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buffer, size_t count)
{
	int value;
	struct sony_nc_value *item =
	    container_of(attr, struct sony_nc_value, devattr);

	if (!item->acpiset)
		return -EIO;

	if (count > 31)
		return -EINVAL;

	value = simple_strtoul(buffer, NULL, 10);

	if (item->validate)
		value = item->validate(SNC_VALIDATE_IN, value);

	if (value < 0)
		return value;

	if (acpi_callsetfunc(sony_nc_acpi_handle, *item->acpiset, value, NULL) < 0)
		return -EIO;
	item->value = value;
	item->valid = 1;
	return count;
}


/*
 * Backlight device
 */
static int sony_backlight_update_status(struct backlight_device *bd)
{
	return acpi_callsetfunc(sony_nc_acpi_handle, "SBRT",
				bd->props.brightness + 1, NULL);
}

static int sony_backlight_get_brightness(struct backlight_device *bd)
{
	int value;

	if (acpi_callgetfunc(sony_nc_acpi_handle, "GBRT", &value))
		return 0;
	/* brightness levels are 1-based, while backlight ones are 0-based */
	return value - 1;
}

static struct backlight_device *sony_backlight_device;
static struct backlight_ops sony_backlight_ops = {
	.update_status = sony_backlight_update_status,
	.get_brightness = sony_backlight_get_brightness,
};

/*
 * New SNC-only Vaios event mapping to driver known keys
 */
struct sony_nc_event {
	u8	data;
	u8	event;
};

static struct sony_nc_event sony_100_events[] = {
	{ 0x90, SONYPI_EVENT_PKEY_P1 },
	{ 0x10, SONYPI_EVENT_ANYBUTTON_RELEASED },
	{ 0x91, SONYPI_EVENT_PKEY_P2 },
	{ 0x11, SONYPI_EVENT_ANYBUTTON_RELEASED },
	{ 0x81, SONYPI_EVENT_FNKEY_F1 },
	{ 0x01, SONYPI_EVENT_FNKEY_RELEASED },
	{ 0x82, SONYPI_EVENT_FNKEY_F2 },
	{ 0x02, SONYPI_EVENT_FNKEY_RELEASED },
	{ 0x83, SONYPI_EVENT_FNKEY_F3 },
	{ 0x03, SONYPI_EVENT_FNKEY_RELEASED },
	{ 0x84, SONYPI_EVENT_FNKEY_F4 },
	{ 0x04, SONYPI_EVENT_FNKEY_RELEASED },
	{ 0x85, SONYPI_EVENT_FNKEY_F5 },
	{ 0x05, SONYPI_EVENT_FNKEY_RELEASED },
	{ 0x86, SONYPI_EVENT_FNKEY_F6 },
	{ 0x06, SONYPI_EVENT_FNKEY_RELEASED },
	{ 0x87, SONYPI_EVENT_FNKEY_F7 },
	{ 0x07, SONYPI_EVENT_FNKEY_RELEASED },
	{ 0x89, SONYPI_EVENT_FNKEY_F9 },
	{ 0x09, SONYPI_EVENT_FNKEY_RELEASED },
	{ 0x8A, SONYPI_EVENT_FNKEY_F10 },
	{ 0x0A, SONYPI_EVENT_FNKEY_RELEASED },
	{ 0x8C, SONYPI_EVENT_FNKEY_F12 },
	{ 0x0C, SONYPI_EVENT_FNKEY_RELEASED },
	{ 0x9f, SONYPI_EVENT_CD_EJECT_PRESSED },
	{ 0x1f, SONYPI_EVENT_ANYBUTTON_RELEASED },
	{ 0, 0 },
};

static struct sony_nc_event sony_127_events[] = {
	{ 0x81, SONYPI_EVENT_MODEKEY_PRESSED },
	{ 0x01, SONYPI_EVENT_ANYBUTTON_RELEASED },
	{ 0x82, SONYPI_EVENT_PKEY_P1 },
	{ 0x02, SONYPI_EVENT_ANYBUTTON_RELEASED },
	{ 0x83, SONYPI_EVENT_PKEY_P2 },
	{ 0x03, SONYPI_EVENT_ANYBUTTON_RELEASED },
	{ 0x84, SONYPI_EVENT_PKEY_P3 },
	{ 0x04, SONYPI_EVENT_ANYBUTTON_RELEASED },
	{ 0x85, SONYPI_EVENT_PKEY_P4 },
	{ 0x05, SONYPI_EVENT_ANYBUTTON_RELEASED },
	{ 0x86, SONYPI_EVENT_PKEY_P5 },
	{ 0x06, SONYPI_EVENT_ANYBUTTON_RELEASED },
	{ 0x06, SONYPI_EVENT_ANYBUTTON_RELEASED },
	{ 0x87, SONYPI_EVENT_SETTINGKEY_PRESSED },
	{ 0x07, SONYPI_EVENT_ANYBUTTON_RELEASED },
	{ 0, 0 },
};

/*
 * ACPI callbacks
 */
static void sony_acpi_notify(acpi_handle handle, u32 event, void *data)
{
	u32 ev = event;
	int result;

	if (ev >= 0x90) {
		/* New-style event */
		int key_handle = 0;
		ev -= 0x90;

		if (sony_find_snc_handle(0x100) == ev)
			key_handle = 0x100;
		if (sony_find_snc_handle(0x127) == ev)
			key_handle = 0x127;

		if (key_handle) {
			struct sony_nc_event *key_event;

			if (sony_call_snc_handle(key_handle, 0x200, &result))
				dprintk("sony_acpi_notify, unable to decode"
					" event 0x%.2x 0x%.2x\n", key_handle,
					ev);
			else
				ev = result & 0xFF;

			if (key_handle == 0x100)
				key_event = sony_100_events;
			else
				key_event = sony_127_events;

			for (; key_event->data; key_event++) {
				if (key_event->data == ev) {
					ev = key_event->event;
					break;
				}
			}

			if (!key_event->data) {
				printk(KERN_INFO DRV_PFX
				       "Unknown event: 0x%x 0x%x\n", key_handle,
				       ev);
			} else
				sony_laptop_report_input_event(ev);

			/* mark event as translated */
			ev |= 0x40000000;
		} else if (sony_find_snc_handle(0x124) == ev) {
			sony_nc_rfkill_update();
			/* restore original event number */
			ev = event;
		} else if (ev == 0xc) {
			int result;
			if (!ACPI_SUCCESS(acpi_callgetfunc(
					handle, "HSC1", &result))) {
				dprintk("sony_acpi_notify: "
					"cannot query speed/stamina switch\n");
				return;
			}
			
			/* restore original event number */
			ev = event;
			
			if (result & 0x02)
				acpi_bus_generate_proc_event(
						sony_nc_acpi_device, 1, ev);
			else
				acpi_bus_generate_proc_event(
						sony_nc_acpi_device, 0, ev);
			return;
		}
	} else
		sony_laptop_report_input_event(ev);
	
	dprintk("sony_acpi_notify, event: 0x%.2x\n", ev);
	acpi_bus_generate_proc_event(sony_nc_acpi_device, 1, ev);
}

static acpi_status sony_walk_callback(acpi_handle handle, u32 level,
				      void *context, void **return_value)
{
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,27)
        struct acpi_device_info *info;
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,31)
        if (ACPI_SUCCESS(acpi_get_object_info(handle, &info))) {
#else
        struct acpi_buffer buffer = {ACPI_ALLOCATE_BUFFER, NULL};

        if (ACPI_SUCCESS(acpi_get_object_info(handle, &buffer))) {
                info = buffer.pointer;
#endif
                
                printk(KERN_WARNING DRV_PFX "method: name: %4.4s, args %X\n",
                        (char *)&info->name, info->param_count);

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,31)
		kfree(info);
#else
                kfree(buffer.pointer);
#endif
        }
#else
        struct acpi_namespace_node *node;
        union acpi_operand_object *operand;

        node = (struct acpi_namespace_node *)handle;
        operand = (union acpi_operand_object *)node->object;

        printk(KERN_WARNING DRV_PFX "method: name: %4.4s, args %X\n", node->name.ascii,
               (u32) operand->method.param_count);
#endif
        return AE_OK;
}

/*
 * ACPI device
 */
static int sony_nc_function_setup(struct acpi_device *device)
{
	int result;

	/* Enable all events */
	acpi_callsetfunc(sony_nc_acpi_handle, "SN02", 0xffff, &result);

	/* Setup hotkeys */
	sony_call_snc_handle(0x0100, 0, &result);
	sony_call_snc_handle(0x0101, 0, &result);
	sony_call_snc_handle(0x0102, 0x100, &result);

	return 0;
}

static int sony_nc_resume(struct acpi_device *device)
{
	struct sony_nc_value *item;
	acpi_handle handle;

	for (item = sony_nc_values; item->name; item++) {
		int ret;

		if (!item->valid)
			continue;
		ret = acpi_callsetfunc(sony_nc_acpi_handle, *item->acpiset,
				       item->value, NULL);
		if (ret < 0) {
			printk("%s: %d\n", __func__, ret);
			break;
		}
	}

	if (ACPI_SUCCESS(acpi_get_handle(sony_nc_acpi_handle, "ECON",
					 &handle))) {
		if (acpi_callsetfunc(sony_nc_acpi_handle, "ECON", 1, NULL))
			dprintk("ECON Method failed\n");
	}

	if (ACPI_SUCCESS(acpi_get_handle(sony_nc_acpi_handle, "SN00",
					 &handle))) {
		dprintk("Doing SNC setup\n");
		sony_nc_function_setup(device);
	}

	/* set the last requested brightness level */
	if (sony_backlight_device &&
			!sony_backlight_update_status(sony_backlight_device))
		printk(KERN_WARNING DRV_PFX "unable to restore brightness level\n");
	
	/* re-read rfkill state */
	sony_nc_rfkill_update();

	return 0;
}

static void sony_nc_rfkill_cleanup(void)
{
	int i;

	for (i = 0; i < N_SONY_RFKILL; i++) {
		if (sony_rfkill_devices[i]) {
			rfkill_unregister(sony_rfkill_devices[i]);
			rfkill_destroy(sony_rfkill_devices[i]);
		}
	}
}

static int sony_nc_rfkill_set(void *data, bool blocked)
{
	int result;
	int argument = sony_rfkill_address[(long) data] + 0x100;

	if (!blocked)
		argument |= 0xff0000;

	return sony_call_snc_handle(0x124, argument, &result);
}

static const struct rfkill_ops sony_rfkill_ops = {
	.set_block = sony_nc_rfkill_set,
};

static int sony_nc_setup_rfkill(struct acpi_device *device,
				enum sony_nc_rfkill nc_type)
{
	int err = 0;
	struct rfkill *rfk;
	enum rfkill_type type;
	int result;
	bool hwblock;

	const char *name;

	switch (nc_type) {
	case SONY_WIFI:
		type = RFKILL_TYPE_WLAN;
		name = "sony-wifi";
		break;
	case SONY_BLUETOOTH:
		type = RFKILL_TYPE_BLUETOOTH;
		name = "sony-bluetooth";
		break;
	case SONY_WWAN:
		type = RFKILL_TYPE_WWAN;
		name = "sony-wwan";
		break;
	case SONY_WIMAX:
		type = RFKILL_TYPE_WIMAX;
		name = "sony-wimax";
		break;
	default:
		return -EINVAL;
	}

	rfk = rfkill_alloc(name, &device->dev, type,
			   &sony_rfkill_ops, (void *)nc_type);
	if (!rfk)
		return -ENOMEM;
	
	sony_call_snc_handle(0x124, 0x200, &result);
	hwblock = !(result & 0x1);
	rfkill_set_hw_state(rfk, hwblock);

	err = rfkill_register(rfk);
	if (err) {
		rfkill_destroy(rfk);
		return err;
	}
	sony_rfkill_devices[nc_type] = rfk;
	return err;
}

static void sony_nc_rfkill_update()
{
	enum sony_nc_rfkill i;
	int result;
	bool hwblock;

	sony_call_snc_handle(0x124, 0x200, &result);
	hwblock = !(result & 0x1);

	for (i = 0; i < N_SONY_RFKILL; i++) {
		int argument = sony_rfkill_address[i];

		if (!sony_rfkill_devices[i])
			continue;

		if (hwblock) {
			if (rfkill_set_hw_state(sony_rfkill_devices[i], true)) {
				/* we already know we're blocked */
			}
			continue;
		}

		sony_call_snc_handle(0x124, argument, &result);
		rfkill_set_states(sony_rfkill_devices[i],
				  !(result & 0xf), false);
	}
}

static int sony_nc_rfkill_setup(struct acpi_device *device)
{
	int result, ret;

	if (sony_find_snc_handle(0x124) == -1)
		return -1;

	ret = sony_call_snc_handle(0x124, 0xb00, &result);
	if (ret) {
		printk(KERN_INFO DRV_PFX
		       "Unable to enumerate rfkill devices: %x\n", ret);
		return ret;
	}

	if (result & 0x1)
		sony_nc_setup_rfkill(device, SONY_WIFI);
	if (result & 0x2)
		sony_nc_setup_rfkill(device, SONY_BLUETOOTH);
	if (result & 0x1c)
		sony_nc_setup_rfkill(device, SONY_WWAN);
	if (result & 0x20)
		sony_nc_setup_rfkill(device, SONY_WIMAX);

	return 0;
}

static int sony_nc_add(struct acpi_device *device)
{
	acpi_status status;
	int result = 0;
	acpi_handle handle;
	struct sony_nc_value *item;

	printk(KERN_INFO DRV_PFX "%s v%s.\n",
		SONY_NC_DRIVER_NAME, SONY_LAPTOP_DRIVER_VERSION);

	sony_nc_acpi_device = device;
	strcpy(acpi_device_class(device), "sony/hotkey");

	sony_nc_acpi_handle = device->handle;

	/* read device status */
	result = acpi_bus_get_status(device);
	/* bail IFF the above call was successful and the device is not present */
	if (!result && !device->status.present) {
		dprintk("Device not present\n");
		result = -ENODEV;
		goto outwalk;
	}

	if (debug) {
		status = acpi_walk_namespace(ACPI_TYPE_METHOD, sony_nc_acpi_handle,
					     1, sony_walk_callback, NULL, NULL);
		if (ACPI_FAILURE(status)) {
			printk(KERN_WARNING DRV_PFX "unable to walk acpi resources\n");
			result = -ENODEV;
			goto outwalk;
		}
	}

	if (ACPI_SUCCESS(acpi_get_handle(sony_nc_acpi_handle, "ECON",
					 &handle))) {
		if (acpi_callsetfunc(sony_nc_acpi_handle, "ECON", 1, NULL))
			dprintk("ECON Method failed\n");
	}

	if (ACPI_SUCCESS(acpi_get_handle(sony_nc_acpi_handle, "SN00",
					 &handle))) {
		dprintk("Doing SNC setup\n");
		sony_nc_function_setup(device);
		sony_nc_rfkill_setup(device);
	}

	/* setup input devices and helper fifo */
	result = sony_laptop_setup_input(device);
	if (result) {
		printk(KERN_ERR DRV_PFX
				"Unabe to create input devices.\n");
		goto outwalk;
	}

	status = acpi_install_notify_handler(sony_nc_acpi_handle,
					     ACPI_DEVICE_NOTIFY,
					     sony_acpi_notify, NULL);
	if (ACPI_FAILURE(status)) {
		printk(KERN_WARNING DRV_PFX "unable to install notify handler (%u)\n", status);
		result = -ENODEV;
		goto outinput;
	}

	if (acpi_video_backlight_support()) {
		printk(KERN_INFO DRV_PFX "brightness ignored, must be "
		       "controlled by ACPI video driver\n");
	} else if (ACPI_SUCCESS(acpi_get_handle(sony_nc_acpi_handle, "GBRT",
						&handle))) {
		sony_backlight_device = backlight_device_register("sony", NULL,
								  NULL,
								  &sony_backlight_ops);

		if (IS_ERR(sony_backlight_device)) {
			printk(KERN_WARNING DRV_PFX "unable to register backlight device\n");
			sony_backlight_device = NULL;
		} else {
			sony_backlight_device->props.brightness =
			    sony_backlight_get_brightness
			    (sony_backlight_device);
			sony_backlight_device->props.max_brightness =
			    SONY_MAX_BRIGHTNESS - 1;
		}

	}

	result = sony_pf_add();
	if (result)
		goto outbacklight;

	/* create sony_pf sysfs attributes related to the SNC device */
	for (item = sony_nc_values; item->name; ++item) {

		if (!debug && item->debug)
			continue;

		/* find the available acpiget as described in the DSDT */
		for (; item->acpiget && *item->acpiget; ++item->acpiget) {
			if (ACPI_SUCCESS(acpi_get_handle(sony_nc_acpi_handle,
							 *item->acpiget,
							 &handle))) {
				dprintk("Found %s getter: %s\n",
						item->name, *item->acpiget);
				item->devattr.attr.mode |= S_IRUGO;
				break;
			}
		}

		/* find the available acpiset as described in the DSDT */
		for (; item->acpiset && *item->acpiset; ++item->acpiset) {
			if (ACPI_SUCCESS(acpi_get_handle(sony_nc_acpi_handle,
							 *item->acpiset,
							 &handle))) {
				dprintk("Found %s setter: %s\n",
						item->name, *item->acpiset);
				item->devattr.attr.mode |= S_IWUSR;
				break;
			}
		}

		if (item->devattr.attr.mode != 0) {
			result =
			    device_create_file(&sony_pf_device->dev,
					       &item->devattr);
			if (result)
				goto out_sysfs;
		}
	}

	return 0;

      out_sysfs:
	for (item = sony_nc_values; item->name; ++item) {
		device_remove_file(&sony_pf_device->dev, &item->devattr);
	}
	sony_pf_remove();

      outbacklight:
	if (sony_backlight_device)
		backlight_device_unregister(sony_backlight_device);

	status = acpi_remove_notify_handler(sony_nc_acpi_handle,
					    ACPI_DEVICE_NOTIFY,
					    sony_acpi_notify);
	if (ACPI_FAILURE(status))
		printk(KERN_WARNING DRV_PFX "unable to remove notify handler\n");

      outinput:
	sony_laptop_remove_input();

      outwalk:
	sony_nc_rfkill_cleanup();
	return result;
}

static int sony_nc_remove(struct acpi_device *device, int type)
{
	acpi_status status;
	struct sony_nc_value *item;

	if (sony_backlight_device)
		backlight_device_unregister(sony_backlight_device);

	sony_nc_acpi_device = NULL;

	status = acpi_remove_notify_handler(sony_nc_acpi_handle,
					    ACPI_DEVICE_NOTIFY,
					    sony_acpi_notify);
	if (ACPI_FAILURE(status))
		printk(KERN_WARNING DRV_PFX "unable to remove notify handler\n");

	for (item = sony_nc_values; item->name; ++item) {
		device_remove_file(&sony_pf_device->dev, &item->devattr);
	}

	sony_pf_remove();
	sony_laptop_remove_input();
	sony_nc_rfkill_cleanup();
	dprintk(SONY_NC_DRIVER_NAME " removed.\n");

	return 0;
}

static const struct acpi_device_id sony_device_ids[] = {
	{SONY_NC_HID, 0},
	{SONY_PIC_HID, 0},
	{"", 0},
};
MODULE_DEVICE_TABLE(acpi, sony_device_ids);

static const struct acpi_device_id sony_nc_device_ids[] = {
	{SONY_NC_HID, 0},
	{"", 0},
};

static struct acpi_driver sony_nc_driver = {
	.name = SONY_NC_DRIVER_NAME,
	.class = SONY_NC_CLASS,
	.ids = sony_nc_device_ids,
	.owner = THIS_MODULE,
	.ops = {
		.add = sony_nc_add,
		.remove = sony_nc_remove,
		.resume = sony_nc_resume,
		},
};

/*********** SPIC (SNY6001) Device ***********/

#define SONYPI_DEVICE_TYPE1	0x00000001
#define SONYPI_DEVICE_TYPE2	0x00000002
#define SONYPI_DEVICE_TYPE3	0x00000004
#define SONYPI_DEVICE_TYPE4	0x00000008

#define SONYPI_TYPE1_OFFSET	0x04
#define SONYPI_TYPE2_OFFSET	0x12
#define SONYPI_TYPE3_OFFSET	0x12

struct sony_pic_ioport {
	struct acpi_resource_io	io1;
	struct acpi_resource_io	io2;
	struct list_head	list;
};

struct sony_pic_irq {
	struct acpi_resource_irq	irq;
	struct list_head		list;
};

struct sonypi_eventtypes {
	u8			data;
	unsigned long		mask;
	struct sonypi_event	*events;
};

struct sony_pic_dev {
	struct acpi_device		*acpi_dev;
	struct sony_pic_irq		*cur_irq;
	struct sony_pic_ioport		*cur_ioport;
	struct list_head		interrupts;
	struct list_head		ioports;
	struct mutex			lock;
	struct sonypi_eventtypes	*event_types;
	int                             (*handle_irq)(const u8, const u8);
	int				model;
	u16				evport_offset;
	u8				camera_power;
	u8				bluetooth_power;
	u8				wwan_power;
};

static struct sony_pic_dev spic_dev = {
	.interrupts	= LIST_HEAD_INIT(spic_dev.interrupts),
	.ioports	= LIST_HEAD_INIT(spic_dev.ioports),
};

static int spic_drv_registered;

/* Event masks */
#define SONYPI_JOGGER_MASK			0x00000001
#define SONYPI_CAPTURE_MASK			0x00000002
#define SONYPI_FNKEY_MASK			0x00000004
#define SONYPI_BLUETOOTH_MASK			0x00000008
#define SONYPI_PKEY_MASK			0x00000010
#define SONYPI_BACK_MASK			0x00000020
#define SONYPI_HELP_MASK			0x00000040
#define SONYPI_LID_MASK				0x00000080
#define SONYPI_ZOOM_MASK			0x00000100
#define SONYPI_THUMBPHRASE_MASK			0x00000200
#define SONYPI_MEYE_MASK			0x00000400
#define SONYPI_MEMORYSTICK_MASK			0x00000800
#define SONYPI_BATTERY_MASK			0x00001000
#define SONYPI_WIRELESS_MASK			0x00002000

struct sonypi_event {
	u8	data;
	u8	event;
};

/* The set of possible button release events */
static struct sonypi_event sonypi_releaseev[] = {
	{ 0x00, SONYPI_EVENT_ANYBUTTON_RELEASED },
	{ 0, 0 }
};

/* The set of possible jogger events  */
static struct sonypi_event sonypi_joggerev[] = {
	{ 0x1f, SONYPI_EVENT_JOGDIAL_UP },
	{ 0x01, SONYPI_EVENT_JOGDIAL_DOWN },
	{ 0x5f, SONYPI_EVENT_JOGDIAL_UP_PRESSED },
	{ 0x41, SONYPI_EVENT_JOGDIAL_DOWN_PRESSED },
	{ 0x1e, SONYPI_EVENT_JOGDIAL_FAST_UP },
	{ 0x02, SONYPI_EVENT_JOGDIAL_FAST_DOWN },
	{ 0x5e, SONYPI_EVENT_JOGDIAL_FAST_UP_PRESSED },
	{ 0x42, SONYPI_EVENT_JOGDIAL_FAST_DOWN_PRESSED },
	{ 0x1d, SONYPI_EVENT_JOGDIAL_VFAST_UP },
	{ 0x03, SONYPI_EVENT_JOGDIAL_VFAST_DOWN },
	{ 0x5d, SONYPI_EVENT_JOGDIAL_VFAST_UP_PRESSED },
	{ 0x43, SONYPI_EVENT_JOGDIAL_VFAST_DOWN_PRESSED },
	{ 0x40, SONYPI_EVENT_JOGDIAL_PRESSED },
	{ 0, 0 }
};

/* The set of possible capture button events */
static struct sonypi_event sonypi_captureev[] = {
	{ 0x05, SONYPI_EVENT_CAPTURE_PARTIALPRESSED },
	{ 0x07, SONYPI_EVENT_CAPTURE_PRESSED },
	{ 0x40, SONYPI_EVENT_CAPTURE_PRESSED },
	{ 0x01, SONYPI_EVENT_CAPTURE_PARTIALRELEASED },
	{ 0, 0 }
};

/* The set of possible fnkeys events */
static struct sonypi_event sonypi_fnkeyev[] = {
	{ 0x10, SONYPI_EVENT_FNKEY_ESC },
	{ 0x11, SONYPI_EVENT_FNKEY_F1 },
	{ 0x12, SONYPI_EVENT_FNKEY_F2 },
	{ 0x13, SONYPI_EVENT_FNKEY_F3 },
	{ 0x14, SONYPI_EVENT_FNKEY_F4 },
	{ 0x15, SONYPI_EVENT_FNKEY_F5 },
	{ 0x16, SONYPI_EVENT_FNKEY_F6 },
	{ 0x17, SONYPI_EVENT_FNKEY_F7 },
	{ 0x18, SONYPI_EVENT_FNKEY_F8 },
	{ 0x19, SONYPI_EVENT_FNKEY_F9 },
	{ 0x1a, SONYPI_EVENT_FNKEY_F10 },
	{ 0x1b, SONYPI_EVENT_FNKEY_F11 },
	{ 0x1c, SONYPI_EVENT_FNKEY_F12 },
	{ 0x1f, SONYPI_EVENT_FNKEY_RELEASED },
	{ 0x21, SONYPI_EVENT_FNKEY_1 },
	{ 0x22, SONYPI_EVENT_FNKEY_2 },
	{ 0x31, SONYPI_EVENT_FNKEY_D },
	{ 0x32, SONYPI_EVENT_FNKEY_E },
	{ 0x33, SONYPI_EVENT_FNKEY_F },
	{ 0x34, SONYPI_EVENT_FNKEY_S },
	{ 0x35, SONYPI_EVENT_FNKEY_B },
	{ 0x36, SONYPI_EVENT_FNKEY_ONLY },
	{ 0, 0 }
};

/* The set of possible program key events */
static struct sonypi_event sonypi_pkeyev[] = {
	{ 0x01, SONYPI_EVENT_PKEY_P1 },
	{ 0x02, SONYPI_EVENT_PKEY_P2 },
	{ 0x04, SONYPI_EVENT_PKEY_P3 },
	{ 0x20, SONYPI_EVENT_PKEY_P1 },
	{ 0, 0 }
};

/* The set of possible bluetooth events */
static struct sonypi_event sonypi_blueev[] = {
	{ 0x55, SONYPI_EVENT_BLUETOOTH_PRESSED },
	{ 0x59, SONYPI_EVENT_BLUETOOTH_ON },
	{ 0x5a, SONYPI_EVENT_BLUETOOTH_OFF },
	{ 0, 0 }
};

/* The set of possible wireless events */
static struct sonypi_event sonypi_wlessev[] = {
	{ 0x59, SONYPI_EVENT_WIRELESS_ON },
	{ 0x5a, SONYPI_EVENT_WIRELESS_OFF },
	{ 0, 0 }
};

/* The set of possible back button events */
static struct sonypi_event sonypi_backev[] = {
	{ 0x20, SONYPI_EVENT_BACK_PRESSED },
	{ 0, 0 }
};

/* The set of possible help button events */
static struct sonypi_event sonypi_helpev[] = {
	{ 0x3b, SONYPI_EVENT_HELP_PRESSED },
	{ 0, 0 }
};


/* The set of possible lid events */
static struct sonypi_event sonypi_lidev[] = {
	{ 0x51, SONYPI_EVENT_LID_CLOSED },
	{ 0x50, SONYPI_EVENT_LID_OPENED },
	{ 0, 0 }
};

/* The set of possible zoom events */
static struct sonypi_event sonypi_zoomev[] = {
	{ 0x39, SONYPI_EVENT_ZOOM_PRESSED },
	{ 0x10, SONYPI_EVENT_ZOOM_IN_PRESSED },
	{ 0x20, SONYPI_EVENT_ZOOM_OUT_PRESSED },
	{ 0x04, SONYPI_EVENT_ZOOM_PRESSED },
	{ 0, 0 }
};

/* The set of possible thumbphrase events */
static struct sonypi_event sonypi_thumbphraseev[] = {
	{ 0x3a, SONYPI_EVENT_THUMBPHRASE_PRESSED },
	{ 0, 0 }
};

/* The set of possible motioneye camera events */
static struct sonypi_event sonypi_meyeev[] = {
	{ 0x00, SONYPI_EVENT_MEYE_FACE },
	{ 0x01, SONYPI_EVENT_MEYE_OPPOSITE },
	{ 0, 0 }
};

/* The set of possible memorystick events */
static struct sonypi_event sonypi_memorystickev[] = {
	{ 0x53, SONYPI_EVENT_MEMORYSTICK_INSERT },
	{ 0x54, SONYPI_EVENT_MEMORYSTICK_EJECT },
	{ 0, 0 }
};

/* The set of possible battery events */
static struct sonypi_event sonypi_batteryev[] = {
	{ 0x20, SONYPI_EVENT_BATTERY_INSERT },
	{ 0x30, SONYPI_EVENT_BATTERY_REMOVE },
	{ 0, 0 }
};

/* The set of possible volume events */
static struct sonypi_event sonypi_volumeev[] = {
	{ 0x01, SONYPI_EVENT_VOLUME_INC_PRESSED },
	{ 0x02, SONYPI_EVENT_VOLUME_DEC_PRESSED },
	{ 0, 0 }
};

/* The set of possible brightness events */
static struct sonypi_event sonypi_brightnessev[] = {
	{ 0x80, SONYPI_EVENT_BRIGHTNESS_PRESSED },
	{ 0, 0 }
};

static struct sonypi_eventtypes type1_events[] = {
	{ 0, 0xffffffff, sonypi_releaseev },
	{ 0x70, SONYPI_MEYE_MASK, sonypi_meyeev },
	{ 0x30, SONYPI_LID_MASK, sonypi_lidev },
	{ 0x60, SONYPI_CAPTURE_MASK, sonypi_captureev },
	{ 0x10, SONYPI_JOGGER_MASK, sonypi_joggerev },
	{ 0x20, SONYPI_FNKEY_MASK, sonypi_fnkeyev },
	{ 0x30, SONYPI_BLUETOOTH_MASK, sonypi_blueev },
	{ 0x40, SONYPI_PKEY_MASK, sonypi_pkeyev },
	{ 0x30, SONYPI_MEMORYSTICK_MASK, sonypi_memorystickev },
	{ 0x40, SONYPI_BATTERY_MASK, sonypi_batteryev },
	{ 0 },
};
static struct sonypi_eventtypes type2_events[] = {
	{ 0, 0xffffffff, sonypi_releaseev },
	{ 0x38, SONYPI_LID_MASK, sonypi_lidev },
	{ 0x11, SONYPI_JOGGER_MASK, sonypi_joggerev },
	{ 0x61, SONYPI_CAPTURE_MASK, sonypi_captureev },
	{ 0x21, SONYPI_FNKEY_MASK, sonypi_fnkeyev },
	{ 0x31, SONYPI_BLUETOOTH_MASK, sonypi_blueev },
	{ 0x08, SONYPI_PKEY_MASK, sonypi_pkeyev },
	{ 0x11, SONYPI_BACK_MASK, sonypi_backev },
	{ 0x21, SONYPI_HELP_MASK, sonypi_helpev },
	{ 0x21, SONYPI_ZOOM_MASK, sonypi_zoomev },
	{ 0x20, SONYPI_THUMBPHRASE_MASK, sonypi_thumbphraseev },
	{ 0x31, SONYPI_MEMORYSTICK_MASK, sonypi_memorystickev },
	{ 0x41, SONYPI_BATTERY_MASK, sonypi_batteryev },
	{ 0x31, SONYPI_PKEY_MASK, sonypi_pkeyev },
	{ 0 },
};
static struct sonypi_eventtypes type3_events[] = {
	{ 0, 0xffffffff, sonypi_releaseev },
	{ 0x21, SONYPI_FNKEY_MASK, sonypi_fnkeyev },
	{ 0x31, SONYPI_WIRELESS_MASK, sonypi_wlessev },
	{ 0x31, SONYPI_MEMORYSTICK_MASK, sonypi_memorystickev },
	{ 0x41, SONYPI_BATTERY_MASK, sonypi_batteryev },
	{ 0x31, SONYPI_PKEY_MASK, sonypi_pkeyev },
	{ 0x05, SONYPI_PKEY_MASK, sonypi_pkeyev },
	{ 0x05, SONYPI_ZOOM_MASK, sonypi_zoomev },
	{ 0x05, SONYPI_CAPTURE_MASK, sonypi_captureev },
	{ 0x05, SONYPI_PKEY_MASK, sonypi_volumeev },
	{ 0x05, SONYPI_PKEY_MASK, sonypi_brightnessev },
	{ 0 },
};

/* low level spic calls */
#define ITERATIONS_LONG		10000
#define ITERATIONS_SHORT	10
#define wait_on_command(command, iterations) {				\
	unsigned int n = iterations;					\
	while (--n && (command))					\
		udelay(1);						\
	if (!n)								\
		dprintk("command failed at %s : %s (line %d)\n",	\
				__FILE__, __func__, __LINE__);	\
}

static u8 sony_pic_call1(u8 dev)
{
	u8 v1, v2;

	wait_on_command(inb_p(spic_dev.cur_ioport->io1.minimum + 4) & 2,
			ITERATIONS_LONG);
	outb(dev, spic_dev.cur_ioport->io1.minimum + 4);
	v1 = inb_p(spic_dev.cur_ioport->io1.minimum + 4);
	v2 = inb_p(spic_dev.cur_ioport->io1.minimum);
	dprintk("sony_pic_call1(0x%.2x): 0x%.4x\n", dev, (v2 << 8) | v1);
	return v2;
}

static u8 sony_pic_call2(u8 dev, u8 fn)
{
	u8 v1;

	wait_on_command(inb_p(spic_dev.cur_ioport->io1.minimum + 4) & 2,
			ITERATIONS_LONG);
	outb(dev, spic_dev.cur_ioport->io1.minimum + 4);
	wait_on_command(inb_p(spic_dev.cur_ioport->io1.minimum + 4) & 2,
			ITERATIONS_LONG);
	outb(fn, spic_dev.cur_ioport->io1.minimum);
	v1 = inb_p(spic_dev.cur_ioport->io1.minimum);
	dprintk("sony_pic_call2(0x%.2x - 0x%.2x): 0x%.4x\n", dev, fn, v1);
	return v1;
}

static u8 sony_pic_call3(u8 dev, u8 fn, u8 v)
{
	u8 v1;

	wait_on_command(inb_p(spic_dev.cur_ioport->io1.minimum + 4) & 2, ITERATIONS_LONG);
	outb(dev, spic_dev.cur_ioport->io1.minimum + 4);
	wait_on_command(inb_p(spic_dev.cur_ioport->io1.minimum + 4) & 2, ITERATIONS_LONG);
	outb(fn, spic_dev.cur_ioport->io1.minimum);
	wait_on_command(inb_p(spic_dev.cur_ioport->io1.minimum + 4) & 2, ITERATIONS_LONG);
	outb(v, spic_dev.cur_ioport->io1.minimum);
	v1 = inb_p(spic_dev.cur_ioport->io1.minimum);
	dprintk("sony_pic_call3(0x%.2x - 0x%.2x - 0x%.2x): 0x%.4x\n",
			dev, fn, v, v1);
	return v1;
}

/*
 * minidrivers for SPIC models
 */
static int type3_handle_irq(const u8 data_mask, const u8 ev)
{
	/*
	 * 0x31 could mean we have to take some extra action and wait for
	 * the next irq for some Type3 models, it will generate a new
	 * irq and we can read new data from the device:
	 *  - 0x5c and 0x5f requires 0xA0
	 *  - 0x61 requires 0xB3
	 */
	if (data_mask == 0x31) {
		if (ev == 0x5c || ev == 0x5f)
			sony_pic_call1(0xA0);
		else if (ev == 0x61)
			sony_pic_call1(0xB3);
		return 0;
	}
	return 1;
}

static void sony_pic_detect_device_type(struct sony_pic_dev *dev)
{
	struct pci_dev *pcidev;

	pcidev = pci_get_device(PCI_VENDOR_ID_INTEL,
			PCI_DEVICE_ID_INTEL_82371AB_3, NULL);
	if (pcidev) {
		dev->model = SONYPI_DEVICE_TYPE1;
		dev->evport_offset = SONYPI_TYPE1_OFFSET;
		dev->event_types = type1_events;
		goto out;
	}

	pcidev = pci_get_device(PCI_VENDOR_ID_INTEL,
			PCI_DEVICE_ID_INTEL_ICH6_1, NULL);
	if (pcidev) {
		dev->model = SONYPI_DEVICE_TYPE2;
		dev->evport_offset = SONYPI_TYPE2_OFFSET;
		dev->event_types = type2_events;
		goto out;
	}

	pcidev = pci_get_device(PCI_VENDOR_ID_INTEL,
			PCI_DEVICE_ID_INTEL_ICH7_1, NULL);
	if (pcidev) {
		dev->model = SONYPI_DEVICE_TYPE3;
		dev->handle_irq = type3_handle_irq;
		dev->evport_offset = SONYPI_TYPE3_OFFSET;
		dev->event_types = type3_events;
		goto out;
	}

	pcidev = pci_get_device(PCI_VENDOR_ID_INTEL,
			PCI_DEVICE_ID_INTEL_ICH8_4, NULL);
	if (pcidev) {
		dev->model = SONYPI_DEVICE_TYPE3;
		dev->handle_irq = type3_handle_irq;
		dev->evport_offset = SONYPI_TYPE3_OFFSET;
		dev->event_types = type3_events;
		goto out;
	}

	pcidev = pci_get_device(PCI_VENDOR_ID_INTEL,
			PCI_DEVICE_ID_INTEL_ICH9_1, NULL);
	if (pcidev) {
		dev->model = SONYPI_DEVICE_TYPE3;
		dev->handle_irq = type3_handle_irq;
		dev->evport_offset = SONYPI_TYPE3_OFFSET;
		dev->event_types = type3_events;
		goto out;
	}

	/* default */
	dev->model = SONYPI_DEVICE_TYPE2;
	dev->evport_offset = SONYPI_TYPE2_OFFSET;
	dev->event_types = type2_events;

out:
	if (pcidev)
		pci_dev_put(pcidev);

	printk(KERN_INFO DRV_PFX "detected Type%d model\n",
			dev->model == SONYPI_DEVICE_TYPE1 ? 1 :
			dev->model == SONYPI_DEVICE_TYPE2 ? 2 : 3);
}

/* camera tests and poweron/poweroff */
#define SONYPI_CAMERA_PICTURE		5
#define SONYPI_CAMERA_CONTROL		0x10

#define SONYPI_CAMERA_BRIGHTNESS		0
#define SONYPI_CAMERA_CONTRAST			1
#define SONYPI_CAMERA_HUE			2
#define SONYPI_CAMERA_COLOR			3
#define SONYPI_CAMERA_SHARPNESS			4

#define SONYPI_CAMERA_EXPOSURE_MASK		0xC
#define SONYPI_CAMERA_WHITE_BALANCE_MASK	0x3
#define SONYPI_CAMERA_PICTURE_MODE_MASK		0x30
#define SONYPI_CAMERA_MUTE_MASK			0x40

/* the rest don't need a loop until not 0xff */
#define SONYPI_CAMERA_AGC			6
#define SONYPI_CAMERA_AGC_MASK			0x30
#define SONYPI_CAMERA_SHUTTER_MASK 		0x7

#define SONYPI_CAMERA_SHUTDOWN_REQUEST		7
#define SONYPI_CAMERA_CONTROL			0x10

#define SONYPI_CAMERA_STATUS 			7
#define SONYPI_CAMERA_STATUS_READY 		0x2
#define SONYPI_CAMERA_STATUS_POSITION		0x4

#define SONYPI_DIRECTION_BACKWARDS 		0x4

#define SONYPI_CAMERA_REVISION 			8
#define SONYPI_CAMERA_ROMVERSION 		9

static int __sony_pic_camera_ready(void)
{
	u8 v;

	v = sony_pic_call2(0x8f, SONYPI_CAMERA_STATUS);
	return (v != 0xff && (v & SONYPI_CAMERA_STATUS_READY));
}

static int __sony_pic_camera_off(void)
{
	if (!camera) {
		printk(KERN_WARNING DRV_PFX "camera control not enabled\n");
		return -ENODEV;
	}

	wait_on_command(sony_pic_call3(0x90, SONYPI_CAMERA_PICTURE,
				SONYPI_CAMERA_MUTE_MASK),
			ITERATIONS_SHORT);

	if (spic_dev.camera_power) {
		sony_pic_call2(0x91, 0);
		spic_dev.camera_power = 0;
	}
	return 0;
}

static int __sony_pic_camera_on(void)
{
	int i, j, x;

	if (!camera) {
		printk(KERN_WARNING DRV_PFX "camera control not enabled\n");
		return -ENODEV;
	}

	if (spic_dev.camera_power)
		return 0;

	for (j = 5; j > 0; j--) {

		for (x = 0; x < 100 && sony_pic_call2(0x91, 0x1); x++)
			msleep(10);
		sony_pic_call1(0x93);

		for (i = 400; i > 0; i--) {
			if (__sony_pic_camera_ready())
				break;
			msleep(10);
		}
		if (i)
			break;
	}

	if (j == 0) {
		printk(KERN_WARNING DRV_PFX "failed to power on camera\n");
		return -ENODEV;
	}

	wait_on_command(sony_pic_call3(0x90, SONYPI_CAMERA_CONTROL,
				0x5a),
			ITERATIONS_SHORT);

	spic_dev.camera_power = 1;
	return 0;
}

/* External camera command (exported to the motion eye v4l driver) */
int sony_pic_camera_command(int command, u8 value)
{
	if (!camera)
		return -EIO;

	mutex_lock(&spic_dev.lock);

	switch (command) {
	case SONY_PIC_COMMAND_SETCAMERA:
		if (value)
			__sony_pic_camera_on();
		else
			__sony_pic_camera_off();
		break;
	case SONY_PIC_COMMAND_SETCAMERABRIGHTNESS:
		wait_on_command(sony_pic_call3(0x90, SONYPI_CAMERA_BRIGHTNESS, value),
				ITERATIONS_SHORT);
		break;
	case SONY_PIC_COMMAND_SETCAMERACONTRAST:
		wait_on_command(sony_pic_call3(0x90, SONYPI_CAMERA_CONTRAST, value),
				ITERATIONS_SHORT);
		break;
	case SONY_PIC_COMMAND_SETCAMERAHUE:
		wait_on_command(sony_pic_call3(0x90, SONYPI_CAMERA_HUE, value),
				ITERATIONS_SHORT);
		break;
	case SONY_PIC_COMMAND_SETCAMERACOLOR:
		wait_on_command(sony_pic_call3(0x90, SONYPI_CAMERA_COLOR, value),
				ITERATIONS_SHORT);
		break;
	case SONY_PIC_COMMAND_SETCAMERASHARPNESS:
		wait_on_command(sony_pic_call3(0x90, SONYPI_CAMERA_SHARPNESS, value),
				ITERATIONS_SHORT);
		break;
	case SONY_PIC_COMMAND_SETCAMERAPICTURE:
		wait_on_command(sony_pic_call3(0x90, SONYPI_CAMERA_PICTURE, value),
				ITERATIONS_SHORT);
		break;
	case SONY_PIC_COMMAND_SETCAMERAAGC:
		wait_on_command(sony_pic_call3(0x90, SONYPI_CAMERA_AGC, value),
				ITERATIONS_SHORT);
		break;
	default:
		printk(KERN_ERR DRV_PFX "sony_pic_camera_command invalid: %d\n",
		       command);
		break;
	}
	mutex_unlock(&spic_dev.lock);
	return 0;
}
EXPORT_SYMBOL(sony_pic_camera_command);

/* gprs/edge modem (SZ460N and SZ210P), thanks to Joshua Wise */
static void __sony_pic_set_wwanpower(u8 state)
{
	state = !!state;
	if (spic_dev.wwan_power == state)
		return;
	sony_pic_call2(0xB0, state);
	sony_pic_call1(0x82);
	spic_dev.wwan_power = state;
}

static ssize_t sony_pic_wwanpower_store(struct device *dev,
		struct device_attribute *attr,
		const char *buffer, size_t count)
{
	unsigned long value;
	if (count > 31)
		return -EINVAL;

	value = simple_strtoul(buffer, NULL, 10);
	mutex_lock(&spic_dev.lock);
	__sony_pic_set_wwanpower(value);
	mutex_unlock(&spic_dev.lock);

	return count;
}

static ssize_t sony_pic_wwanpower_show(struct device *dev,
		struct device_attribute *attr, char *buffer)
{
	ssize_t count;
	mutex_lock(&spic_dev.lock);
	count = snprintf(buffer, PAGE_SIZE, "%d\n", spic_dev.wwan_power);
	mutex_unlock(&spic_dev.lock);
	return count;
}

/* bluetooth subsystem power state */
static void __sony_pic_set_bluetoothpower(u8 state)
{
	state = !!state;
	if (spic_dev.bluetooth_power == state)
		return;
	sony_pic_call2(0x96, state);
	sony_pic_call1(0x82);
	spic_dev.bluetooth_power = state;
}

static ssize_t sony_pic_bluetoothpower_store(struct device *dev,
		struct device_attribute *attr,
		const char *buffer, size_t count)
{
	unsigned long value;
	if (count > 31)
		return -EINVAL;

	value = simple_strtoul(buffer, NULL, 10);
	mutex_lock(&spic_dev.lock);
	__sony_pic_set_bluetoothpower(value);
	mutex_unlock(&spic_dev.lock);

	return count;
}

static ssize_t sony_pic_bluetoothpower_show(struct device *dev,
		struct device_attribute *attr, char *buffer)
{
	ssize_t count = 0;
	mutex_lock(&spic_dev.lock);
	count = snprintf(buffer, PAGE_SIZE, "%d\n", spic_dev.bluetooth_power);
	mutex_unlock(&spic_dev.lock);
	return count;
}

/* fan speed */
/* FAN0 information (reverse engineered from ACPI tables) */
#define SONY_PIC_FAN0_STATUS	0x93
static int sony_pic_set_fanspeed(unsigned long value)
{
	return ec_write(SONY_PIC_FAN0_STATUS, value);
}

static int sony_pic_get_fanspeed(u8 *value)
{
	return ec_read(SONY_PIC_FAN0_STATUS, value);
}

static ssize_t sony_pic_fanspeed_store(struct device *dev,
		struct device_attribute *attr,
		const char *buffer, size_t count)
{
	unsigned long value;
	if (count > 31)
		return -EINVAL;

	value = simple_strtoul(buffer, NULL, 10);
	if (sony_pic_set_fanspeed(value))
		return -EIO;

	return count;
}

static ssize_t sony_pic_fanspeed_show(struct device *dev,
		struct device_attribute *attr, char *buffer)
{
	u8 value = 0;
	if (sony_pic_get_fanspeed(&value))
		return -EIO;

	return snprintf(buffer, PAGE_SIZE, "%d\n", value);
}

#define SPIC_ATTR(_name, _mode)					\
struct device_attribute spic_attr_##_name = __ATTR(_name,	\
		_mode, sony_pic_## _name ##_show,		\
		sony_pic_## _name ##_store)

static SPIC_ATTR(bluetoothpower, 0644);
static SPIC_ATTR(wwanpower, 0644);
static SPIC_ATTR(fanspeed, 0644);

static struct attribute *spic_attributes[] = {
	&spic_attr_bluetoothpower.attr,
	&spic_attr_wwanpower.attr,
	&spic_attr_fanspeed.attr,
	NULL
};

static struct attribute_group spic_attribute_group = {
	.attrs = spic_attributes
};

/******** SONYPI compatibility **********/
#ifdef CONFIG_SONYPI_COMPAT

/* battery / brightness / temperature  addresses */
#define SONYPI_BAT_FLAGS	0x81
#define SONYPI_LCD_LIGHT	0x96
#define SONYPI_BAT1_PCTRM	0xa0
#define SONYPI_BAT1_LEFT	0xa2
#define SONYPI_BAT1_MAXRT	0xa4
#define SONYPI_BAT2_PCTRM	0xa8
#define SONYPI_BAT2_LEFT	0xaa
#define SONYPI_BAT2_MAXRT	0xac
#define SONYPI_BAT1_MAXTK	0xb0
#define SONYPI_BAT1_FULL	0xb2
#define SONYPI_BAT2_MAXTK	0xb8
#define SONYPI_BAT2_FULL	0xba
#define SONYPI_TEMP_STATUS	0xC1

struct sonypi_compat_s {
	struct fasync_struct	*fifo_async;
	struct kfifo		*fifo;
	spinlock_t		fifo_lock;
	wait_queue_head_t	fifo_proc_list;
	atomic_t		open_count;
};
static struct sonypi_compat_s sonypi_compat = {
	.open_count = ATOMIC_INIT(0),
};

static int sonypi_misc_fasync(int fd, struct file *filp, int on)
{
	int retval;

	retval = fasync_helper(fd, filp, on, &sonypi_compat.fifo_async);
	if (retval < 0)
		return retval;
	return 0;
}

static int sonypi_misc_release(struct inode *inode, struct file *file)
{
	atomic_dec(&sonypi_compat.open_count);
	return 0;
}

static int sonypi_misc_open(struct inode *inode, struct file *file)
{
	/* Flush input queue on first open */
	unsigned long flags;

	spin_lock_irqsave(sonypi_compat.fifo->lock, flags);

	if (atomic_inc_return(&sonypi_compat.open_count) == 1)
		__kfifo_reset(sonypi_compat.fifo);

	spin_unlock_irqrestore(sonypi_compat.fifo->lock, flags);

	return 0;
}

static ssize_t sonypi_misc_read(struct file *file, char __user *buf,
				size_t count, loff_t *pos)
{
	ssize_t ret;
	unsigned char c;

	if ((kfifo_len(sonypi_compat.fifo) == 0) &&
	    (file->f_flags & O_NONBLOCK))
		return -EAGAIN;

	ret = wait_event_interruptible(sonypi_compat.fifo_proc_list,
				       kfifo_len(sonypi_compat.fifo) != 0);
	if (ret)
		return ret;

	while (ret < count &&
	       (kfifo_get(sonypi_compat.fifo, &c, sizeof(c)) == sizeof(c))) {
		if (put_user(c, buf++))
			return -EFAULT;
		ret++;
	}

	if (ret > 0) {
		struct inode *inode = file->f_path.dentry->d_inode;
		inode->i_atime = current_fs_time(inode->i_sb);
	}

	return ret;
}

static unsigned int sonypi_misc_poll(struct file *file, poll_table *wait)
{
	poll_wait(file, &sonypi_compat.fifo_proc_list, wait);
	if (kfifo_len(sonypi_compat.fifo))
		return POLLIN | POLLRDNORM;
	return 0;
}

static int ec_read16(u8 addr, u16 *value)
{
	u8 val_lb, val_hb;
	if (ec_read(addr, &val_lb))
		return -1;
	if (ec_read(addr + 1, &val_hb))
		return -1;
	*value = val_lb | (val_hb << 8);
	return 0;
}

static long sonypi_misc_ioctl(struct file *fp, unsigned int cmd,
							unsigned long arg)
{
	int ret = 0;
	void __user *argp = (void __user *)arg;
	u8 val8;
	u16 val16;
	int value;

	mutex_lock(&spic_dev.lock);
	switch (cmd) {
	case SONYPI_IOCGBRT:
		if (sony_backlight_device == NULL) {
			ret = -EIO;
			break;
		}
		if (acpi_callgetfunc(sony_nc_acpi_handle, "GBRT", &value)) {
			ret = -EIO;
			break;
		}
		val8 = ((value & 0xff) - 1) << 5;
		if (copy_to_user(argp, &val8, sizeof(val8)))
				ret = -EFAULT;
		break;
	case SONYPI_IOCSBRT:
		if (sony_backlight_device == NULL) {
			ret = -EIO;
			break;
		}
		if (copy_from_user(&val8, argp, sizeof(val8))) {
			ret = -EFAULT;
			break;
		}
		if (acpi_callsetfunc(sony_nc_acpi_handle, "SBRT",
				(val8 >> 5) + 1, NULL)) {
			ret = -EIO;
			break;
		}
		/* sync the backlight device status */
		sony_backlight_device->props.brightness =
		    sony_backlight_get_brightness(sony_backlight_device);
		break;
	case SONYPI_IOCGBAT1CAP:
		if (ec_read16(SONYPI_BAT1_FULL, &val16)) {
			ret = -EIO;
			break;
		}
		if (copy_to_user(argp, &val16, sizeof(val16)))
			ret = -EFAULT;
		break;
	case SONYPI_IOCGBAT1REM:
		if (ec_read16(SONYPI_BAT1_LEFT, &val16)) {
			ret = -EIO;
			break;
		}
		if (copy_to_user(argp, &val16, sizeof(val16)))
			ret = -EFAULT;
		break;
	case SONYPI_IOCGBAT2CAP:
		if (ec_read16(SONYPI_BAT2_FULL, &val16)) {
			ret = -EIO;
			break;
		}
		if (copy_to_user(argp, &val16, sizeof(val16)))
			ret = -EFAULT;
		break;
	case SONYPI_IOCGBAT2REM:
		if (ec_read16(SONYPI_BAT2_LEFT, &val16)) {
			ret = -EIO;
			break;
		}
		if (copy_to_user(argp, &val16, sizeof(val16)))
			ret = -EFAULT;
		break;
	case SONYPI_IOCGBATFLAGS:
		if (ec_read(SONYPI_BAT_FLAGS, &val8)) {
			ret = -EIO;
			break;
		}
		val8 &= 0x07;
		if (copy_to_user(argp, &val8, sizeof(val8)))
			ret = -EFAULT;
		break;
	case SONYPI_IOCGBLUE:
		val8 = spic_dev.bluetooth_power;
		if (copy_to_user(argp, &val8, sizeof(val8)))
			ret = -EFAULT;
		break;
	case SONYPI_IOCSBLUE:
		if (copy_from_user(&val8, argp, sizeof(val8))) {
			ret = -EFAULT;
			break;
		}
		__sony_pic_set_bluetoothpower(val8);
		break;
	/* FAN Controls */
	case SONYPI_IOCGFAN:
		if (sony_pic_get_fanspeed(&val8)) {
			ret = -EIO;
			break;
		}
		if (copy_to_user(argp, &val8, sizeof(val8)))
			ret = -EFAULT;
		break;
	case SONYPI_IOCSFAN:
		if (copy_from_user(&val8, argp, sizeof(val8))) {
			ret = -EFAULT;
			break;
		}
		if (sony_pic_set_fanspeed(val8))
			ret = -EIO;
		break;
	/* GET Temperature (useful under APM) */
	case SONYPI_IOCGTEMP:
		if (ec_read(SONYPI_TEMP_STATUS, &val8)) {
			ret = -EIO;
			break;
		}
		if (copy_to_user(argp, &val8, sizeof(val8)))
			ret = -EFAULT;
		break;
	default:
		ret = -EINVAL;
	}
	mutex_unlock(&spic_dev.lock);
	return ret;
}

static const struct file_operations sonypi_misc_fops = {
	.owner		= THIS_MODULE,
	.read		= sonypi_misc_read,
	.poll		= sonypi_misc_poll,
	.open		= sonypi_misc_open,
	.release	= sonypi_misc_release,
	.fasync		= sonypi_misc_fasync,
	.unlocked_ioctl	= sonypi_misc_ioctl,
};

static struct miscdevice sonypi_misc_device = {
	.minor		= MISC_DYNAMIC_MINOR,
	.name		= "sonypi",
	.fops		= &sonypi_misc_fops,
};

static void sonypi_compat_report_event(u8 event)
{
	kfifo_put(sonypi_compat.fifo, (unsigned char *)&event, sizeof(event));
	kill_fasync(&sonypi_compat.fifo_async, SIGIO, POLL_IN);
	wake_up_interruptible(&sonypi_compat.fifo_proc_list);
}

static int sonypi_compat_init(void)
{
	int error;

	spin_lock_init(&sonypi_compat.fifo_lock);
	sonypi_compat.fifo = kfifo_alloc(SONY_LAPTOP_BUF_SIZE, GFP_KERNEL,
					 &sonypi_compat.fifo_lock);
	if (IS_ERR(sonypi_compat.fifo)) {
		printk(KERN_ERR DRV_PFX "kfifo_alloc failed\n");
		return PTR_ERR(sonypi_compat.fifo);
	}

	init_waitqueue_head(&sonypi_compat.fifo_proc_list);

	if (minor != -1)
		sonypi_misc_device.minor = minor;
	error = misc_register(&sonypi_misc_device);
	if (error) {
		printk(KERN_ERR DRV_PFX "misc_register failed\n");
		goto err_free_kfifo;
	}
	if (minor == -1)
		printk(KERN_INFO DRV_PFX "device allocated minor is %d\n",
		       sonypi_misc_device.minor);

	return 0;

err_free_kfifo:
	kfifo_free(sonypi_compat.fifo);
	return error;
}

static void sonypi_compat_exit(void)
{
	misc_deregister(&sonypi_misc_device);
	kfifo_free(sonypi_compat.fifo);
}
#else
static int sonypi_compat_init(void) { return 0; }
static void sonypi_compat_exit(void) { }
static void sonypi_compat_report_event(u8 event) { }
#endif /* CONFIG_SONYPI_COMPAT */

/*
 * ACPI callbacks
 */
static acpi_status
sony_pic_read_possible_resource(struct acpi_resource *resource, void *context)
{
	u32 i;
	struct sony_pic_dev *dev = (struct sony_pic_dev *)context;

	switch (resource->type) {
	case ACPI_RESOURCE_TYPE_START_DEPENDENT:
		{
			/* start IO enumeration */
			struct sony_pic_ioport *ioport = kzalloc(sizeof(*ioport), GFP_KERNEL);
			if (!ioport)
				return AE_ERROR;

			list_add(&ioport->list, &dev->ioports);
			return AE_OK;
		}

	case ACPI_RESOURCE_TYPE_END_DEPENDENT:
		/* end IO enumeration */
		return AE_OK;

	case ACPI_RESOURCE_TYPE_IRQ:
		{
			struct acpi_resource_irq *p = &resource->data.irq;
			struct sony_pic_irq *interrupt = NULL;
			if (!p || !p->interrupt_count) {
				/*
				 * IRQ descriptors may have no IRQ# bits set,
				 * particularly those those w/ _STA disabled
				 */
				dprintk("Blank IRQ resource\n");
				return AE_OK;
			}
			for (i = 0; i < p->interrupt_count; i++) {
				if (!p->interrupts[i]) {
					printk(KERN_WARNING DRV_PFX
							"Invalid IRQ %d\n",
							p->interrupts[i]);
					continue;
				}
				interrupt = kzalloc(sizeof(*interrupt),
						GFP_KERNEL);
				if (!interrupt)
					return AE_ERROR;

				list_add(&interrupt->list, &dev->interrupts);
				interrupt->irq.triggering = p->triggering;
				interrupt->irq.polarity = p->polarity;
				interrupt->irq.sharable = p->sharable;
				interrupt->irq.interrupt_count = 1;
				interrupt->irq.interrupts[0] = p->interrupts[i];
			}
			return AE_OK;
		}
	case ACPI_RESOURCE_TYPE_IO:
		{
			struct acpi_resource_io *io = &resource->data.io;
			struct sony_pic_ioport *ioport =
				list_first_entry(&dev->ioports, struct sony_pic_ioport, list);
			if (!io) {
				dprintk("Blank IO resource\n");
				return AE_OK;
			}

			if (!ioport->io1.minimum) {
				memcpy(&ioport->io1, io, sizeof(*io));
				dprintk("IO1 at 0x%.4x (0x%.2x)\n", ioport->io1.minimum,
						ioport->io1.address_length);
			}
			else if (!ioport->io2.minimum) {
				memcpy(&ioport->io2, io, sizeof(*io));
				dprintk("IO2 at 0x%.4x (0x%.2x)\n", ioport->io2.minimum,
						ioport->io2.address_length);
			}
			else {
				printk(KERN_ERR DRV_PFX "Unknown SPIC Type, more than 2 IO Ports\n");
				return AE_ERROR;
			}
			return AE_OK;
		}
	default:
		dprintk("Resource %d isn't an IRQ nor an IO port\n",
				resource->type);

	case ACPI_RESOURCE_TYPE_END_TAG:
		return AE_OK;
	}
	return AE_CTRL_TERMINATE;
}

static int sony_pic_possible_resources(struct acpi_device *device)
{
	int result = 0;
	acpi_status status = AE_OK;

	if (!device)
		return -EINVAL;

	/* get device status */
	/* see acpi_pci_link_get_current acpi_pci_link_get_possible */
	dprintk("Evaluating _STA\n");
	result = acpi_bus_get_status(device);
	if (result) {
		printk(KERN_WARNING DRV_PFX "Unable to read status\n");
		goto end;
	}

	if (!device->status.enabled)
		dprintk("Device disabled\n");
	else
		dprintk("Device enabled\n");

	/*
	 * Query and parse 'method'
	 */
	dprintk("Evaluating %s\n", METHOD_NAME__PRS);
	status = acpi_walk_resources(device->handle, METHOD_NAME__PRS,
			sony_pic_read_possible_resource, &spic_dev);
	if (ACPI_FAILURE(status)) {
		printk(KERN_WARNING DRV_PFX
				"Failure evaluating %s\n",
				METHOD_NAME__PRS);
		result = -ENODEV;
	}
end:
	return result;
}

/*
 *  Disable the spic device by calling its _DIS method
 */
static int sony_pic_disable(struct acpi_device *device)
{
	acpi_status ret = acpi_evaluate_object(device->handle, "_DIS", NULL,
					       NULL);

	if (ACPI_FAILURE(ret) && ret != AE_NOT_FOUND)
		return -ENXIO;

	dprintk("Device disabled\n");
	return 0;
}


/*
 *  Based on drivers/acpi/pci_link.c:acpi_pci_link_set
 *
 *  Call _SRS to set current resources
 */
static int sony_pic_enable(struct acpi_device *device,
		struct sony_pic_ioport *ioport, struct sony_pic_irq *irq)
{
	acpi_status status;
	int result = 0;
	/* Type 1 resource layout is:
	 *    IO
	 *    IO
	 *    IRQNoFlags
	 *    End
	 *
	 * Type 2 and 3 resource layout is:
	 *    IO
	 *    IRQNoFlags
	 *    End
	 */
	struct {
		struct acpi_resource res1;
		struct acpi_resource res2;
		struct acpi_resource res3;
		struct acpi_resource res4;
	} *resource;
	struct acpi_buffer buffer = { 0, NULL };

	if (!ioport || !irq)
		return -EINVAL;

	/* init acpi_buffer */
	resource = kzalloc(sizeof(*resource) + 1, GFP_KERNEL);
	if (!resource)
		return -ENOMEM;

	buffer.length = sizeof(*resource) + 1;
	buffer.pointer = resource;

	/* setup Type 1 resources */
	if (spic_dev.model == SONYPI_DEVICE_TYPE1) {

		/* setup io resources */
		resource->res1.type = ACPI_RESOURCE_TYPE_IO;
		resource->res1.length = sizeof(struct acpi_resource);
		memcpy(&resource->res1.data.io, &ioport->io1,
				sizeof(struct acpi_resource_io));

		resource->res2.type = ACPI_RESOURCE_TYPE_IO;
		resource->res2.length = sizeof(struct acpi_resource);
		memcpy(&resource->res2.data.io, &ioport->io2,
				sizeof(struct acpi_resource_io));

		/* setup irq resource */
		resource->res3.type = ACPI_RESOURCE_TYPE_IRQ;
		resource->res3.length = sizeof(struct acpi_resource);
		memcpy(&resource->res3.data.irq, &irq->irq,
				sizeof(struct acpi_resource_irq));
		/* we requested a shared irq */
		resource->res3.data.irq.sharable = ACPI_SHARED;

		resource->res4.type = ACPI_RESOURCE_TYPE_END_TAG;

	}
	/* setup Type 2/3 resources */
	else {
		/* setup io resource */
		resource->res1.type = ACPI_RESOURCE_TYPE_IO;
		resource->res1.length = sizeof(struct acpi_resource);
		memcpy(&resource->res1.data.io, &ioport->io1,
				sizeof(struct acpi_resource_io));

		/* setup irq resource */
		resource->res2.type = ACPI_RESOURCE_TYPE_IRQ;
		resource->res2.length = sizeof(struct acpi_resource);
		memcpy(&resource->res2.data.irq, &irq->irq,
				sizeof(struct acpi_resource_irq));
		/* we requested a shared irq */
		resource->res2.data.irq.sharable = ACPI_SHARED;

		resource->res3.type = ACPI_RESOURCE_TYPE_END_TAG;
	}

	/* Attempt to set the resource */
	dprintk("Evaluating _SRS\n");
	status = acpi_set_current_resources(device->handle, &buffer);

	/* check for total failure */
	if (ACPI_FAILURE(status)) {
		printk(KERN_ERR DRV_PFX "Error evaluating _SRS\n");
		result = -ENODEV;
		goto end;
	}

	/* Necessary device initializations calls (from sonypi) */
	sony_pic_call1(0x82);
	sony_pic_call2(0x81, 0xff);
	sony_pic_call1(compat ? 0x92 : 0x82);

end:
	kfree(resource);
	return result;
}

/*****************
 *
 * ISR: some event is available
 *
 *****************/
static irqreturn_t sony_pic_irq(int irq, void *dev_id)
{
	int i, j;
	u8 ev = 0;
	u8 data_mask = 0;
	u8 device_event = 0;

	struct sony_pic_dev *dev = (struct sony_pic_dev *) dev_id;

	ev = inb_p(dev->cur_ioport->io1.minimum);
	if (dev->cur_ioport->io2.minimum)
		data_mask = inb_p(dev->cur_ioport->io2.minimum);
	else
		data_mask = inb_p(dev->cur_ioport->io1.minimum +
				dev->evport_offset);

	dprintk("event ([%.2x] [%.2x]) at port 0x%.4x(+0x%.2x)\n",
			ev, data_mask, dev->cur_ioport->io1.minimum,
			dev->evport_offset);

	if (ev == 0x00 || ev == 0xff)
		return IRQ_HANDLED;

	for (i = 0; dev->event_types[i].mask; i++) {

		if ((data_mask & dev->event_types[i].data) !=
		    dev->event_types[i].data)
			continue;

		if (!(mask & dev->event_types[i].mask))
			continue;

		for (j = 0; dev->event_types[i].events[j].event; j++) {
			if (ev == dev->event_types[i].events[j].data) {
				device_event =
					dev->event_types[i].events[j].event;
				goto found;
			}
		}
	}
	/* Still not able to decode the event try to pass
	 * it over to the minidriver
	 */
	if (dev->handle_irq && dev->handle_irq(data_mask, ev) == 0)
		return IRQ_HANDLED;

	dprintk("unknown event ([%.2x] [%.2x]) at port 0x%.4x(+0x%.2x)\n",
			ev, data_mask, dev->cur_ioport->io1.minimum,
			dev->evport_offset);
	return IRQ_HANDLED;

found:
	sony_laptop_report_input_event(device_event);
	acpi_bus_generate_proc_event(dev->acpi_dev, 1, device_event);
	sonypi_compat_report_event(device_event);

	return IRQ_HANDLED;
}

/*****************
 *
 *  ACPI driver
 *
 *****************/
static int sony_pic_remove(struct acpi_device *device, int type)
{
	struct sony_pic_ioport *io, *tmp_io;
	struct sony_pic_irq *irq, *tmp_irq;

	if (sony_pic_disable(device)) {
		printk(KERN_ERR DRV_PFX "Couldn't disable device.\n");
		return -ENXIO;
	}

	free_irq(spic_dev.cur_irq->irq.interrupts[0], &spic_dev);
	release_region(spic_dev.cur_ioport->io1.minimum,
			spic_dev.cur_ioport->io1.address_length);
	if (spic_dev.cur_ioport->io2.minimum)
		release_region(spic_dev.cur_ioport->io2.minimum,
				spic_dev.cur_ioport->io2.address_length);

	sonypi_compat_exit();

	sony_laptop_remove_input();

	/* pf attrs */
	sysfs_remove_group(&sony_pf_device->dev.kobj, &spic_attribute_group);
	sony_pf_remove();

	list_for_each_entry_safe(io, tmp_io, &spic_dev.ioports, list) {
		list_del(&io->list);
		kfree(io);
	}
	list_for_each_entry_safe(irq, tmp_irq, &spic_dev.interrupts, list) {
		list_del(&irq->list);
		kfree(irq);
	}
	spic_dev.cur_ioport = NULL;
	spic_dev.cur_irq = NULL;

	dprintk(SONY_PIC_DRIVER_NAME " removed.\n");
	return 0;
}

static int sony_pic_add(struct acpi_device *device)
{
	int result;
	struct sony_pic_ioport *io, *tmp_io;
	struct sony_pic_irq *irq, *tmp_irq;

	printk(KERN_INFO DRV_PFX "%s v%s.\n",
		SONY_PIC_DRIVER_NAME, SONY_LAPTOP_DRIVER_VERSION);

	spic_dev.acpi_dev = device;
	strcpy(acpi_device_class(device), "sony/hotkey");
	sony_pic_detect_device_type(&spic_dev);
	mutex_init(&spic_dev.lock);

	/* read _PRS resources */
	result = sony_pic_possible_resources(device);
	if (result) {
		printk(KERN_ERR DRV_PFX
				"Unabe to read possible resources.\n");
		goto err_free_resources;
	}

	/* setup input devices and helper fifo */
	result = sony_laptop_setup_input(device);
	if (result) {
		printk(KERN_ERR DRV_PFX
				"Unabe to create input devices.\n");
		goto err_free_resources;
	}

	if (sonypi_compat_init())
		goto err_remove_input;

	/* request io port */
	list_for_each_entry_reverse(io, &spic_dev.ioports, list) {
		if (request_region(io->io1.minimum, io->io1.address_length,
					"Sony Programable I/O Device")) {
			dprintk("I/O port1: 0x%.4x (0x%.4x) + 0x%.2x\n",
					io->io1.minimum, io->io1.maximum,
					io->io1.address_length);
			/* Type 1 have 2 ioports */
			if (io->io2.minimum) {
				if (request_region(io->io2.minimum,
						io->io2.address_length,
						"Sony Programable I/O Device")) {
					dprintk("I/O port2: 0x%.4x (0x%.4x) + 0x%.2x\n",
							io->io2.minimum, io->io2.maximum,
							io->io2.address_length);
					spic_dev.cur_ioport = io;
					break;
				}
				else {
					dprintk("Unable to get I/O port2: "
							"0x%.4x (0x%.4x) + 0x%.2x\n",
							io->io2.minimum, io->io2.maximum,
							io->io2.address_length);
					release_region(io->io1.minimum,
							io->io1.address_length);
				}
			}
			else {
				spic_dev.cur_ioport = io;
				break;
			}
		}
	}
	if (!spic_dev.cur_ioport) {
		printk(KERN_ERR DRV_PFX "Failed to request_region.\n");
		result = -ENODEV;
		goto err_remove_compat;
	}

	/* request IRQ */
	list_for_each_entry_reverse(irq, &spic_dev.interrupts, list) {
		if (!request_irq(irq->irq.interrupts[0], sony_pic_irq,
					IRQF_DISABLED, "sony-laptop", &spic_dev)) {
			dprintk("IRQ: %d - triggering: %d - "
					"polarity: %d - shr: %d\n",
					irq->irq.interrupts[0],
					irq->irq.triggering,
					irq->irq.polarity,
					irq->irq.sharable);
			spic_dev.cur_irq = irq;
			break;
		}
	}
	if (!spic_dev.cur_irq) {
		printk(KERN_ERR DRV_PFX "Failed to request_irq.\n");
		result = -ENODEV;
		goto err_release_region;
	}

	/* set resource status _SRS */
	result = sony_pic_enable(device, spic_dev.cur_ioport, spic_dev.cur_irq);
	if (result) {
		printk(KERN_ERR DRV_PFX "Couldn't enable device.\n");
		goto err_free_irq;
	}

	spic_dev.bluetooth_power = -1;
	/* create device attributes */
	result = sony_pf_add();
	if (result)
		goto err_disable_device;

	result = sysfs_create_group(&sony_pf_device->dev.kobj, &spic_attribute_group);
	if (result)
		goto err_remove_pf;

	return 0;

err_remove_pf:
	sony_pf_remove();

err_disable_device:
	sony_pic_disable(device);

err_free_irq:
	free_irq(spic_dev.cur_irq->irq.interrupts[0], &spic_dev);

err_release_region:
	release_region(spic_dev.cur_ioport->io1.minimum,
			spic_dev.cur_ioport->io1.address_length);
	if (spic_dev.cur_ioport->io2.minimum)
		release_region(spic_dev.cur_ioport->io2.minimum,
				spic_dev.cur_ioport->io2.address_length);

err_remove_compat:
	sonypi_compat_exit();

err_remove_input:
	sony_laptop_remove_input();

err_free_resources:
	list_for_each_entry_safe(io, tmp_io, &spic_dev.ioports, list) {
		list_del(&io->list);
		kfree(io);
	}
	list_for_each_entry_safe(irq, tmp_irq, &spic_dev.interrupts, list) {
		list_del(&irq->list);
		kfree(irq);
	}
	spic_dev.cur_ioport = NULL;
	spic_dev.cur_irq = NULL;

	return result;
}

static int sony_pic_suspend(struct acpi_device *device, pm_message_t state)
{
	if (sony_pic_disable(device))
		return -ENXIO;
	return 0;
}

static int sony_pic_resume(struct acpi_device *device)
{
	sony_pic_enable(device, spic_dev.cur_ioport, spic_dev.cur_irq);
	return 0;
}

static const struct acpi_device_id sony_pic_device_ids[] = {
	{SONY_PIC_HID, 0},
	{"", 0},
};

static struct acpi_driver sony_pic_driver = {
	.name = SONY_PIC_DRIVER_NAME,
	.class = SONY_PIC_CLASS,
	.ids = sony_pic_device_ids,
	.owner = THIS_MODULE,
	.ops = {
		.add = sony_pic_add,
		.remove = sony_pic_remove,
		.suspend = sony_pic_suspend,
		.resume = sony_pic_resume,
		},
};

static struct dmi_system_id __initdata sonypi_dmi_table[] = {
	{
		.ident = "Sony Vaio",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Sony Corporation"),
			DMI_MATCH(DMI_PRODUCT_NAME, "PCG-"),
		},
	},
	{
		.ident = "Sony Vaio",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Sony Corporation"),
			DMI_MATCH(DMI_PRODUCT_NAME, "VGN-"),
		},
	},
	{ }
};

static int __init sony_laptop_init(void)
{
	int result;

	if (!no_spic && dmi_check_system(sonypi_dmi_table)) {
		result = acpi_bus_register_driver(&sony_pic_driver);
		if (result) {
			printk(KERN_ERR DRV_PFX
					"Unable to register SPIC driver.");
			goto out;
		}
		spic_drv_registered = 1;
	}

	result = acpi_bus_register_driver(&sony_nc_driver);
	if (result) {
		printk(KERN_ERR DRV_PFX "Unable to register SNC driver.");
		goto out_unregister_pic;
	}

	return 0;

out_unregister_pic:
	if (spic_drv_registered)
		acpi_bus_unregister_driver(&sony_pic_driver);
out:
	return result;
}

static void __exit sony_laptop_exit(void)
{
	acpi_bus_unregister_driver(&sony_nc_driver);
	if (spic_drv_registered)
		acpi_bus_unregister_driver(&sony_pic_driver);
}

module_init(sony_laptop_init);
module_exit(sony_laptop_exit);
