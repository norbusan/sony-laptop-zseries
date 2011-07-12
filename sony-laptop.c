/*
 * ACPI Sony Notebook Control Driver (SNC and SPIC)
 *
 * Copyright (C) 2004-2005 Stelian Pop <stelian@popies.net>
 * Copyright (C) 2007-2009 Mattia Dongili <malattia@linux.it>
 * Copyright (C) 2011 Marco Chiappero <marco@absence.it>
 * Copyright (C) 2011 Javier Achirica <jachirica@gmail.com>
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

/*
 * defining SONY_ZSERIES includes code for graphics card switching
 */
#define SONY_ZSERIES


#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

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
#include <linux/slab.h>
#include <acpi/acpi_drivers.h>
#include <acpi/acpi_bus.h>
#include <linux/uaccess.h>
#include <linux/sonypi.h>
#include <linux/sony-laptop.h>
#include <linux/rfkill.h>
#ifdef CONFIG_SONYPI_COMPAT
#include <linux/poll.h>
#include <linux/miscdevice.h>
#endif
#ifdef SONY_ZSERIES
#include <linux/version.h>
#endif


#define dprintk(fmt, ...)			\
do {						\
	if (debug)				\
		pr_warn(fmt, ##__VA_ARGS__);	\
} while (0)

#ifdef SONY_ZSERIES
#define SONY_LAPTOP_DRIVER_VERSION     "0.9np8pre2"
#else
#define SONY_LAPTOP_DRIVER_VERSION	"0.6"
#endif

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

#ifdef CONFIG_SONYPI_COMPAT
static int minor = -1;
module_param(minor, int, 0);
MODULE_PARM_DESC(minor,
		 "minor number of the misc device for the SPIC compatibility code, "
		 "default is -1 (automatic)");
#endif

static int kbd_backlight;	/* = 0 */
module_param(kbd_backlight, int, 0444);
MODULE_PARM_DESC(kbd_backlight,
		 "set this to 0 to disable keyboard backlight, "
		 "1 to enable it (default: 0)");

static int kbd_backlight_timeout;	/* = 0 */
module_param(kbd_backlight_timeout, int, 0444);
MODULE_PARM_DESC(kbd_backlight_timeout,
		 "set this to 0 to set the default 10 seconds timeout, "
		 "1 for 30 seconds, 2 for 60 seconds and 3 to disable timeout "
		 "(default: 0)");

static int force_shock_notifications;	/* = 0 */
module_param(force_shock_notifications, int, 0);
MODULE_PARM_DESC(force_shock_notifications,
		"set this to 1 to force the generation of shock protection "
		"events, even though the notebook do not support head "
		"unloading for the installed drive drive");

#ifdef SONY_ZSERIES
static int speed_stamina;
module_param(speed_stamina, int, 0444);
MODULE_PARM_DESC(speed_stamina,
                 "Set this to 1 to enable SPEED mode on module load (EXPERIMENTAL)");
static int sony_dsm_type = 0;
static char *sony_acpi_path_dsm[] =
{
	"\\_SB.PCI0.OVGA._DSM",
	"\\_SB.PCI0.P0P2.DGPU._DSM"
};
static char *sony_acpi_path_hsc1[] =
{
	"\\_SB.PCI0.LPC.SNC.HSC1",
	"\\_SB.PCI0.LPCB.SNC.HSC1"
};
/* static acpi_handle sony_nc_acpi_handle; */
static int acpi_callgetfunc(acpi_handle, char*, unsigned int*);
#endif
/*********** Input Devices ***********/

#define SONY_LAPTOP_BUF_SIZE	128
struct sony_laptop_input_s {
	atomic_t		users;
	struct input_dev	*jog_dev;
	struct input_dev	*key_dev;
	struct kfifo		fifo;
	spinlock_t		fifo_lock;
	struct timer_list	release_key_timer;
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
	58,	/* 72 SONYPI_EVENT_MEDIA_PRESSED */
	59,	/* 73 SONYPI_EVENT_VENDOR_PRESSED */
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
	KEY_MEDIA,	/* 58 SONYPI_EVENT_MEDIA_PRESSED */
	KEY_VENDOR,	/* 59 SONYPI_EVENT_VENDOR_PRESSED */
};

/* release buttons after a short delay if pressed */
static void do_sony_laptop_release_key(unsigned long unused)
{
	struct sony_laptop_keypress kp;
	unsigned long flags;

	spin_lock_irqsave(&sony_laptop_input.fifo_lock, flags);

	if (kfifo_out(&sony_laptop_input.fifo,
		      (unsigned char *)&kp, sizeof(kp)) == sizeof(kp)) {
		input_report_key(kp.dev, kp.key, 0);
		input_sync(kp.dev);
	}

	/* If there is something in the fifo schedule next release. */
	if (kfifo_len(&sony_laptop_input.fifo) != 0)
		mod_timer(&sony_laptop_input.release_key_timer,
			  jiffies + msecs_to_jiffies(10));

	spin_unlock_irqrestore(&sony_laptop_input.fifo_lock, flags);
}

/* forward event to the input subsystem */
static void sony_laptop_report_input_event(u8 event)
{
	struct input_dev *jog_dev = sony_laptop_input.jog_dev;
	struct input_dev *key_dev = sony_laptop_input.key_dev;
	struct sony_laptop_keypress kp = { NULL };

	if (event == SONYPI_EVENT_FNKEY_RELEASED ||
			event == SONYPI_EVENT_ANYBUTTON_RELEASED) {
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
			dprintk("sony_laptop_report_input_event, "
				"event not known: %d\n", event);
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

		/* schedule key release */
		kfifo_in_locked(&sony_laptop_input.fifo,
				(unsigned char *)&kp, sizeof(kp),
				&sony_laptop_input.fifo_lock);
		mod_timer(&sony_laptop_input.release_key_timer,
			  jiffies + msecs_to_jiffies(10));
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
	error = kfifo_alloc(&sony_laptop_input.fifo,
			    SONY_LAPTOP_BUF_SIZE, GFP_KERNEL);
	if (error) {
		pr_err("kfifo_alloc failed\n");
		goto err_dec_users;
	}

	setup_timer(&sony_laptop_input.release_key_timer,
		    do_sony_laptop_release_key, 0);

	/* input keys */
	key_dev = input_allocate_device();
	if (!key_dev) {
		error = -ENOMEM;
		goto err_free_kfifo;
	}

	key_dev->name = "Sony Vaio Keys";
	key_dev->id.bustype = BUS_ISA;
	key_dev->id.vendor = PCI_VENDOR_ID_SONY;
	key_dev->dev.parent = &acpi_device->dev;

	/* Initialize the Input Drivers: special keys */
	input_set_capability(key_dev, EV_MSC, MSC_SCAN);

	__set_bit(EV_KEY, key_dev->evbit);
	key_dev->keycodesize = sizeof(sony_laptop_input_keycode_map[0]);
	key_dev->keycodemax = ARRAY_SIZE(sony_laptop_input_keycode_map);
	key_dev->keycode = &sony_laptop_input_keycode_map;
	for (i = 0; i < ARRAY_SIZE(sony_laptop_input_keycode_map); i++)
		__set_bit(sony_laptop_input_keycode_map[i], key_dev->keybit);
	__clear_bit(KEY_RESERVED, key_dev->keybit);

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

	input_set_capability(jog_dev, EV_KEY, BTN_MIDDLE);
	input_set_capability(jog_dev, EV_REL, REL_WHEEL);

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

err_free_kfifo:
	kfifo_free(&sony_laptop_input.fifo);

err_dec_users:
	atomic_dec(&sony_laptop_input.users);
	return error;
}

static void sony_laptop_remove_input(void)
{
	struct sony_laptop_keypress kp = { NULL };

	/* Cleanup only after the last user has gone */
	if (!atomic_dec_and_test(&sony_laptop_input.users))
		return;

	del_timer_sync(&sony_laptop_input.release_key_timer);

	/*
	 * Generate key-up events for remaining keys. Note that we don't
	 * need locking since nobody is adding new events to the kfifo.
	 */
	while (kfifo_out(&sony_laptop_input.fifo,
			 (unsigned char *)&kp, sizeof(kp)) == sizeof(kp)) {
		input_report_key(kp.dev, kp.key, 0);
		input_sync(kp.dev);
	}

	/* destroy input devs */
	input_unregister_device(sony_laptop_input.key_dev);
	sony_laptop_input.key_dev = NULL;

	if (sony_laptop_input.jog_dev) {
		input_unregister_device(sony_laptop_input.jog_dev);
		sony_laptop_input.jog_dev = NULL;
	}

	kfifo_free(&sony_laptop_input.fifo);
}

/*********** Platform Device ***********/
#ifdef SONY_ZSERIES
static int sony_ovga_dsm(int func, int arg)
{
	static char muid[] = {
		/*00*/  0xA0, 0xA0, 0x95, 0x9D, 0x60, 0x00, 0x48, 0x4D,	 /* MUID */
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

	result = acpi_evaluate_object(NULL, (char*)sony_acpi_path_dsm[sony_dsm_type], &input, &output);
	if (result) {
		printk("%s failed: %d, func %d, para, %d.\n", sony_acpi_path_dsm[sony_dsm_type], result, func, arg);
		return -1;
	}
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

	/* Determine which variant, VGN or VPC */
	if(ACPI_SUCCESS(acpi_callgetfunc(NULL, "\\_SB.PCI0.P0P2.DGPU._STA", &result)))
		sony_dsm_type = 1;

	pr_info("Determined GFX switch ACPI path as %s.\n", sony_acpi_path_dsm[sony_dsm_type]);
	result = device_create_file(&pdev->dev, &sony_pf_speed_stamina_attr);
	if (result)
		printk(KERN_DEBUG "sony_pf_probe: failed to add speed/stamina switch\n");

	/* initialize default, look at module param speed_stamina or switch */
	if (!ACPI_SUCCESS(acpi_callgetfunc(NULL, sony_acpi_path_hsc1[sony_dsm_type], &result))) {
		result = -1;
		dprintk("sony_nc_notify: "
			"cannot query speed/stamina switch\n");
	}
	else
	{
		pr_info("Speed/stamina switch: %s.\n", (result & 0x80)?"auto":(result & 2)?"stamina":"speed");
		if(!(result & 2))
			speed_stamina = 1;
		else if((result & 0x80) && sony_dsm_type == 1)
		{
			if((ACPI_SUCCESS(acpi_callgetfunc(NULL, "\\_SB.ADP1._PSR", &result))) && (result == 1))
			{
				pr_info("PSU connected - Selecting speed mode.\n");
				speed_stamina = 1;
			}
		}
	}

	if (speed_stamina == 1) {
		sony_dgpu_on();
		sony_led_speed();
	} else {
		sony_dgpu_off();
		sony_led_stamina();
	}
	return 0;
}
static int sony_resume_noirq(struct device *pdev)
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

static struct dev_pm_ops sony_dev_pm_ops = {
	.resume_noirq = sony_resume_noirq,
};
#endif  /* SONY_ZSERIES */


static atomic_t sony_pf_users = ATOMIC_INIT(0);
static struct platform_driver sony_pf_driver = {
	.driver = {
		   .name = "sony-laptop",
		   .owner = THIS_MODULE,
#ifdef SONY_ZSERIES
#ifdef CONFIG_PM
		   .pm = &sony_dev_pm_ops,
#endif
#endif
		  }
#ifdef SONY_ZSERIES
	,
	.probe  = sony_pf_probe,
#endif
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

	platform_device_unregister(sony_pf_device);
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
	struct device_attribute devattr;	/* sysfs attribute */
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

static struct sony_nc_value sony_nc_values[] = {
	SNC_HANDLE(brightness_default, snc_brightness_def_get,
			snc_brightness_def_set, brightness_default_validate, 0),
	SNC_HANDLE(fnkey, snc_fnkey_get, NULL, NULL, 0),
	SNC_HANDLE(cdpower, snc_cdpower_get, snc_cdpower_set,
			boolean_validate, 0),
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
	SNC_HANDLE_NULL
};

static acpi_handle sony_nc_acpi_handle;
static struct acpi_device *sony_nc_acpi_device;

/*
 * acpi_evaluate_object wrappers
 */
static int acpi_callgetfunc(acpi_handle handle, char *name,
				unsigned int *result)
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

	pr_warn("acpi_callreadfunc failed\n");

	return -1;
}

static int acpi_callsetfunc(acpi_handle handle, char *name, u32 value,
				unsigned int *result)
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
				pr_warn("acpi_evaluate_object bad "
					"return type\n");
				return -1;
			}
			*result = out_obj.integer.value;
		}
		return 0;
	}

	pr_warn("acpi_evaluate_object failed\n");

	return -1;
}

static int acpi_callsetfunc_buffer(acpi_handle handle, u64 value,
					u8 array[], unsigned int size)
{
	u8 buffer[sizeof(value)];
	int length = -1;
	struct acpi_object_list params;
	union acpi_object in_obj;
	union acpi_object *values;
	struct acpi_buffer output = {ACPI_ALLOCATE_BUFFER, NULL};
	acpi_status status;

	if (!array || !size)
		return length;

	/* use a buffer type as parameter to overcome any 32 bits ACPI limit */
	memcpy(buffer, &value, sizeof(buffer));

	params.count = 1;
	params.pointer = &in_obj;
	in_obj.type = ACPI_TYPE_BUFFER;
	in_obj.buffer.length = sizeof(buffer);
	in_obj.buffer.pointer = buffer;

	/* since SN06 is the only known method returning a buffer we
	 * can hard code it, it is not necessary to have a parameter
	 */
	status = acpi_evaluate_object(sony_nc_acpi_handle, "SN06", &params,
			&output);
	values = (union acpi_object *) output.pointer;
	if (ACPI_FAILURE(status) || !values) {
		dprintk("acpi_evaluate_object failed\n");
		goto error;
	}

	/* some buggy DSDTs return integer when the output does
	   not execede the 4 bytes size
	*/
	if (values->type == ACPI_TYPE_BUFFER) {
		if (values->buffer.length <= 0)
			goto error;

		length = size > values->buffer.length ?
			values->buffer.length : size;

		memcpy(array, values->buffer.pointer, length);
	} else if (values->type == ACPI_TYPE_INTEGER) {
		u32 result = values->integer.value;
		if (size < 4)
			goto error;

		length = 0;
		while (length != 4) {
			array[length] = result & 0xff;
			result >>= 8;
			length++;
		}
	} else {
		pr_err("Invalid return object 0x%.2x\n", values->type);
		goto error;
	}

error:
	kfree(output.pointer);
	return length;
}

struct sony_nc_handles {
	u16 cap[0x10];
	struct device_attribute devattr;
};

static struct sony_nc_handles *handles;

static ssize_t sony_nc_handles_show(struct device *dev,
		struct device_attribute *attr, char *buffer)
{
	ssize_t len = 0;
	int i;

	for (i = 0; i < ARRAY_SIZE(handles->cap); i++) {
		len += snprintf(buffer + len, PAGE_SIZE - len, "0x%.4x ",
				handles->cap[i]);
	}
	len += snprintf(buffer + len, PAGE_SIZE - len, "\n");

	return len;
}

static int sony_nc_handles_setup(struct platform_device *pd)
{
	unsigned int i, result;

	handles = kzalloc(sizeof(*handles), GFP_KERNEL);
	if (!handles)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(handles->cap); i++) {
		if (!acpi_callsetfunc(sony_nc_acpi_handle,
					"SN00", i + 0x20, &result)) {
			dprintk("caching handle 0x%.4x (offset: 0x%.2x)\n",
					result, i);
			handles->cap[i] = result;
		}
	}

	if (debug) {
		sysfs_attr_init(&handles->devattr.attr);
		handles->devattr.attr.name = "handles";
		handles->devattr.attr.mode = S_IRUGO;
		handles->devattr.show = sony_nc_handles_show;

		/* allow reading capabilities via sysfs */
		if (device_create_file(&pd->dev, &handles->devattr)) {
			kfree(handles);
			handles = NULL;
			return -1;
		}
	}

	return 0;
}

static int sony_nc_handles_cleanup(struct platform_device *pd)
{
	if (handles) {
		if (debug)
			device_remove_file(&pd->dev, &handles->devattr);
		kfree(handles);
		handles = NULL;
	}
	return 0;
}

static int sony_find_snc_handle(unsigned int handle)
{
	int i;

	/* not initialized yet or invalid handle, return early */
	if (!handles || !handle)
		return -1;

	for (i = 0; i < 0x10; i++) {
		if (handles->cap[i] == handle) {
			dprintk("found handle 0x%.4x (offset: 0x%.2x)\n",
					handle, i);
			return i;
		}
	}
	dprintk("handle 0x%.4x not found\n", handle);
	return -1;
}

/* call command method SN07, accepts a 32 bit integer, returns a integer */
static int sony_call_snc_handle(unsigned int handle, unsigned int argument,
				unsigned int *result)
{
	int ret = 0;
	int offset = sony_find_snc_handle(handle);

	if (offset < 0)
		return -1;

	/* max 32 bit wide argument, for wider input use SN06 */
	ret = acpi_callsetfunc(sony_nc_acpi_handle, "SN07", offset | argument,
			result);
	dprintk("called SN07 with 0x%.4x (result: 0x%.4x)\n", offset | argument,
			*result);
	return ret;
}

/* call command method SN06, accepts a wide input buffer, returns a buffer */
static int sony_call_snc_handle_buffer(unsigned int handle, u64 argument,
					u8 result[], unsigned int size)
{
	int ret = 0;
	int offset = sony_find_snc_handle(handle);

	if (offset < 0)
		return -1;

	ret = acpi_callsetfunc_buffer(sony_nc_acpi_handle,
			offset | argument, result, size);
	dprintk("called SN06 with 0x%.4llx (%u bytes read)\n",
			offset | argument, ret);

	return ret;
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
static ssize_t sony_nc_sysfs_show(struct device *dev,
					struct device_attribute *attr,
					char *buffer)
{
	unsigned int value;
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
	unsigned long value;
	struct sony_nc_value *item =
	    container_of(attr, struct sony_nc_value, devattr);

	if (!item->acpiset)
		return -EIO;

	if (count > 31)
		return -EINVAL;

	if (strict_strtoul(buffer, 10, &value))
		return -EINVAL;

	if (item->validate)
		value = item->validate(SNC_VALIDATE_IN, value);

	if (value < 0)
		return value;

	if (acpi_callsetfunc(sony_nc_acpi_handle,
				*item->acpiset, value, NULL) < 0)
		return -EIO;
	item->value = value;
	item->valid = 1;
	return count;
}

/*
 * New SNC-only Vaios event mapping to driver known keys
 */
struct sony_nc_event {
	u8	data;
	u8	event;
};

static struct sony_nc_event sony_100_events[] = {
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
	{ 0x88, SONYPI_EVENT_FNKEY_F8 },
	{ 0x08, SONYPI_EVENT_FNKEY_RELEASED },
	{ 0x89, SONYPI_EVENT_FNKEY_F9 },
	{ 0x09, SONYPI_EVENT_FNKEY_RELEASED },
	{ 0x8A, SONYPI_EVENT_FNKEY_F10 },
	{ 0x0A, SONYPI_EVENT_FNKEY_RELEASED },
	{ 0x8B, SONYPI_EVENT_FNKEY_F11 },
	{ 0x0B, SONYPI_EVENT_FNKEY_RELEASED },
	{ 0x8C, SONYPI_EVENT_FNKEY_F12 },
	{ 0x0C, SONYPI_EVENT_FNKEY_RELEASED },
	{ 0x90, SONYPI_EVENT_PKEY_P1 },
	{ 0x10, SONYPI_EVENT_ANYBUTTON_RELEASED },
	{ 0x91, SONYPI_EVENT_PKEY_P2 },
	{ 0x11, SONYPI_EVENT_ANYBUTTON_RELEASED },
	{ 0x9d, SONYPI_EVENT_ZOOM_PRESSED },
	{ 0x1d, SONYPI_EVENT_ANYBUTTON_RELEASED },
	{ 0x9f, SONYPI_EVENT_CD_EJECT_PRESSED },
	{ 0x1f, SONYPI_EVENT_ANYBUTTON_RELEASED },
	{ 0xa1, SONYPI_EVENT_MEDIA_PRESSED },
	{ 0x21, SONYPI_EVENT_ANYBUTTON_RELEASED },
	{ 0xa4, SONYPI_EVENT_CD_EJECT_PRESSED },
	{ 0x24, SONYPI_EVENT_ANYBUTTON_RELEASED },
	{ 0xa5, SONYPI_EVENT_VENDOR_PRESSED },
	{ 0x25, SONYPI_EVENT_ANYBUTTON_RELEASED },
	{ 0xa6, SONYPI_EVENT_HELP_PRESSED },
	{ 0x26, SONYPI_EVENT_ANYBUTTON_RELEASED },
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
	{ 0x87, SONYPI_EVENT_SETTINGKEY_PRESSED },
	{ 0x07, SONYPI_EVENT_ANYBUTTON_RELEASED },
	{ 0, 0 },
};

/*
 * ACPI device
 */
static int sony_nc_function_setup(unsigned int handle)
{
	unsigned int result;

	if (handle == 0x0102)
		sony_call_snc_handle(0x0102, 0x100, &result);
	else
		sony_call_snc_handle(handle, 0, &result);

	return 0;
}

static int sony_nc_hotkeys_decode(unsigned int handle)
{
	int ret = -EINVAL;
	unsigned int result = 0;
	struct sony_nc_event *key_event;

	if (sony_call_snc_handle(handle, 0x200, &result)) {
		dprintk("sony_nc_hotkeys_decode,"
				" unable to retrieve the hotkey\n");
	} else {
		result &= 0xff;

		if (handle == 0x100)
			key_event = sony_100_events;
		else
			key_event = sony_127_events;

		for (; key_event->data; key_event++) {
			if (key_event->data == result) {
				ret = key_event->event;
				break;
			}
		}

		if (!key_event->data)
			pr_info("Unknown hotkey 0x%.2x (handle 0x%.2x)\n",
							result, handle);
		else
			dprintk("sony_nc_hotkeys_decode, hotkey 0x%.2x decoded "
					"to event 0x%.2x\n", result, ret);
	}

	return ret;
}

enum sony_nc_rfkill {
	SONY_WIFI,
	SONY_BLUETOOTH,
	SONY_WWAN,
	SONY_WIMAX,
	N_SONY_RFKILL,
};
struct sony_rfkill_data {
	struct rfkill *devices[N_SONY_RFKILL];
	const unsigned int address[N_SONY_RFKILL];
	unsigned int handle;
};
static struct sony_rfkill_data sony_rfkill = {
	{NULL}, {0x300, 0x500, 0x700, 0x900}, 0};

static int sony_nc_rfkill_update_wwan(void)
{
	unsigned int result, cmd;
	bool battery;
	bool swblock;

	if (sony_call_snc_handle(sony_rfkill.handle, 0x0200, &result))
		return -EIO;
	battery = !!(result & 0x2);

	/* retrieve the device block state */
	if (sony_call_snc_handle(sony_rfkill.handle,
				sony_rfkill.address[SONY_WWAN], &result))
		return -EIO;
	swblock = !(result & 0x02);

	if (battery && !swblock) {
		/* set the power state according with swblock */
		cmd = 0xff0000;
	} else if (!battery && !swblock) {
		swblock = true;
		cmd = 0x20000;
	} else {
		return 0;
	}

	cmd |= sony_rfkill.address[SONY_WWAN] + 0x100;

	/* set the power state */
	sony_call_snc_handle(sony_rfkill.handle, cmd, &result);

	/* update the rfkill sw state */
	rfkill_set_sw_state(sony_rfkill.devices[SONY_WWAN], swblock);

	return 0;
}

static int sony_nc_get_rfkill_hwblock(void)
{
	unsigned int result;

	if (sony_call_snc_handle(sony_rfkill.handle, 0x200, &result))
		return -1;

	return result & 0x1;
}

static void sony_nc_rfkill_cleanup(void)
{
	int i;

	for (i = 0; i < N_SONY_RFKILL; i++) {
		if (sony_rfkill.devices[i]) {
			rfkill_unregister(sony_rfkill.devices[i]);
			rfkill_destroy(sony_rfkill.devices[i]);
		}
	}
}

static int sony_nc_rfkill_set(void *data, bool blocked)
{
	unsigned int result, argument = sony_rfkill.address[(long) data];

	/* wwan state change not allowed when the battery is not present */
	sony_call_snc_handle(sony_rfkill.handle, 0x0200, &result);
	if (((long) data == SONY_WWAN) && !(result & 0x2)) {
		if (!blocked) {
			/* notify user space: the battery must be present */
			acpi_bus_generate_proc_event(sony_nc_acpi_device,
				       2, 2);
			acpi_bus_generate_netlink_event(
					sony_nc_acpi_device->pnp.device_class,
					dev_name(&sony_nc_acpi_device->dev),
					2, 2);
		}

		return -1;
	}

	/* do not force an already set state */
	sony_call_snc_handle(sony_rfkill.handle, argument, &result);
	if ((result & 0x1) == !blocked)
		return 0;

	argument += 0x100;
	if (!blocked)
		argument |= 0xff0000;

	return sony_call_snc_handle(sony_rfkill.handle, argument, &result);
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
	const char *name;
	unsigned int result;
	bool hwblock, swblock, wwblock;

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

	sony_call_snc_handle(sony_rfkill.handle, 0x200, &result);
	hwblock = !(result & 0x1);
	wwblock = !(result & 0x2);

	result = 0;
	sony_call_snc_handle(sony_rfkill.handle, sony_rfkill.address[nc_type],
				&result);
	swblock = !(result & 0x2);

	/* hard block the WWAN module if no battery is present */
	if ((nc_type == SONY_WWAN) && wwblock)
		swblock = true;

	rfkill_init_sw_state(rfk, swblock);
	rfkill_set_hw_state(rfk, hwblock);

	err = rfkill_register(rfk);
	if (err) {
		rfkill_destroy(rfk);
		return err;
	}
	sony_rfkill.devices[nc_type] = rfk;
	return err;
}

static void sony_nc_rfkill_update(void)
{
	enum sony_nc_rfkill i;
	unsigned int result;
	bool hwblock, swblock, wwblock;

	sony_call_snc_handle(sony_rfkill.handle, 0x200, &result);
	hwblock = !(result & 0x1);
	wwblock = !(result & 0x2);

	for (i = 0; i < N_SONY_RFKILL; i++) {
		unsigned int argument = sony_rfkill.address[i];

		if (!sony_rfkill.devices[i])
			continue;

		sony_call_snc_handle(sony_rfkill.handle, argument, &result);
		/* block wwan when no battery is present */
		if ((i == SONY_WWAN) && wwblock)
			swblock = true;
		else
			swblock = !(result & 0x2);

		rfkill_set_states(sony_rfkill.devices[i],
				  swblock, hwblock);
	}
}

static int sony_nc_rfkill_setup(struct acpi_device *device, unsigned int handle)
{
#define	RFKILL_BUFF_SIZE 8
	u8 dev_code, i, buff[RFKILL_BUFF_SIZE] = { 0 };

	sony_rfkill.handle = handle;

	/* need to read the whole buffer returned by the acpi call to SN06
	 * here otherwise we may miss some features
	 */
	if (sony_call_snc_handle_buffer(sony_rfkill.handle, 0x000,
					buff, RFKILL_BUFF_SIZE) < 0)
		return -EIO;

	/* the buffer is filled with magic numbers describing the devices
	 * available, 0xff terminates the enumeration
	 */
	for (i = 0; i < RFKILL_BUFF_SIZE; i++) {

		dev_code = buff[i];
		if (dev_code == 0xff)
			break;

		/*
		   known codes:

		   0x00	WLAN
		   0x10 BLUETOOTH
		   0x20 WWAN GPRS-EDGE
		   0x21 WWAN HSDPA
		   0x22 WWAN EV-DO
		   0x23 WWAN GPS
		   0x25	Gobi WWAN no GPS
		   0x26 Gobi WWAN + GPS
		   0x28	Gobi WWAN no GPS
		   0x29 Gobi WWAN + GPS
		   0x50	Gobi WWAN no GPS
		   0x51 Gobi WWAN + GPS
		   0x30	WIMAX
		   0x70 no SIM card slot
		   0x71 SIM card slot
		*/
		dprintk("Radio devices, looking at 0x%.2x\n", dev_code);

		if (dev_code == 0 && !sony_rfkill.devices[SONY_WIFI])
			sony_nc_setup_rfkill(device, SONY_WIFI);

		if (dev_code == 0x10 && !sony_rfkill.devices[SONY_BLUETOOTH])
			sony_nc_setup_rfkill(device, SONY_BLUETOOTH);

		if (((0xf0 & dev_code) == 0x20 || (0xf0 & dev_code) == 0x50) &&
				!sony_rfkill.devices[SONY_WWAN])
			sony_nc_setup_rfkill(device, SONY_WWAN);

		if (dev_code == 0x30 && !sony_rfkill.devices[SONY_WIMAX])
			sony_nc_setup_rfkill(device, SONY_WIMAX);
	}

	return 0;
}

/*	ALS controlled backlight feature	*/
/* generic ALS data and interface */
#define ALS_TABLE_SIZE	25

struct als_device_ops {
	int (*init)(const u8 defaults[]);
	int (*exit)(void);
	int (*event_handler)(void);
	int (*set_power)(unsigned int);
	int (*get_power)(unsigned int *);
	int (*get_lux)(unsigned int *, unsigned int *);
	int (*get_kelvin)(unsigned int *);
};

static struct  sony_als_device {
	unsigned int handle;

	unsigned int power;
	unsigned int managed;

	unsigned int levels_num;
	u8 *levels;
	unsigned int defaults_num;
	u8 *defaults;
	u8 parameters[ALS_TABLE_SIZE];

	/* common device operations */
	const struct als_device_ops *ops;

	/* basic ALS sys interface */
	unsigned int attrs_num;
	struct device_attribute attrs[7];
} *sony_als;

/*
	model specific ALS data and controls
	TAOS TSL256x device data
*/
#define LUX_SHIFT_BITS		16	/* for non-floating point math */
/* scale 100000 multiplied fractional coefficients rounding the values */
#define SCALE(u)	((((((u64) u) << LUX_SHIFT_BITS) / 10000) + 5) / 10)

#define TSL256X_REG_CTRL	0x00
#define TSL256X_REG_TIMING	0x01
#define TSL256X_REG_TLOW	0x02
#define TSL256X_REG_THIGH	0x04
#define TSL256X_REG_INT		0x06
#define TSL256X_REG_ID		0x0a
#define TSL256X_REG_DATA0	0x0c
#define TSL256X_REG_DATA1	0x0e

#define TSL256X_POWER_ON	0x03
#define TSL256X_POWER_OFF	0x00

#define TSL256X_POWER_MASK	0x03
#define TSL256X_INT_MASK	0x10

struct tsl256x_coeff {
	u32 ratio;
	u32 ch0;
	u32 ch1;
	u32 ka;
	s32 kb;
};

struct tsl256x_data {
	unsigned int gaintime;
	unsigned int periods;
	u8 *defaults;
	struct tsl256x_coeff const *coeff_table;
};
static struct tsl256x_data *tsl256x_handle;

static const struct tsl256x_coeff tsl256x_coeff_fn[] = {
	{
		.ratio	= SCALE(12500),	/* 0.125 * 2^LUX_SHIFT_BITS  */
		.ch0	= SCALE(3040),	/* 0.0304 * 2^LUX_SHIFT_BITS */
		.ch1	= SCALE(2720),	/* 0.0272 * 2^LUX_SHIFT_BITS */
		.ka	= SCALE(313550000),
		.kb	= -10651,
	}, {
		.ratio	= SCALE(25000),	/* 0.250 * 2^LUX_SHIFT_BITS  */
		.ch0	= SCALE(3250),	/* 0.0325 * 2^LUX_SHIFT_BITS */
		.ch1	= SCALE(4400),	/* 0.0440 * 2^LUX_SHIFT_BITS */
		.ka	= SCALE(203390000),
		.kb	= -2341,
	}, {
		.ratio	= SCALE(37500),	/* 0.375 * 2^LUX_SHIFT_BITS  */
		.ch0	= SCALE(3510),	/* 0.0351 * 2^LUX_SHIFT_BITS */
		.ch1	= SCALE(5440),	/* 0.0544 * 2^LUX_SHIFT_BITS */
		.ka	= SCALE(152180000),
		.kb	= 157,
	}, {
		.ratio	= SCALE(50000),	/* 0.50 * 2^LUX_SHIFT_BITS   */
		.ch0	= SCALE(3810),	/* 0.0381 * 2^LUX_SHIFT_BITS */
		.ch1	= SCALE(6240),	/* 0.0624 * 2^LUX_SHIFT_BITS */
		.ka	= SCALE(163580000),
		.kb	= -145,
	}, {
		.ratio	= SCALE(61000),	/* 0.61 * 2^LUX_SHIFT_BITS   */
		.ch0	= SCALE(2240),	/* 0.0224 * 2^LUX_SHIFT_BITS */
		.ch1	= SCALE(3100),	/* 0.0310 * 2^LUX_SHIFT_BITS */
		.ka	= SCALE(180800000),
		.kb	= -495,
	}, {
		.ratio	= SCALE(80000),	/* 0.80 * 2^LUX_SHIFT_BITS   */
		.ch0	= SCALE(1280),	/* 0.0128 * 2^LUX_SHIFT_BITS */
		.ch1	= SCALE(1530),	/* 0.0153 * 2^LUX_SHIFT_BITS */
		.ka	= SCALE(197340000),
		.kb	= -765
	}, {
		.ratio	= SCALE(130000),/* 1.3 * 2^LUX_SHIFT_BITS     */
		.ch0	= SCALE(146),	/* 0.00146 * 2^LUX_SHIFT_BITS */
		.ch1	= SCALE(112),	/* 0.00112 * 2^LUX_SHIFT_BITS */
		.ka	= SCALE(182900000),
		.kb	= -608,
	}, {
		.ratio	= UINT_MAX,	/* for higher ratios */
		.ch0	= 0,
		.ch1	= 0,
		.ka	= 0,
		.kb	= 830,
	}
};

static const struct tsl256x_coeff tsl256x_coeff_cs[] = {
	{
		.ratio  = SCALE(13000),	/* 0.130 * 2^LUX_SHIFT_BITS  */
		.ch0    = SCALE(3150),	/* 0.0315 * 2^LUX_SHIFT_BITS */
		.ch1    = SCALE(2620),	/* 0.0262 * 2^LUX_SHIFT_BITS */
		.ka	= SCALE(300370000),
		.kb	= -9587,
	}, {
		.ratio  = SCALE(26000),	/* 0.260 * 2^LUX_SHIFT_BITS  */
		.ch0    = SCALE(3370),	/* 0.0337 * 2^LUX_SHIFT_BITS */
		.ch1    = SCALE(4300),	/* 0.0430 * 2^LUX_SHIFT_BITS */
		.ka	= SCALE(194270000),
		.kb	= -1824,
	}, {
		.ratio  = SCALE(39000),	/* 0.390 * 2^LUX_SHIFT_BITS  */
		.ch0    = SCALE(3630),	/* 0.0363 * 2^LUX_SHIFT_BITS */
		.ch1    = SCALE(5290),	/* 0.0529 * 2^LUX_SHIFT_BITS */
		.ka	= SCALE(152520000),
		.kb	= 145,
	}, {
		.ratio  = SCALE(52000),	/* 0.520 * 2^LUX_SHIFT_BITS  */
		.ch0    = SCALE(3920),	/* 0.0392 * 2^LUX_SHIFT_BITS */
		.ch1    = SCALE(6050),	/* 0.0605 * 2^LUX_SHIFT_BITS */
		.ka	= SCALE(165960000),
		.kb	= -200,
	}, {
		.ratio  = SCALE(65000),	/* 0.650 * 2^LUX_SHIFT_BITS  */
		.ch0    = SCALE(2290),	/* 0.0229 * 2^LUX_SHIFT_BITS */
		.ch1    = SCALE(2910),	/* 0.0291 * 2^LUX_SHIFT_BITS */
		.ka	= SCALE(184800000),
		.kb	= -566,
	}, {
		.ratio  = SCALE(80000),	/* 0.800 * 2^LUX_SHIFT_BITS  */
		.ch0    = SCALE(1570),	/* 0.0157 * 2^LUX_SHIFT_BITS */
		.ch1    = SCALE(1800),	/* 0.0180 * 2^LUX_SHIFT_BITS */
		.ka	= SCALE(199220000),
		.kb	= -791,
	}, {
		.ratio  = SCALE(130000),/* 0.130 * 2^LUX_SHIFT_BITS  */
		.ch0    = SCALE(338),	/* 0.00338 * 2^LUX_SHIFT_BITS */
		.ch1    = SCALE(260),	/* 0.00260 * 2^LUX_SHIFT_BITS */
		.ka	= SCALE(182900000),
		.kb	= -608,
	}, {
		.ratio  = UINT_MAX,	/* for higher ratios */
		.ch0    = 0,
		.ch1    = 0,
		.ka	= 0,
		.kb	= 830,
	}
};

/*	TAOS helper & control functions		*/
static inline int tsl256x_exec_writebyte(unsigned int reg,
						unsigned int const *value)
{
	unsigned int result;

	return (sony_call_snc_handle(sony_als->handle, (*value << 0x18) |
		(reg << 0x10) | 0x800500, &result) || !(result & 0x01))
		? -EIO : 0;
}

static inline int tsl256x_exec_writeword(unsigned int reg,
						unsigned int const *value)
{
	u8 result[1];
	u64 arg = *value;

	/* using sony_call_snc_handle_buffer due to possible input overflows */
	return ((sony_call_snc_handle_buffer(sony_als->handle, (arg << 0x18) |
				(reg << 0x10) | 0xA00700, result, 1) < 0) ||
				!(result[0] & 0x01)) ? -EIO : 0;
}

static inline int tsl256x_exec_readbyte(unsigned int reg, unsigned int *result)
{
	if (sony_call_snc_handle(sony_als->handle, (reg << 0x10)
		| 0x800400, result) || !(*result & 0x01))
		return -EIO;
	*result = (*result >> 0x08) & 0xFF;

	return 0;
}

static inline int tsl256x_exec_readword(unsigned int reg, unsigned int *result)
{
	if (sony_call_snc_handle(sony_als->handle, (reg << 0x10)
		| 0xA00600, result) || !(*result & 0x01))
		return -EIO;
	*result = (*result >> 0x08) & 0xFFFF;

	return 0;
}

static int tsl256x_interrupt_ctrls(unsigned int *interrupt,
					unsigned int *periods)
{
	unsigned int value, result;

	/* if no interrupt parameter, retrieve interrupt status */
	if (!interrupt) {
		if (tsl256x_exec_readbyte(TSL256X_REG_INT, &result))
			return -EIO;

		value = (result & TSL256X_INT_MASK);
	} else {
		value = *interrupt << 0x04;
	}

	/* if no periods provided use the last one set */
	value |= (periods ? *periods : tsl256x_handle->periods);

	if (tsl256x_exec_writebyte(TSL256X_REG_INT, &value))
		return -EIO;

	if (periods)
		tsl256x_handle->periods = *periods;

	return 0;
}

static int tsl256x_setup(void)
{
	unsigned int interr = 1, zero = 0;

	/*
	 *   reset the threshold settings to trigger an event as soon
	 *   as the event goes on, forcing a backlight adaptation to
	 *   the current lighting conditions
	 */
	tsl256x_exec_writeword(TSL256X_REG_TLOW, &zero);
	tsl256x_exec_writeword(TSL256X_REG_THIGH, &zero);

	/* set gain and time */
	if (tsl256x_exec_writebyte(TSL256X_REG_TIMING,
				&tsl256x_handle->gaintime))
		return -EIO;

	/* restore persistence value and enable the interrupt generation */
	if (tsl256x_interrupt_ctrls(&interr, &tsl256x_handle->periods))
		return -EIO;

	return 0;
}

static int tsl256x_set_power(unsigned int status)
{
	int ret;

	if (status) {
		ret = tsl256x_setup();
		if (ret)
			return ret;
	}

	status = status ? TSL256X_POWER_ON : TSL256X_POWER_OFF;
	ret = tsl256x_exec_writebyte(TSL256X_REG_CTRL, &status);

	return ret;
}

static int tsl256x_get_power(unsigned int *status)
{
	if (tsl256x_exec_readbyte(TSL256X_REG_CTRL, status))
		return -EIO;

	*status = ((*status & TSL256X_POWER_MASK) == TSL256X_POWER_ON) ? 1 : 0;

	return 0;
}

static int tsl256x_get_raw_data(unsigned int *ch0, unsigned int *ch1)
{
	if (!ch0)
		return -1;

	if (tsl256x_exec_readword(TSL256X_REG_DATA0, ch0))
		return -EIO;

	if (ch1) {
		if (tsl256x_exec_readword(TSL256X_REG_DATA1, ch1))
			return -EIO;
	}

	return 0;
}

static int tsl256x_set_thresholds(const unsigned int *ch0)
{
	unsigned int tlow, thigh;

	tlow = (*ch0 * tsl256x_handle->defaults[0]) / 100;
	thigh = ((*ch0 * tsl256x_handle->defaults[1]) / 100) + 1;

	if (thigh > 0xffff)
		thigh = 0xffff;

	if (tsl256x_exec_writeword(TSL256X_REG_TLOW, &tlow) ||
		tsl256x_exec_writeword(TSL256X_REG_THIGH, &thigh))
		return -EIO;

	return 0;
}

#define MAX_LUX 1500
static void tsl256x_calculate_lux(const u32 ch0, const u32 ch1,
				unsigned int *integ, unsigned int *fract)
{
	/* the raw output from the sensor is just a "count" value, as
	   it is the result of the integration of the analog sensor
	   signal, the counts-to-lux curve (and its approximation can
	   be found on the datasheet.
	*/
	const struct tsl256x_coeff *coeff = tsl256x_handle->coeff_table;
	u32 ratio, temp, integer, fractional;

	if (ch0 >= 65535 || ch1 >= 65535)
		goto saturation;

	/* STEP 1: ratio calculation, for ch0 & ch1 coeff selection */

	/* protect against division by 0 */
	ratio = ch0 ? ((ch1 << (LUX_SHIFT_BITS + 1)) / ch0) : UINT_MAX;
	/* round the ratio value */
	ratio = ratio == UINT_MAX ? ratio : (ratio + 1) >> 1;

	/* coeff selection rule */
	while (coeff->ratio < ratio)
		coeff++;

	/* STEP 2: lux calculation formula using the right coeffcients */
	temp = (ch0 * coeff->ch0) - (ch1 * coeff->ch1);
	/* the sensor is placed under a plastic or glass cover which filters
	   a certain ammount of light (depending on that particular material).
	   To have an accurate reading, we need to compensate for this loss,
	   multiplying for compensation parameter, taken from the DSDT.
	*/
	temp *= tsl256x_handle->defaults[3] / 10;

	/* STEP 3: separate integer and fractional part */
	/* remove the integer part and multiply for the 10^N, N decimals  */
	fractional = (temp % (1 << LUX_SHIFT_BITS)) * 100; /* two decimals */
	/* scale down the value */
	fractional >>= LUX_SHIFT_BITS;

	/* strip off fractional portion to obtain the integer part */
	integer = temp >> LUX_SHIFT_BITS;

	if (integer > MAX_LUX)
		goto saturation;

	*integ = integer;
	*fract = fractional;

	return;

saturation:
	*integ = MAX_LUX;
	*fract = 0;
}

static void tsl256x_calculate_kelvin(const u32 *ch0, const u32 *ch1,
					unsigned int *temperature)
{
	const struct tsl256x_coeff *coeff = tsl256x_handle->coeff_table;
	u32 ratio;

	/* protect against division by 0 */
	ratio = *ch0 ? ((*ch1 << (LUX_SHIFT_BITS + 1)) / *ch0) : UINT_MAX;
	/* round the ratio value */
	ratio = (ratio + 1) >> 1;

	/* coeff selection rule */
	while (coeff->ratio < ratio)
		coeff++;

	*temperature = ratio ? coeff->ka / ratio + coeff->kb : 0;
}

static int tsl256x_get_lux(unsigned int *integ, unsigned int *fract)
{
	int ret = 0;
	unsigned int ch0, ch1;

	if (!integ || !fract)
		return -1;

	ret = tsl256x_get_raw_data(&ch0, &ch1);
	if (!ret)
		tsl256x_calculate_lux(ch0, ch1, integ, fract);

	return ret;
}

static int tsl256x_get_kelvin(unsigned int *temperature)
{
	int ret = -1;
	unsigned int ch0, ch1;

	if (!temperature)
		return ret;

	ret = tsl256x_get_raw_data(&ch0, &ch1);
	if (!ret)
		tsl256x_calculate_kelvin(&ch0, &ch1, temperature);

	return ret;
}

static int tsl256x_get_id(char *model, unsigned int *id, bool *cs)
{
	int ret;
	unsigned int result;
	char *name = NULL;
	bool unknown = false;
	bool type_cs = false;

	ret = tsl256x_exec_readbyte(TSL256X_REG_ID, &result);
	if (ret)
		return ret;

	switch ((result >> 0x04) & 0x0F) {
	case 5:
		name = "TAOS TSL2561";
		break;
	case 4:
		name = "TAOS TSL2560";
		break;
	case 3:
		name = "TAOS TSL2563";
		break;
	case 2:
		name = "TAOS TSL2562";
		break;
	case 1:
		type_cs = true;
		name = "TAOS TSL2561CS";
		break;
	case 0:
		type_cs = true;
		name = "TAOS TSL2560CS";
		break;
	default:
		unknown = true;
		break;
	}

	if (id)
		*id = result;
	if (cs)
		*cs = type_cs;
	if (model && name)
		strcpy(model, name);

	return unknown;
}

static int tsl256x_event_handler(void)
{
	unsigned int ch0, interr = 1;

	/* wait for the EC to clear the interrupt */
/*      schedule_timeout_interruptible(msecs_to_jiffies(100));	*/
	/* ...or force the interrupt clear immediately */
	sony_call_snc_handle(sony_als->handle, 0x04C60500, &interr);

	/* read the raw data */
	tsl256x_get_raw_data(&ch0, NULL);

	/* set the thresholds */
	tsl256x_set_thresholds(&ch0);

	/* enable interrupt */
	tsl256x_interrupt_ctrls(&interr, NULL);

	return 0;
}

static int tsl256x_init(const u8 defaults[])
{
	unsigned int id;
	int ret = 0;
	bool cs; /* if CS package choose CS coefficients */
	char model[64];

	/* detect the device */
	ret = tsl256x_get_id(model, &id, &cs);
	if (ret < 0)
		return ret;
	if (ret) {
		dprintk("unsupported ALS found (unknown model "
			"number %u rev. %u\n", id >> 4, id & 0x0F);
		return ret;
	} else {
		dprintk("found ALS model number %u rev. %u (%s)\n",
				id >> 4, id & 0x0F, model);
	}

	tsl256x_handle = kzalloc(sizeof(struct tsl256x_data), GFP_KERNEL);
	if (!tsl256x_handle)
		return -ENOMEM;

	tsl256x_handle->defaults = kzalloc(sizeof(u8) * 4, GFP_KERNEL);
	if (!tsl256x_handle->defaults) {
		kfree(tsl256x_handle);
		return -ENOMEM;
	}

	/* populate the device data */
	tsl256x_handle->defaults[0] = defaults[3];  /* low threshold % */
	tsl256x_handle->defaults[1] = defaults[4];  /* high threshold % */
	tsl256x_handle->defaults[2] = defaults[9];  /* sensor interrupt rate */
	tsl256x_handle->defaults[3] = defaults[10]; /* light compensat. rate */
	tsl256x_handle->gaintime = 0x12;
	tsl256x_handle->periods = defaults[9];
	tsl256x_handle->coeff_table = cs ? tsl256x_coeff_cs : tsl256x_coeff_fn;

	ret = tsl256x_setup();

	return ret;
}

static int tsl256x_exit(void)
{
	unsigned int interr = 0, periods = tsl256x_handle->defaults[2];

	/* disable the interrupt generation, restore defaults */
	tsl256x_interrupt_ctrls(&interr, &periods);

	tsl256x_handle->coeff_table = NULL;
	kfree(tsl256x_handle->defaults);
	tsl256x_handle->defaults = NULL;
	kfree(tsl256x_handle);

	return 0;
}

/* TAOS TSL256x specific ops */
static const struct als_device_ops tsl256x_ops = {
	.init = tsl256x_init,
	.exit = tsl256x_exit,
	.event_handler = tsl256x_event_handler,
	.set_power = tsl256x_set_power,
	.get_power = tsl256x_get_power,
	.get_lux = tsl256x_get_lux,
	.get_kelvin = tsl256x_get_kelvin,
};

/* unknown ALS sensors controlled by the EC present on newer Vaios */
static inline int ngals_get_raw_data(unsigned int *data)
{
	if (sony_call_snc_handle(sony_als->handle, 0x1000, data))
		return -EIO;

	return 0;
}

static int ngals_get_lux(unsigned int *integ, unsigned int *fract)
{
	unsigned int data;

	if (sony_call_snc_handle(sony_als->handle, 0x1000, &data))
		return -EIO;

	/* if we have a valid lux data */
	if (!!(data & 0xff0000) == 0x01) {
		*integ = 0xffff & data;
		*fract = 0;
	} else {
		return -1;
	}

	return 0;
}

static const struct als_device_ops ngals_ops = {
	.init = NULL,
	.exit = NULL,
	.event_handler = NULL,
	.set_power = NULL,
	.get_power = NULL,
	.get_lux = ngals_get_lux,
	.get_kelvin = NULL,
};

/*	ALS common data and functions	*/
static int sony_nc_als_event_handler(void)
{
	/* call the device handler */
	if (sony_als->ops->event_handler)
		sony_als->ops->event_handler();

	return 0;
}

static int sony_nc_als_power_set(unsigned int status)
{
	if (!sony_als->ops->set_power)
		return -EPERM;

	if (sony_als->ops->set_power(status))
		return -EIO;

	sony_als->power = status;

	return 0;
}

static int sony_nc_als_managed_set(unsigned int status)
{
	int ret = 0;
	unsigned int result, cmd;
	static bool was_on;

	/*  turn on/off the event notification
	 *  (and enable als_backlight writes)
	 */
	cmd = sony_als->handle == 0x0143 ? 0x2200 : 0x0900;
	if (sony_call_snc_handle(sony_als->handle,
		(status << 0x10) | cmd, &result))
		return -EIO;

	sony_als->managed = status;

	/* turn on the ALS; this will also enable the interrupt generation */
	if (status) /* store the power state else check the previous state */
		was_on = sony_als->power;
	else if (was_on)
		return 0;

	ret = sony_nc_als_power_set(status);
	if (ret == -EPERM) /* new models do not allow power control */
		ret = 0;

	return ret;
}

static unsigned int level;
static int sony_nc_als_get_brightness(struct backlight_device *bd)
{
	if (bd->props.brightness != level)
		dprintk("bd->props.brightness != level\n");

	return level;
}

static int sony_nc_als_update_status(struct backlight_device *bd)
{
	unsigned int value, result;

	if (sony_als->managed) {
		if (bd->props.brightness != level) {
			char *env[2] = { "ALS=2", NULL};
			kobject_uevent_env(&sony_nc_acpi_device->dev.kobj,
						KOBJ_CHANGE, env);

			dprintk("generating ALS event 3 (reason: 2)\n");
			acpi_bus_generate_proc_event(sony_nc_acpi_device,
					3, 2);
			acpi_bus_generate_netlink_event(
					sony_nc_acpi_device->pnp.device_class,
					dev_name(&sony_nc_acpi_device->dev),
					3, 2);
		}
	} else {
		unsigned int cmd;

		value = sony_als->levels[bd->props.brightness];
		cmd = sony_als->handle == 0x0143 ? 0x3000 : 0x0100;
		if (sony_call_snc_handle(sony_als->handle,
					(value << 0x10) | cmd, &result))
			return -EIO;
	}

	level = bd->props.brightness;

	return level;
}

/*	ALS sys interface	*/
static ssize_t sony_nc_als_power_show(struct device *dev,
		struct device_attribute *attr, char *buffer)
{
	ssize_t count = 0;
	int status;

	if (!sony_als->ops->get_power)
		return -EPERM;

	if (sony_als->ops->get_power(&status))
		return -EIO;

	count = snprintf(buffer, PAGE_SIZE, "%d\n", status);

	return count;
}

static ssize_t sony_nc_als_power_store(struct device *dev,
		struct device_attribute *attr,
		const char *buffer, size_t count)
{
	int ret;
	unsigned long value;

	if (count > 31)
		return -EINVAL;

	if (strict_strtoul(buffer, 10, &value) || value > 1)
		return -EINVAL;

	/* no action if already set */
	if (value == sony_als->power)
		return count;

	ret = sony_nc_als_power_set(value);
	if (ret)
		return ret;

	return count;
}

static ssize_t sony_nc_als_managed_show(struct device *dev,
		struct device_attribute *attr, char *buffer)
{
	ssize_t count = 0;
	unsigned int status, cmd;

	cmd = sony_als->handle == 0x0143 ? 0x2100 : 0x0A00;
	if (sony_call_snc_handle(sony_als->handle, cmd, &status))
		return -EIO;

	count = snprintf(buffer, PAGE_SIZE, "%d\n", status & 0x01);

	return count;
}

static ssize_t sony_nc_als_managed_store(struct device *dev,
		struct device_attribute *attr,
		const char *buffer, size_t count)
{
	unsigned long value;

	if (count > 31)
		return -EINVAL;

	if (strict_strtoul(buffer, 10, &value) || value > 1)
		return -EINVAL;

	if (sony_als->managed != value) {
		int ret = sony_nc_als_managed_set(value);
		if (ret)
			return ret;
	}

	return count;
}

static ssize_t sony_nc_als_lux_show(struct device *dev,
		struct device_attribute *attr, char *buffer)
{
	ssize_t count = 0;
	unsigned int integ = 0, fract = 0;

	if (sony_als->power)
		/* sony_als->ops->get_lux is mandatory, no check */
		sony_als->ops->get_lux(&integ, &fract);

	count = snprintf(buffer, PAGE_SIZE, "%u.%.2u\n", integ, fract);

	return count;
}

static ssize_t sony_nc_als_parameters_show(struct device *dev,
		struct device_attribute *attr, char *buffer)
{
	ssize_t count = 0;
	unsigned int i, num;
	u8 *list;

	if (!strcmp(attr->attr.name, "als_defaults")) {
		list = sony_als->defaults;
		num = sony_als->defaults_num;
	} else { /* als_backlight_levels */
		list = sony_als->levels;
		num = sony_als->levels_num;
	}

	for (i = 0; i < num; i++)
		count += snprintf(buffer + count, PAGE_SIZE - count,
				"0x%.2x ", list[i]);

	count += snprintf(buffer + count, PAGE_SIZE - count, "\n");

	return count;
}

static ssize_t sony_nc_als_backlight_show(struct device *dev,
		struct device_attribute *attr, char *buffer)
{
	ssize_t count = 0;
	unsigned int result, cmd;

	cmd = sony_als->handle == 0x0143 ? 0x3100 : 0x0200;
	if (sony_call_snc_handle(sony_als->handle, cmd, &result))
		return -EIO;

	count = snprintf(buffer, PAGE_SIZE, "%d\n", result & 0xff);

	return count;
}

static ssize_t sony_nc_als_backlight_store(struct device *dev,
		struct device_attribute *attr,
		const char *buffer, size_t count)
{
	unsigned long value;
	unsigned int result, cmd, max = sony_als->levels_num - 1;

	if (count > 31)
		return -EINVAL;

	if (strict_strtoul(buffer, 10, &value))
		return -EINVAL;

	if (!sony_als->managed)
		return -EPERM;

	/* verify that the provided value falls inside the model
	   specific backlight range */
	if ((value < sony_als->levels[0])
			|| (value > sony_als->levels[max]))
		return -EINVAL;

	cmd = sony_als->handle == 0x0143 ? 0x3000 : 0x0100;
	if (sony_call_snc_handle(sony_als->handle, (value << 0x10) | cmd,
				&result))
		return -EIO;

	return count;
}

static ssize_t sony_nc_als_kelvin_show(struct device *dev,
		struct device_attribute *attr, char *buffer)
{
	ssize_t count = 0;
	unsigned int kelvin = 0;

	if (sony_als->ops->get_kelvin && sony_als->power)
		sony_als->ops->get_kelvin(&kelvin);

	count = snprintf(buffer, PAGE_SIZE, "%d\n", kelvin);

	return count;
}

/*	ALS attach/detach functions	*/
static int sony_nc_als_setup(struct platform_device *pd, unsigned int handle)
{
	int i = 0;

	/* check the device presence */
	if (handle == 0x0137) {
		unsigned int result;

		if (sony_call_snc_handle(handle, 0xB00, &result))
			return -EIO;

		if (!(result & 0x01)) {
			pr_info("no ALS present\n");
			return 0;
		}
	}

	sony_als = kzalloc(sizeof(struct sony_als_device), GFP_KERNEL);
	if (!sony_als)
		return -ENOMEM;

	/* set model specific data */
	/* if handle 0x012f or 0x0137 use tsl256x_ops, else new als controls */
	if (handle == 0x0143) {
		sony_als->ops = &ngals_ops;
		sony_als->levels_num = 16;
		sony_als->defaults_num = 9;
	} else {
		sony_als->ops = &tsl256x_ops;
		sony_als->levels_num = 9;
		sony_als->defaults_num = 13;
	}
	/* backlight levels are the first levels_num values, the remaining
	   defaults_num values are default settings for als regulation
	*/
	sony_als->levels = sony_als->parameters;
	sony_als->defaults = sony_als->parameters + sony_als->levels_num;

	sony_als->handle = handle;

	/* get power state */
	if (sony_als->ops->get_power) {
		if (sony_als->ops->get_power(&sony_als->power))
			pr_warn("unable to retrieve the power status\n");
	}

	/* set managed to 0, userspace daemon should enable it */
	sony_nc_als_managed_set(0);

	/* get ALS parameters */
	if (sony_call_snc_handle_buffer(sony_als->handle, 0x0000,
		sony_als->parameters, ALS_TABLE_SIZE) < 0)
		goto nosensor;

	/* initial device configuration */
	if (sony_als->ops->init)
		if (sony_als->ops->init(sony_als->defaults)) {
			pr_warn("ALS setup failed\n");
			goto nosensor;
		}

	/* set up the sys interface */

	/* notifications and backlight enable control file */
	sysfs_attr_init(&sony_als->attrs[0].attr);
	sony_als->attrs[0].attr.name = "als_managed";
	sony_als->attrs[0].attr.mode = S_IRUGO | S_IWUSR;
	sony_als->attrs[0].show = sony_nc_als_managed_show;
	sony_als->attrs[0].store = sony_nc_als_managed_store;
	/* lux equivalent value */
	sysfs_attr_init(&sony_als->attrs[1].attr);
	sony_als->attrs[1].attr.name = "als_lux";
	sony_als->attrs[1].attr.mode = S_IRUGO;
	sony_als->attrs[1].show = sony_nc_als_lux_show;
	/* ALS default parameters */
	sysfs_attr_init(&sony_als->attrs[2].attr);
	sony_als->attrs[2].attr.name = "als_defaults";
	sony_als->attrs[2].attr.mode = S_IRUGO;
	sony_als->attrs[2].show = sony_nc_als_parameters_show;
	/* ALS default backlight levels */
	sysfs_attr_init(&sony_als->attrs[3].attr);
	sony_als->attrs[3].attr.name = "als_backlight_levels";
	sony_als->attrs[3].attr.mode = S_IRUGO;
	sony_als->attrs[3].show = sony_nc_als_parameters_show;
	/* als backlight control */
	sysfs_attr_init(&sony_als->attrs[4].attr);
	sony_als->attrs[4].attr.name = "als_backlight";
	sony_als->attrs[4].attr.mode = S_IRUGO | S_IWUSR;
	sony_als->attrs[4].show = sony_nc_als_backlight_show;
	sony_als->attrs[4].store = sony_nc_als_backlight_store;

	sony_als->attrs_num = 5;
	/* end mandatory sys interface */

	if (sony_als->ops->get_power || sony_als->ops->set_power) {
		int i = sony_als->attrs_num++;

		/* als power control */
		sysfs_attr_init(&sony_als->attrs[i].attr);
		sony_als->attrs[i].attr.name = "als_power";
		sony_als->attrs[i].attr.mode = S_IRUGO | S_IWUSR;
		sony_als->attrs[i].show = sony_nc_als_power_show;
		sony_als->attrs[i].store = sony_nc_als_power_store;
	}

	if (sony_als->ops->get_kelvin) {
		int i = sony_als->attrs_num++;

		/* light temperature */
		sysfs_attr_init(&sony_als->attrs[i].attr);
		sony_als->attrs[i].attr.name = "als_kelvin";
		sony_als->attrs[i].attr.mode = S_IRUGO;
		sony_als->attrs[i].show = sony_nc_als_kelvin_show;
	}

	/* everything or nothing, otherwise unable to control the ALS */
	for (; i < sony_als->attrs_num; i++) {
		if (device_create_file(&pd->dev, &sony_als->attrs[i]))
			goto attrserror;
	}

	return 0;

attrserror:
	for (; i > 0; i--)
		device_remove_file(&pd->dev, &sony_als->attrs[i]);
nosensor:
	kfree(sony_als);
	sony_als = NULL;

	return -1;
}

static void sony_nc_als_resume(void)
{
	if (sony_als->managed) /* it restores the power state too */
		sony_nc_als_managed_set(1);
	else if (sony_als->power)
		sony_nc_als_power_set(1);
}

static int sony_nc_als_cleanup(struct platform_device *pd)
{
	if (sony_als) {
		int i;

		for (i = 0; i < sony_als->attrs_num; i++)
			device_remove_file(&pd->dev, &sony_als->attrs[i]);

		/* disable the events notification */
		if (sony_als->managed)
			if (sony_nc_als_managed_set(0))
				pr_info("ALS notifications disable failed\n");

		if (sony_als->power)
			if (sony_nc_als_power_set(0))
				pr_info("ALS power off failed\n");

		if (sony_als->ops->exit)
			if (sony_als->ops->exit())
				pr_info("ALS device cleaning failed\n");

		kfree(sony_als);
		sony_als = NULL;
	}

	return 0;
}
/*	end ALS code	*/

/* Keyboard backlight feature */
static struct sony_kbdbl_data {
	unsigned int handle;
	unsigned int base;
	unsigned int mode;
	unsigned int timeout;
	struct device_attribute mode_attr;
	struct device_attribute timeout_attr;
} *sony_kbdbl;

static int __sony_nc_kbd_backlight_mode_set(u8 value)
{
	unsigned int result;

	if (value > 1)
		return -EINVAL;

	if (sony_call_snc_handle(sony_kbdbl->handle, (value << 0x10) |
				(sony_kbdbl->base), &result))
		return -EIO;

	sony_kbdbl->mode = value;

	/* Try to turn the light on/off immediately */
	sony_call_snc_handle(sony_kbdbl->handle, (value << 0x10) |
				(sony_kbdbl->base + 0x100), &result);

	return 0;
}

static ssize_t sony_nc_kbd_backlight_mode_store(struct device *dev,
		struct device_attribute *attr,
		const char *buffer, size_t count)
{
	int ret = 0;
	unsigned long value;

	if (count > 31)
		return -EINVAL;

	if (strict_strtoul(buffer, 10, &value))
		return -EINVAL;

	ret = __sony_nc_kbd_backlight_mode_set(value);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t sony_nc_kbd_backlight_mode_show(struct device *dev,
		struct device_attribute *attr, char *buffer)
{
	ssize_t count = 0;

	count = snprintf(buffer, PAGE_SIZE, "%d\n", sony_kbdbl->mode);

	return count;
}

static int __sony_nc_kbd_backlight_timeout_set(u8 value)
{
	unsigned int result;

	if (value > 3)
		return -EINVAL;

	if (sony_call_snc_handle(sony_kbdbl->handle, (value << 0x10) |
				(sony_kbdbl->base + 0x200), &result))
		return -EIO;

	sony_kbdbl->timeout = value;

	return 0;
}

static ssize_t sony_nc_kbd_backlight_timeout_store(struct device *dev,
		struct device_attribute *attr,
		const char *buffer, size_t count)
{
	int ret = 0;
	unsigned long value;

	if (count > 31)
		return -EINVAL;

	if (strict_strtoul(buffer, 10, &value))
		return -EINVAL;

	ret = __sony_nc_kbd_backlight_timeout_set(value);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t sony_nc_kbd_backlight_timeout_show(struct device *dev,
		struct device_attribute *attr, char *buffer)
{
	ssize_t count = 0;

	count = snprintf(buffer, PAGE_SIZE, "%d\n", sony_kbdbl->timeout);

	return count;
}

static int sony_nc_kbd_backlight_setup(struct platform_device *pd,
					unsigned int handle)
{
	unsigned int result, base_cmd;
	bool found = false;

	/* verify the kbd backlight presence, some models do not have it */
	if (handle == 0x0137) {
		if (sony_call_snc_handle(handle, 0x0B00, &result))
			return -EIO;

		found = !!(result & 0x02);
		base_cmd = 0x0C00;
	} else {
		if (sony_call_snc_handle(handle, 0x0100, &result))
			return -EIO;

		found = result & 0x01;
		base_cmd = 0x4000;
	}

	if (!found) {
		dprintk("no backlight keyboard found\n");
		return 0;
	}

	sony_kbdbl = kzalloc(sizeof(*sony_kbdbl), GFP_KERNEL);
	if (!sony_kbdbl)
		return -ENOMEM;

	sysfs_attr_init(&sony_kbdbl->mode_attr.attr);
	sony_kbdbl->mode_attr.attr.name = "kbd_backlight";
	sony_kbdbl->mode_attr.attr.mode = S_IRUGO | S_IWUSR;
	sony_kbdbl->mode_attr.show = sony_nc_kbd_backlight_mode_show;
	sony_kbdbl->mode_attr.store = sony_nc_kbd_backlight_mode_store;

	sysfs_attr_init(&sony_kbdbl->timeout_attr.attr);
	sony_kbdbl->timeout_attr.attr.name = "kbd_backlight_timeout";
	sony_kbdbl->timeout_attr.attr.mode = S_IRUGO | S_IWUSR;
	sony_kbdbl->timeout_attr.show = sony_nc_kbd_backlight_timeout_show;
	sony_kbdbl->timeout_attr.store = sony_nc_kbd_backlight_timeout_store;

	if (device_create_file(&pd->dev, &sony_kbdbl->mode_attr))
		goto outkzalloc;

	if (device_create_file(&pd->dev, &sony_kbdbl->timeout_attr))
		goto outmode;

	sony_kbdbl->handle = handle;
	sony_kbdbl->base = base_cmd;

	__sony_nc_kbd_backlight_mode_set(kbd_backlight);
	__sony_nc_kbd_backlight_timeout_set(kbd_backlight_timeout);

	return 0;

outmode:
	device_remove_file(&pd->dev, &sony_kbdbl->mode_attr);
outkzalloc:
	kfree(sony_kbdbl);
	sony_kbdbl = NULL;
	return -1;
}

static int sony_nc_kbd_backlight_cleanup(struct platform_device *pd)
{
	if (sony_kbdbl) {
		unsigned int result;

		device_remove_file(&pd->dev, &sony_kbdbl->mode_attr);
		device_remove_file(&pd->dev, &sony_kbdbl->timeout_attr);

		/* restore the default hw behaviour */
		sony_call_snc_handle(sony_kbdbl->handle,
				sony_kbdbl->base | 0x10000, &result);
		sony_call_snc_handle(sony_kbdbl->handle,
				sony_kbdbl->base + 0x200, &result);

		kfree(sony_kbdbl);
		sony_kbdbl = NULL;
	}
	return 0;
}

static void sony_nc_kbd_backlight_resume(void)
{
	unsigned int result;

	if (!sony_kbdbl)
		return;

	if (sony_kbdbl->mode == 0)
		sony_call_snc_handle(sony_kbdbl->handle,
				sony_kbdbl->base, &result);

	if (sony_kbdbl->timeout != 0)
		sony_call_snc_handle(sony_kbdbl->handle,
				(sony_kbdbl->base + 0x200) |
				(sony_kbdbl->timeout << 0x10), &result);
}

/*	GSensor, HDD Shock Protection	*/
enum axis {
	X_AXIS = 4,	/* frontal  */
	Y_AXIS,		/* lateral  */
	Z_AXIS		/* vertical */
};

static struct sony_gsensor_device {
	unsigned int handle;
	unsigned int attrs_num;
	struct device_attribute *attrs;
} *sony_gsensor;

/* the EC uses pin #11 of the SATA power connector to command the
   immediate idle feature; however some drives do not implement it
   and pin #11 is NC. Let's verify, otherwise no automatic
   protection is possible by the hardware
*/
static int sony_nc_gsensor_support_get(unsigned int *support)
{
	unsigned int result;

	if (sony_call_snc_handle(sony_gsensor->handle, 0x0200, &result))
		return -EIO;

	*support = sony_gsensor->handle == 0x0134
			? !!(result & 0x20)
			: !!(result & 0x01);

	return 0;
}

static int sony_nc_gsensor_status_set(int value)
{
	unsigned int result, capable, reg, arg;
	bool update = false;

	if (sony_nc_gsensor_support_get(&capable))
		return -EIO;

	if (!capable)
		pr_warn("hardware protection not available, the HDD"
			       " do not support this feature\n");

	/* do not return immediately even though there is no HW
	 * capability, userspace can thus receive the shock
	 * notifications and call the ATA7 immediate idle command to
	 * unload the heads. Just return after enabling notifications
	*/
	reg = sony_gsensor->handle == 0x0134 ?
		(!value << 0x08) : (value << 0x10);

	if (sony_call_snc_handle(sony_gsensor->handle, reg, &result))
		return -EIO;

	if (!capable)
		return 0;

	/* if the requested protection setting is different
	   from the current one
	*/
	reg = sony_gsensor->handle == 0x0134 ? 0x0200 : 0x0400;
	if (sony_call_snc_handle(sony_gsensor->handle, reg, &result))
		return -EIO;

	if (sony_gsensor->handle == 0x0134) {
		if (!!(result & 0x04) != value) {
			arg = (result & 0x1B) | (value << 0x02);
			update = true;
		}
	} else {
		if ((result & 0x01) != value) {
			arg = value;
			update = true;
		}
	}

	if (update && sony_call_snc_handle(sony_gsensor->handle,
			(arg << 0x10) | 0x0300, &result))
		return -EIO;

	return 0;
}

static int sony_nc_gsensor_axis_get(enum axis name)
{
	unsigned int result;

	if (sony_call_snc_handle(sony_gsensor->handle, name << 0x08, &result))
		return -EIO;

	return result;
}

/*			G sensor sys interface			*/
static ssize_t sony_nc_gsensor_type_show(struct device *dev,
		struct device_attribute *attr, char *buffer)
{
	ssize_t count = 0;
	unsigned int result;

	if (sony_call_snc_handle(sony_gsensor->handle, 0x0200, &result))
		return -EIO;

	count = snprintf(buffer, PAGE_SIZE, "%d\n", (result >> 0x03) & 0x03);

	return count;
}

static ssize_t sony_nc_gsensor_type_store(struct device *dev,
		struct device_attribute *attr,
		const char *buffer, size_t count)
{
	/*
	 *  axis out type control file:
	 *  0: raw values, 1: acc values 2: threshold values
	 */
	unsigned int result;
	unsigned long value;

	/* sanity checks and conversion */
	if (count > 31 || strict_strtoul(buffer, 10, &value) || value > 2)
		return -EINVAL;

	value <<= 0x03;

	/* retrieve the current state / settings */
	if (sony_call_snc_handle(sony_gsensor->handle, 0x0200, &result))
		return -EIO;

	if ((result & 0x18) != value) {
		/* the last 3 bits need to be preserved */
		value |= (result & 0x07);

		if (sony_call_snc_handle(sony_gsensor->handle,
				(value << 0x10) | 0x0300, &result))
				return -EIO;
	}

	return count;
}

static ssize_t sony_nc_gsensor_axis_show(struct device *dev,
		struct device_attribute *attr, char *buffer)
{
	ssize_t count = 0;
	unsigned int result;
	enum axis arg;

	/* file being read for axis selection */
	if (!strcmp(attr->attr.name, "gsensor_xval"))
		arg = X_AXIS;
	else if (!strcmp(attr->attr.name, "gsensor_yval"))
		arg = Y_AXIS;
	else if (!strcmp(attr->attr.name, "gsensor_zval"))
		arg = Z_AXIS;
	else
		return count;

	result = sony_nc_gsensor_axis_get(arg);
	if (result < 0)
		return -EIO;

	count = snprintf(buffer, PAGE_SIZE, "%d\n", result);

	return count;
}

static ssize_t sony_nc_gsensor_status_show(struct device *dev,
		struct device_attribute *attr, char *buffer)
{
	ssize_t count = 0;
	unsigned int result;

	if (sony_gsensor->handle == 0x0134) {
		if (sony_call_snc_handle(sony_gsensor->handle, 0x0200,
					&result))
			return -EIO;

		result = !!(result & 0x04);
	} else {
		if (sony_call_snc_handle(sony_gsensor->handle, 0x0400,
					&result))
			return -EIO;

		result &= 0x01;
	}

	count = snprintf(buffer, PAGE_SIZE, "%d\n", result);

	return count;
}

static ssize_t sony_nc_gsensor_status_store(struct device *dev,
		struct device_attribute *attr,
		const char *buffer, size_t count)
{
	int ret;
	unsigned long value;

	if (count > 31)
		return -EINVAL;
	if (strict_strtoul(buffer, 10, &value) || value > 1)
		return -EINVAL;

	ret = sony_nc_gsensor_status_set(value);
	if (ret)
		return ret;

	return count;
}

static ssize_t sony_nc_gsensor_sensitivity_show(struct device *dev,
		struct device_attribute *attr, char *buffer)
{
	ssize_t count = 0;
	unsigned int result;

	if (sony_call_snc_handle(sony_gsensor->handle, 0x0200, &result))
		return -EINVAL;

	count = snprintf(buffer, PAGE_SIZE, "%d\n", result & 0x03);
	return count;
}

static ssize_t sony_nc_gsensor_sensitivity_store(struct device *dev,
		struct device_attribute *attr,
		const char *buffer, size_t count)
{
	unsigned int result;
	unsigned long value;

	if (count > 31)
		return -EINVAL;
	if (strict_strtoul(buffer, 10, &value) || value > 2)
		return -EINVAL;

	/* retrieve the other parameters to be stored as well */
	if (sony_call_snc_handle(sony_gsensor->handle, 0x0200, &result))
		return -EIO;
	value |= (result & 0x1C); /* preserve only the needed bits */

	if (sony_call_snc_handle(sony_gsensor->handle, (value << 0x10)
		| 0x0300, &result))
		return -EIO;

	return count;
}

static int sony_nc_gsensor_setup(struct platform_device *pd,
					unsigned int handle)
{
	int i, enable, support;

	sony_gsensor = kzalloc(sizeof(struct sony_gsensor_device), GFP_KERNEL);
	if (!sony_gsensor)
		return -ENOMEM;

	sony_gsensor->handle = handle;
	sony_gsensor->attrs_num = handle == 0x0134 ? 6 : 1;

	sony_gsensor->attrs = kzalloc(sizeof(struct device_attribute)
				* sony_gsensor->attrs_num, GFP_KERNEL);
	if (!sony_gsensor->attrs)
		goto memerror;

	/* check the storing device support */
	if (sony_nc_gsensor_support_get(&support))
		return -EIO;

	/* enable the HDD protection and notification by default
	   when hardware driven protection is possible */
	enable = support ? 1 : force_shock_notifications;
	if (sony_nc_gsensor_status_set(enable))
		if (enable)
			pr_warn("failed to enable the HDD shock protection\n");

	/* activation control	*/
	sysfs_attr_init(&sony_gsensor->attrs[0].attr);
	sony_gsensor->attrs[0].attr.name = "gsensor_protection";
	sony_gsensor->attrs[0].attr.mode = S_IRUGO | S_IWUSR;
	sony_gsensor->attrs[0].show = sony_nc_gsensor_status_show;
	sony_gsensor->attrs[0].store = sony_nc_gsensor_status_store;

	if (sony_gsensor->attrs_num > 1) {
		/* sensitivity selection */
		sysfs_attr_init(&sony_gsensor->attrs[1].attr);
		sony_gsensor->attrs[1].attr.name = "gsensor_sensitivity";
		sony_gsensor->attrs[1].attr.mode = S_IRUGO | S_IWUSR;
		sony_gsensor->attrs[1].show = sony_nc_gsensor_sensitivity_show;
		sony_gsensor->attrs[1].store =
					sony_nc_gsensor_sensitivity_store;
		/* x/y/z output selection */
		sysfs_attr_init(&sony_gsensor->attrs[2].attr);
		sony_gsensor->attrs[2].attr.name = "gsensor_val_type";
		sony_gsensor->attrs[2].attr.mode = S_IRUGO | S_IWUSR;
		sony_gsensor->attrs[2].show = sony_nc_gsensor_type_show;
		sony_gsensor->attrs[2].store = sony_nc_gsensor_type_store;

		sysfs_attr_init(&sony_gsensor->attrs[3].attr);
		sony_gsensor->attrs[3].attr.name = "gsensor_xval";
		sony_gsensor->attrs[3].attr.mode = S_IRUGO;
		sony_gsensor->attrs[3].show = sony_nc_gsensor_axis_show;

		sysfs_attr_init(&sony_gsensor->attrs[4].attr);
		sony_gsensor->attrs[4].attr.name = "gsensor_yval";
		sony_gsensor->attrs[4].attr.mode = S_IRUGO;
		sony_gsensor->attrs[4].show = sony_nc_gsensor_axis_show;

		sysfs_attr_init(&sony_gsensor->attrs[5].attr);
		sony_gsensor->attrs[5].attr.name = "gsensor_zval";
		sony_gsensor->attrs[5].attr.mode = S_IRUGO;
		sony_gsensor->attrs[5].show = sony_nc_gsensor_axis_show;
	}

	for (i = 0; i < sony_gsensor->attrs_num; i++) {
		if (device_create_file(&pd->dev, &sony_gsensor->attrs[i]))
			goto attrserror;
	}

	return 0;

attrserror:
	for (; i > 0; i--)
		device_remove_file(&pd->dev, &sony_gsensor->attrs[i]);

	kfree(sony_gsensor->attrs);
memerror:
	kfree(sony_gsensor);
	sony_gsensor = NULL;

	return -1;
}

static int sony_nc_gsensor_cleanup(struct platform_device *pd)
{
	if (sony_gsensor) {
		unsigned int i, result, reg;

		for (i = 0; i < sony_gsensor->attrs_num; i++)
			device_remove_file(&pd->dev, &sony_gsensor->attrs[i]);

		/* disable the event generation,
		 * preserve any other setting
		 */
		reg = sony_gsensor->handle == 0x0134 ? 0x0100 : 0x0000;

		sony_call_snc_handle(sony_gsensor->handle, reg, &result);

		kfree(sony_gsensor->attrs);
		kfree(sony_gsensor);
		sony_gsensor = NULL;
	}

	return 0;
}
/*			end G sensor code			*/

static struct sony_battcare_data {
	unsigned int handle;
	struct device_attribute attrs[2];
} *sony_battcare;

static ssize_t sony_nc_battery_care_limit_store(struct device *dev,
		struct device_attribute *attr,
		const char *buffer, size_t count)
{
	unsigned int result, cmd;
	unsigned long value;

	if (count > 31)
		return -EINVAL;
	if (strict_strtoul(buffer, 10, &value))
		return -EINVAL;

	/*  limit values (2 bits):
	 *  00 - none
	 *  01 - 80%
	 *  10 - 50%
	 *  11 - 100%
	 *
	 *  bit 0: 0 disable BCL, 1 enable BCL
	 *  bit 1: 1 tell to store the battery limit (see bits 6,7) too
	 *  bits 2,3: reserved
	 *  bits 4,5: store the limit into the EC
	 *  bits 6,7: store the limit into the battery
	 */

	/*
	 * handle 0x0115 should allow storing on battery too;
	 * handle 0x0136 same as 0x0115 + health status;
	 * handle 0x013f, same as 0x0136 but no storing on the battery
	 *
	 * Store only inside the EC for now, regardless the handle number
	 */
	switch (value) {
	case 0:	/* disable */
		cmd = 0x00;
		break;
	case 1: /* enable, 80% charge limit */
		cmd = 0x11;
		break;
	case 2: /* enable, 50% charge limit */
		cmd = 0x21;
		break;
	default:
		return -EINVAL;
	}

	if (sony_call_snc_handle(sony_battcare->handle, (cmd << 0x10) | 0x0100,
				&result))
		return -EIO;

	return count;
}

static ssize_t sony_nc_battery_care_limit_show(struct device *dev,
		struct device_attribute *attr, char *buffer)
{
	ssize_t count = 0;
	unsigned int result, status;

	if (sony_call_snc_handle(sony_battcare->handle, 0x0000, &result))
		return -EIO;

	/* if disabled 0, else take the limit bits */
	status = !(result & 0x01) ? 0 : ((result & 0x30) >> 0x04);

	count = snprintf(buffer, PAGE_SIZE, "%d\n", status);
	return count;
}

static ssize_t sony_nc_battery_care_health_show(struct device *dev,
		struct device_attribute *attr, char *buffer)
{
	ssize_t count = 0;
	unsigned int health;

	if (sony_call_snc_handle(sony_battcare->handle, 0x0200, &health))
		return -EIO;

	count = snprintf(buffer, PAGE_SIZE, "%d\n", health & 0xff);

	return count;
}

static int sony_nc_battery_care_setup(struct platform_device *pd,
					unsigned int handle)
{
	sony_battcare = kzalloc(sizeof(struct sony_battcare_data), GFP_KERNEL);
	if (!sony_battcare)
		return -ENOMEM;

	sony_battcare->handle = handle;

	sysfs_attr_init(&sony_battcare->attrs[0].attr);
	sony_battcare->attrs[0].attr.name = "battery_care_limiter";
	sony_battcare->attrs[0].attr.mode = S_IRUGO | S_IWUSR;
	sony_battcare->attrs[0].show = sony_nc_battery_care_limit_show;
	sony_battcare->attrs[0].store = sony_nc_battery_care_limit_store;

	if (device_create_file(&pd->dev, &sony_battcare->attrs[0]))
		goto outkzalloc;

	if (handle == 0x0115) /* no health indication */
		return 0;

	sysfs_attr_init(&sony_battcare->attrs[1].attr);
	sony_battcare->attrs[1].attr.name = "battery_care_health";
	sony_battcare->attrs[1].attr.mode = S_IRUGO;
	sony_battcare->attrs[1].show = sony_nc_battery_care_health_show;

	if (device_create_file(&pd->dev, &sony_battcare->attrs[1]))
		goto outlimiter;

	return 0;

outlimiter:
	device_remove_file(&pd->dev, &sony_battcare->attrs[0]);
outkzalloc:
	kfree(sony_battcare);
	sony_battcare = NULL;

	return -1;
}

static int sony_nc_battery_care_cleanup(struct platform_device *pd)
{
	if (sony_battcare) {
		device_remove_file(&pd->dev, &sony_battcare->attrs[0]);
		if (sony_battcare->handle != 0x0115)
			device_remove_file(&pd->dev, &sony_battcare->attrs[1]);

		kfree(sony_battcare);
		sony_battcare = NULL;
	}

	return 0;
}

static struct sony_thermal_data {
	unsigned int mode;
	unsigned int profiles;
	struct device_attribute mode_attr;
	struct device_attribute profiles_attr;
} *sony_thermal;

static int sony_nc_thermal_mode_set(unsigned int profile)
{
	unsigned int cmd, result;

	/* to avoid the 1 value hole when only 2 profiles are available */
	switch (profile) {
	case 1: /* performance */
		cmd = 2;
		break;
	case 2: /* silent */
		cmd = 1;
		break;
	default: /* balanced */
		cmd = 0;
		break;
	}

	if (sony_call_snc_handle(0x0122, cmd << 0x10 | 0x0200, &result))
		return -EIO;

	sony_thermal->mode = profile;

	return 0;
}

static int sony_nc_thermal_mode_get(unsigned int *profile)
{
	unsigned int result;

	if (sony_call_snc_handle(0x0122, 0x0100, &result))
		return -EIO;

	/* to avoid the 1 value hole when only 2 profiles are available */
	switch (result & 0xff) {
	case 2: /* performance */
		*profile = 1;
		break;
	case 1: /* silent */
		*profile = 2;
		break;
	default: /* balanced */
		*profile = 0;
		break;
	}

	return 0;
}

static ssize_t sony_nc_thermal_profiles_show(struct device *dev,
		struct device_attribute *attr, char *buffer)
{
	return snprintf(buffer, PAGE_SIZE, "%u\n", sony_thermal->profiles);
}

static ssize_t sony_nc_thermal_mode_store(struct device *dev,
		struct device_attribute *attr,
		const char *buffer, size_t count)
{
	unsigned long value;

	if (count > 31)
		return -EINVAL;
	if (strict_strtoul(buffer, 10, &value) ||
		value > (sony_thermal->profiles - 1))
		return -EINVAL;

	if (sony_nc_thermal_mode_set(value))
		return -EIO;

	return count;
}

static ssize_t sony_nc_thermal_mode_show(struct device *dev,
		struct device_attribute *attr, char *buffer)
{
	ssize_t count = 0;
	unsigned int profile;

	if (sony_nc_thermal_mode_get(&profile))
		return -EIO;

	count = snprintf(buffer, PAGE_SIZE, "%d\n", profile);

	return count;
}

static int sony_nc_thermal_setup(struct platform_device *pd)
{
	sony_thermal = kzalloc(sizeof(struct sony_thermal_data), GFP_KERNEL);
	if (!sony_thermal)
		return -ENOMEM;

	if (sony_call_snc_handle(0x0122, 0x0000, &sony_thermal->profiles)) {
		pr_warn("unable to retrieve the available profiles\n");
		goto outkzalloc;
	}

	if (sony_nc_thermal_mode_get(&sony_thermal->mode)) {
		pr_warn("unable to retrieve the current profile");
		goto outkzalloc;
	}

	sysfs_attr_init(&sony_thermal->profiles_attr.attr);
	sony_thermal->profiles_attr.attr.name = "thermal_profiles";
	sony_thermal->profiles_attr.attr.mode = S_IRUGO;
	sony_thermal->profiles_attr.show = sony_nc_thermal_profiles_show;

	sysfs_attr_init(&sony_thermal->mode_attr.attr);
	sony_thermal->mode_attr.attr.name = "thermal_control";
	sony_thermal->mode_attr.attr.mode = S_IRUGO | S_IWUSR;
	sony_thermal->mode_attr.show = sony_nc_thermal_mode_show;
	sony_thermal->mode_attr.store = sony_nc_thermal_mode_store;

	if (device_create_file(&pd->dev, &sony_thermal->profiles_attr))
		goto outkzalloc;

	if (device_create_file(&pd->dev, &sony_thermal->mode_attr))
		goto outprofiles;

	return 0;

outprofiles:
	device_remove_file(&pd->dev, &sony_thermal->profiles_attr);
outkzalloc:
	kfree(sony_thermal);
	sony_thermal = NULL;
	return -1;
}

static int sony_nc_thermal_cleanup(struct platform_device *pd)
{
	if (sony_thermal) {
		device_remove_file(&pd->dev, &sony_thermal->profiles_attr);
		device_remove_file(&pd->dev, &sony_thermal->mode_attr);
		kfree(sony_thermal);
		sony_thermal = NULL;
	}

	return 0;
}

static void sony_nc_thermal_resume(void)
{
	unsigned int status;

	sony_nc_thermal_mode_get(&status);

	if (status != sony_thermal->mode)
		sony_nc_thermal_mode_set(sony_thermal->mode);
}

static struct device_attribute *sony_lid;

static ssize_t sony_nc_lid_resume_store(struct device *dev,
		struct device_attribute *attr,
		const char *buffer, size_t count)
{
	unsigned int result;
	unsigned long value;

	if (count > 31)
		return -EINVAL;
	if (strict_strtoul(buffer, 10, &value) || value > 3)
		return -EINVAL;

	/* 00 <- disabled
	   01 <- resume from S4
	   10 <- resume from S3
	   11 <- resume from S4 and S3
	*/
	/* we must set bit 1 and 2 (bit 0 is for S5), so shift one bit more */
	if (sony_call_snc_handle(0x0119, value << 0x11 | 0x0100, &result))
		return -EIO;

	return count;
}

static ssize_t sony_nc_lid_resume_show(struct device *dev,
		struct device_attribute *attr, char *buffer)
{
	ssize_t count = 0;
	unsigned int result;

	if (sony_call_snc_handle(0x0119, 0x0000, &result))
		return -EIO;

	count = snprintf(buffer, PAGE_SIZE, "%d\n", (result >> 1) & 0x03);

	return count;
}

static int sony_nc_lid_resume_setup(struct platform_device *pd)
{
	sony_lid = kzalloc(sizeof(struct device_attribute), GFP_KERNEL);
	if (!sony_lid)
		return -ENOMEM;

	sysfs_attr_init(&sony_lid->attr);
	sony_lid->attr.name = "lid_resume_control";
	sony_lid->attr.mode = S_IRUGO | S_IWUSR;
	sony_lid->show = sony_nc_lid_resume_show;
	sony_lid->store = sony_nc_lid_resume_store;

	if (device_create_file(&pd->dev, sony_lid)) {
		kfree(sony_lid);
		sony_lid = NULL;
		return -1;
	}

	return 0;
}

static int sony_nc_lid_resume_cleanup(struct platform_device *pd)
{
	if (sony_lid) {
		device_remove_file(&pd->dev, sony_lid);
		kfree(sony_lid);
		sony_lid = NULL;
	}

	return 0;
}

static struct device_attribute *sony_hsc;

static ssize_t sony_nc_highspeed_charging_store(struct device *dev,
		struct device_attribute *attr,
		const char *buffer, size_t count)
{
	unsigned int result;
	unsigned long value;

	if (count > 31)
		return -EINVAL;
	if (strict_strtoul(buffer, 10, &value) || value > 1)
		return -EINVAL;

	if (sony_call_snc_handle(0x0131, value << 0x10 | 0x0200, &result))
		return -EIO;

	return count;
}

static ssize_t sony_nc_highspeed_charging_show(struct device *dev,
		struct device_attribute *attr, char *buffer)
{
	ssize_t count = 0;
	unsigned int result;

	if (sony_call_snc_handle(0x0131, 0x0100, &result))
		return -EIO;

	count = snprintf(buffer, PAGE_SIZE, "%d\n", result & 0x01);

	return count;
}

static int sony_nc_highspeed_charging_setup(struct platform_device *pd)
{
	unsigned int result;

	if (sony_call_snc_handle(0x0131, 0x0000, &result) || !(result & 0x01)) {
		pr_info("no High Speed Charging capability found\n");
		return 0;
	}

	sony_hsc = kzalloc(sizeof(struct device_attribute), GFP_KERNEL);
	if (!sony_hsc)
		return -ENOMEM;

	sysfs_attr_init(&sony_hsc->attr);
	sony_hsc->attr.name = "battery_highspeed_charging";
	sony_hsc->attr.mode = S_IRUGO | S_IWUSR;
	sony_hsc->show = sony_nc_highspeed_charging_show;
	sony_hsc->store = sony_nc_highspeed_charging_store;

	if (device_create_file(&pd->dev, sony_hsc)) {
		kfree(sony_hsc);
		sony_hsc = NULL;
		return -1;
	}

	return 0;
}

static int sony_nc_highspeed_charging_cleanup(struct platform_device *pd)
{
	if (sony_hsc) {
		device_remove_file(&pd->dev, sony_hsc);
		kfree(sony_hsc);
		sony_hsc = NULL;
	}

	return 0;
}

static struct sony_tpad_device {
	unsigned int handle;
	struct device_attribute attr;
} *sony_tpad;

static ssize_t sony_nc_touchpad_store(struct device *dev,
		struct device_attribute *attr,
		const char *buffer, size_t count)
{
	unsigned int result;
	unsigned long value;

	if (count > 31)
		return -EINVAL;
	if (strict_strtoul(buffer, 10, &value) || value > 1)
		return -EINVAL;

	/* sysfs: 0 disabled, 1 enabled; EC: 0 enabled, 1 disabled */
	if (sony_call_snc_handle(sony_tpad->handle,
				(!value << 0x10) | 0x100, &result))
		return -EIO;

	return count;
}

static ssize_t sony_nc_touchpad_show(struct device *dev,
		struct device_attribute *attr, char *buffer)
{
	ssize_t count = 0;
	unsigned int result;

	if (sony_call_snc_handle(sony_tpad->handle, 0x000, &result))
		return -EINVAL;

	/* 1 tpad off, 0 tpad on */
	count = snprintf(buffer, PAGE_SIZE, "%d\n", !(result & 0x01));
	return count;
}

static int sony_nc_touchpad_setup(struct platform_device *pd,
					unsigned int handle)
{
	sony_tpad = kzalloc(sizeof(struct sony_tpad_device), GFP_KERNEL);
	if (!sony_tpad)
		return -ENOMEM;

	sony_tpad->handle = handle;

	sysfs_attr_init(&sony_tpad->attr.attr);
	sony_tpad->attr.attr.name = "touchpad";
	sony_tpad->attr.attr.mode = S_IRUGO | S_IWUSR;
	sony_tpad->attr.show = sony_nc_touchpad_show;
	sony_tpad->attr.store = sony_nc_touchpad_store;

	if (device_create_file(&pd->dev, &sony_tpad->attr)) {
		kfree(sony_tpad);
		sony_tpad = NULL;
		return -1;
	}

	return 0;
}

static int sony_nc_touchpad_cleanup(struct platform_device *pd)
{
	if (sony_tpad) {
		device_remove_file(&pd->dev, &sony_tpad->attr);
		kfree(sony_tpad);
		sony_tpad = NULL;
	}

	return 0;
}

#define SONY_FAN_HANDLE 0x0149
#define FAN_SPEEDS_NUM	4	/* leave some more room */
#define FAN_ATTRS_NUM	3
static struct sony_fan_device {
	unsigned int speeds_num;
	unsigned int speeds[4];
	struct device_attribute	attrs[3];
} *sony_fan;

static ssize_t sony_nc_fan_control_store(struct device *dev,
		struct device_attribute *attr,
		const char *buffer, size_t count)
{
	unsigned int result;
	unsigned long value;

	if (count > 31)
		return -EINVAL;
	if (strict_strtoul(buffer, 10, &value)
		|| value > sony_fan->speeds_num)
		return -EINVAL;

	if (sony_call_snc_handle(SONY_FAN_HANDLE,
				(value << 0x10) | 0x0200, &result))
		return -EIO;

	return count;
}

static ssize_t sony_nc_fan_control_show(struct device *dev,
		struct device_attribute *attr, char *buffer)
{
	ssize_t count = 0;
	unsigned int result;

	if (sony_call_snc_handle(SONY_FAN_HANDLE, 0x0100, &result))
		return -EINVAL;

	count = snprintf(buffer, PAGE_SIZE, "%d\n", result & 0xff);
	return count;
}

static ssize_t sony_nc_fan_profiles_show(struct device *dev,
		struct device_attribute *attr, char *buffer)
{
	ssize_t count = 0;
	unsigned int i;

	for (i = 0; i < sony_fan->speeds_num; i++)
		count += snprintf(buffer + count, PAGE_SIZE - count,
				"%.4u ", sony_fan->speeds[i] * 100);

	count += snprintf(buffer + count, PAGE_SIZE - count, "\n");

	return count;
}

static ssize_t sony_nc_fan_speed_show(struct device *dev,
		struct device_attribute *attr, char *buffer)
{
	ssize_t count = 0;
	unsigned int result;

	if (sony_call_snc_handle(SONY_FAN_HANDLE, 0x0300, &result))
		return -EINVAL;

	count = snprintf(buffer, PAGE_SIZE, "%d\n",
				(result & 0xff) * 100);
	return count;
}

static int sony_nc_fan_setup(struct platform_device *pd)
{
	int ret;
	unsigned int i, found;
	u8 list[FAN_SPEEDS_NUM * 2] = { 0 };

	sony_fan = kzalloc(sizeof(struct sony_fan_device), GFP_KERNEL);
	if (!sony_fan)
		return -ENOMEM;

	ret = sony_call_snc_handle_buffer(SONY_FAN_HANDLE, 0x0000,
					list, FAN_SPEEDS_NUM * 2);
	if (ret < 0)
		pr_info("unable to retrieve fan profiles table\n");

	for (i = 0, found = 0;
		list[i] != 0 && found < FAN_SPEEDS_NUM; i += 2, found++) {

		sony_fan->speeds[found] = list[i+1];
	}
	sony_fan->speeds_num = found;

	sysfs_attr_init(&sony_fan->attrs[0].attr);
	sony_fan->attrs[0].attr.name = "fan_speed";
	sony_fan->attrs[0].attr.mode = S_IRUGO;
	sony_fan->attrs[0].show = sony_nc_fan_speed_show;

	sysfs_attr_init(&sony_fan->attrs[1].attr);
	sony_fan->attrs[1].attr.name = "fan_profiles";
	sony_fan->attrs[1].attr.mode = S_IRUGO;
	sony_fan->attrs[1].show = sony_nc_fan_profiles_show;

	sysfs_attr_init(&sony_fan->attrs[2].attr);
	sony_fan->attrs[2].attr.name = "fan_control";
	sony_fan->attrs[2].attr.mode = S_IRUGO | S_IWUSR;
	sony_fan->attrs[2].show = sony_nc_fan_control_show;
	sony_fan->attrs[2].store = sony_nc_fan_control_store;

	for (i = 0; i < FAN_ATTRS_NUM; i++) {
		if (device_create_file(&pd->dev, &sony_fan->attrs[i]))
			goto attrserror;
	}

	return 0;

attrserror:
	for (; i > 0; i--)
		device_remove_file(&pd->dev, &sony_fan->attrs[i]);

	kfree(sony_fan);
	sony_fan = NULL;

	return -1;
}

static int sony_nc_fan_cleanup(struct platform_device *pd)
{
	if (sony_fan) {
		int i;

		for (i = 0; i < FAN_ATTRS_NUM; i++)
			device_remove_file(&pd->dev, &sony_fan->attrs[i]);

		kfree(sony_fan);
		sony_fan = NULL;
	}

	return 0;
}

static struct sony_odd_device {
	unsigned int vendor_id;
	unsigned int model_id;
	struct device_attribute status_attr;
} *sony_odd;

#if 0
static int sony_nc_odd_remove(void)
{
	/*
	   0 - change the link state first?
	   1 - scsi lookup searching for the optical device (scsi_device *)
	   2 - call int scsi_remove_device(struct scsi_device *sdev)
	*/

	struct scsi_device *sdev;
	struct Scsi_Host *shost;
	int error = -ENXIO;

	shost = scsi_host_lookup(host);
	if (!shost)
		return error;

	sdev = scsi_device_lookup(shost, channel, id, lun);
	if (sdev) {
		scsi_remove_device(sdev);
		scsi_device_put(sdev);
		error = 0;
	}

	scsi_host_put(shost);

	return 0;
}
#endif

static ssize_t sony_nc_odd_status_store(struct device *dev,
		struct device_attribute *attr,
		const char *buffer, size_t count)
{
	unsigned int result;
	unsigned long value;

	if (count > 31)
		return -EINVAL;
	if (strict_strtoul(buffer, 10, &value) || value > 1)
		return -EINVAL;

#if 0
	if (off)
		sony_nc_odd_remove();

		/* and goes on, otherwise leave */
#endif

	/* 0x200 turn on (sysfs: 1), 0x300 turn off (sysfs: 0) */
	value = (!value << 0x08) + 0x200;

	/* the MSB have to be high */
	if (sony_call_snc_handle(0x126, (1 << 0x10) | value, &result))
		return -EIO;

#if 0
	if (on)
		/* force a bus scan? */
#endif

	return count;
}

static ssize_t sony_nc_odd_status_show(struct device *dev,
		struct device_attribute *attr, char *buffer)
{
	ssize_t count = 0;
	unsigned int result;

	if (sony_call_snc_handle(0x126, 0x100, &result))
		return -EINVAL;

	count = snprintf(buffer, PAGE_SIZE, "%d\n", result & 0x01);
	return count;
}

static int sony_nc_odd_setup(struct platform_device *pd)
{
#define ODD_TAB_SIZE 32
	u8 list[ODD_TAB_SIZE] = { 0 };
	int ret = 0;
	int found = 0;
	int i = 0;
	unsigned int vendor = 0;
	unsigned int model = 0;
	u16 word = 0;

	ret = sony_call_snc_handle_buffer(0x126, 0x0000, list, ODD_TAB_SIZE);
	if (ret < 0) {
		pr_info("unable to retrieve the odd table\n");
		return -EIO;
	}

	/* parse the table looking for optical devices */
	do {
		word = (list[i+1] << 8) | list[i];

		if (word == 1) { /* 1 DWord device data following */
			vendor = (list[i+3] << 8) | list[i+2];
			model = (list[i+5] << 8) | list[i+4];
			found++;
			i += 6;
		} else {
			i += 2;
		}
	} while (word != 0xff00);

	if (found)
		dprintk("one optical device found, connected to: %x:%x\n",
				vendor, model);
	else
		return 0;

	sony_odd = kzalloc(sizeof(*sony_odd), GFP_KERNEL);
	if (!sony_odd)
		return -ENOMEM;

	sony_odd->vendor_id = vendor;
	sony_odd->model_id = model;

	sysfs_attr_init(&sony_odd->status_attr.attr);
	sony_odd->status_attr.attr.name = "odd_power";
	sony_odd->status_attr.attr.mode = S_IRUGO | S_IWUSR;
	sony_odd->status_attr.show = sony_nc_odd_status_show;
	sony_odd->status_attr.store = sony_nc_odd_status_store;

	if (device_create_file(&pd->dev, &sony_odd->status_attr)) {
		kfree(sony_odd);
		sony_odd = NULL;
		return -1;
	}

	return 0;
}

static int sony_nc_odd_cleanup(struct platform_device *pd)
{
	if (sony_odd) {
		device_remove_file(&pd->dev, &sony_odd->status_attr);
		kfree(sony_odd);
		sony_odd = NULL;
	}

	return 0;
}

/*
 * Backlight device
 */
static struct backlight_device *sony_backlight_device;

static int sony_backlight_update_status(struct backlight_device *bd)
{
	return acpi_callsetfunc(sony_nc_acpi_handle, "SBRT",
				bd->props.brightness + 1, NULL);
}

static int sony_backlight_get_brightness(struct backlight_device *bd)
{
	unsigned int value;

	if (acpi_callgetfunc(sony_nc_acpi_handle, "GBRT", &value))
		return 0;
	/* brightness levels are 1-based, while backlight ones are 0-based */
	return value - 1;
}

static const struct backlight_ops sony_backlight_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = sony_backlight_update_status,
	.get_brightness = sony_backlight_get_brightness,
};
static const struct backlight_ops sony_als_backlight_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = sony_nc_als_update_status,
	.get_brightness = sony_nc_als_get_brightness,
};

static void sony_nc_backlight_setup(void)
{
	acpi_handle unused;
	int max_brightness = 0;
	const struct backlight_ops *ops = NULL;
	struct backlight_properties props;

	/* do not use SNC GBRT/SBRT controls along with the ALS */
	if (sony_als) {
		/* ALS based backlight device */
		ops = &sony_als_backlight_ops;
		max_brightness = sony_als->levels_num - 1;
	} else if (ACPI_SUCCESS(acpi_get_handle(sony_nc_acpi_handle, "GBRT",
						&unused))) {
		ops = &sony_backlight_ops;
		max_brightness = SONY_MAX_BRIGHTNESS - 1;
	} else {
		return;
	}

	memset(&props, 0, sizeof(struct backlight_properties));
	props.type = BACKLIGHT_PLATFORM;
	props.max_brightness = max_brightness;
	sony_backlight_device = backlight_device_register("sony", NULL, NULL,
								ops, &props);

	if (IS_ERR(sony_backlight_device)) {
		pr_warn("unable to register backlight device\n");
		sony_backlight_device = NULL;
	} else {
		sony_backlight_device->props.brightness =
			ops->get_brightness(sony_backlight_device);
	}
}

static void sony_nc_backlight_cleanup(void)
{
	if (sony_backlight_device)
		backlight_device_unregister(sony_backlight_device);
}

static void sony_nc_tests_resume(void)
{
	return;
}

/* place here some unknown handles, we might want to see some output */
static void sony_nc_tests_setup(void)
{
	int ret;
	unsigned int result;

	result = 0;
	ret = sony_call_snc_handle(0x114, 0x000, &result);
	if (!ret)
		pr_info("handle 0x114 returned: %x\n", result & 0xff);

	result = 0;
	ret = sony_call_snc_handle(0x139, 0x0000, &result);
	if (!ret)
		pr_info("handle 0x139+00 returned: %x\n", result & 0xffff);

	result = 0;
	ret = sony_call_snc_handle(0x139, 0x0100, &result);
	if (!ret)
		pr_info("handle 0x139+01 returned: %x\n", result & 0xffff);

	return;
}

static void sony_nc_snc_setup_handles(struct platform_device *pd)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(handles->cap); i++) {
		int ret = 0;
		int unsigned handle = handles->cap[i];

		if (!handle)
			continue;

		dprintk("looking at handle 0x%.4x\n", handle);

		switch (handle) {
		case 0x0100:
		case 0x0127:
		case 0x0101:
		case 0x0102:
			ret = sony_nc_function_setup(handle);
			break;
		case 0x0105:
		case 0x0148: /* same as 0x0105 + Fn-F1 combo */
			ret = sony_nc_touchpad_setup(pd, handle);
			break;
		case 0x0115:
		case 0x0136:
		case 0x013f:
			ret = sony_nc_battery_care_setup(pd, handle);
			break;
		case 0x0119:
			ret = sony_nc_lid_resume_setup(pd);
			break;
		case 0x0122:
			ret = sony_nc_thermal_setup(pd);
			break;
		case 0x0126:
			ret = sony_nc_odd_setup(pd);
			break;
		case 0x0137:
		case 0x0143:
			ret = sony_nc_kbd_backlight_setup(pd, handle);
		case 0x012f: /* no keyboard backlight */
			ret = sony_nc_als_setup(pd, handle);
			break;
		case 0x0131:
			ret = sony_nc_highspeed_charging_setup(pd);
			break;
		case 0x0134:
		case 0x0147:
			ret = sony_nc_gsensor_setup(pd, handle);
			break;
		case 0x0149:
			ret = sony_nc_fan_setup(pd);
			break;
		case 0x0124:
		case 0x0135:
			ret = sony_nc_rfkill_setup(sony_nc_acpi_device, handle);
			break;
		default:
			continue;
		}

		if (ret < 0) {
			pr_warn("handle 0x%.4x setup failed (ret: %i)",
								handle, ret);
		} else {
			dprintk("handle 0x%.4x setup completed\n", handle);
		}
	}

	if (debug)
		sony_nc_tests_setup();
}

static void sony_nc_snc_cleanup_handles(struct platform_device *pd)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(handles->cap); i++) {

		int unsigned handle = handles->cap[i];

		if (!handle)
			continue;

		dprintk("looking at handle 0x%.4x\n", handle);

		switch (handle) {
		case 0x0105:
		case 0x0148:
			sony_nc_touchpad_cleanup(pd);
			break;
		case 0x0115:
		case 0x0136:
		case 0x013f:
			sony_nc_battery_care_cleanup(pd);
			break;
		case 0x0119:
			sony_nc_lid_resume_cleanup(pd);
			break;
		case 0x0122:
			sony_nc_thermal_cleanup(pd);
			break;
		case 0x0126:
			sony_nc_odd_cleanup(pd);
			break;
		case 0x0137:
		case 0x0143:
			sony_nc_kbd_backlight_cleanup(pd);
		case 0x012f:
			sony_nc_als_cleanup(pd);
			break;
		case 0x0131:
			sony_nc_highspeed_charging_cleanup(pd);
			break;
		case 0x0134:
		case 0x0147:
			sony_nc_gsensor_cleanup(pd);
			break;
		case 0x0149:
			sony_nc_fan_cleanup(pd);
			break;
		case 0x0124:
		case 0x0135:
			sony_nc_rfkill_cleanup();
			break;
		default:
			continue;
		}

		dprintk("handle 0x%.4x deconfigured\n", handle);
	}
}

static int sony_nc_snc_setup(struct platform_device *pd)
{
	unsigned int i, string[4], bitmask, result;

	for (i = 0; i < 4; i++) {
		if (acpi_callsetfunc(sony_nc_acpi_handle,
				"SN00", i, &string[i]))
			return -EIO;
	}
	if (strncmp("SncSupported", (char *) string, 0x10)) {
		pr_info("SNC device present but not supported by hardware");
		return -1;
	}

	if (!acpi_callsetfunc(sony_nc_acpi_handle, "SN00", 0x04, &result)) {
		unsigned int model, i;
		for (model = 0, i = 0; i < 4; i++)
			model |= ((result >> (i * 8)) & 0xff) << ((3 - i) * 8);
		pr_info("found Vaio model ID: %u\n", model);
	}

	/* retrieve the implemented offsets mask */
	if (acpi_callsetfunc(sony_nc_acpi_handle, "SN00", 0x10, &bitmask))
		return -EIO;

	/* retrieve the available handles, otherwise return */
	if (sony_nc_handles_setup(pd))
		return -2;

	/* setup found handles here */
	sony_nc_snc_setup_handles(pd);

	/* Enable all events for the found handles, otherwise return */
	if (acpi_callsetfunc(sony_nc_acpi_handle, "SN02", bitmask, &result))
		return -EIO;

	/* check for SN05 presence? */

	return 0;
}

static int sony_nc_snc_cleanup(struct platform_device *pd)
{
	unsigned int result, bitmask;

	/* retrieve the event enabled handles */
	acpi_callgetfunc(sony_nc_acpi_handle, "SN01", &bitmask);

	/* disable the event generation	for every handle */
	acpi_callsetfunc(sony_nc_acpi_handle, "SN03", bitmask, &result);

	/* cleanup handles here */
	sony_nc_snc_cleanup_handles(pd);

	sony_nc_handles_cleanup(pd);

	return 0;
}

static int sony_nc_snc_resume(void)
{
	unsigned int i, result, bitmask;

	/* retrieve the implemented offsets mask */
	if (acpi_callsetfunc(sony_nc_acpi_handle, "SN00", 0x10, &bitmask))
		return -EIO;

	/* Enable all events, otherwise return */
	if (acpi_callsetfunc(sony_nc_acpi_handle, "SN02", bitmask, &result))
		return -EIO;

	for (i = 0; i < ARRAY_SIZE(handles->cap); i++) {
		int unsigned handle = handles->cap[i];

		if (!handle)
			continue;

		dprintk("looking at handle 0x%.4x\n", handle);

		switch (handle) {
		case 0x0100:
		case 0x0127:
		case 0x0101:
		case 0x0102:
			sony_nc_function_setup(handle);
			break;
		case 0x0122:
			sony_nc_thermal_resume();
			break;
		case 0x0124:
		case 0x0135:
			/* re-read rfkill state */
			sony_nc_rfkill_update();
			break;
		case 0x0137: /* kbd + als */
		case 0x0143:
			sony_nc_kbd_backlight_resume();
		case 0x012f: /* als only */
			sony_nc_als_resume();
			break;
		default:
			continue;
		}

		dprintk("handle 0x%.4x updated\n", handle);
	}

	if (debug)
		sony_nc_tests_resume();

	return 0;
}

/*
 * ACPI callbacks
 */
static acpi_status sony_walk_callback(acpi_handle handle, u32 level,
				      void *context, void **return_value)
{
	struct acpi_device_info *info;

	if (ACPI_SUCCESS(acpi_get_object_info(handle, &info))) {
		pr_warn("method: name: %4.4s, args %X\n",
			(char *)&info->name, info->param_count);

		kfree(info);
	}

	return AE_OK;
}

#define EV_HOTKEYS	1
#define EV_RFKILL	2
#define EV_ALS		3
#define EV_GSENSOR	4
#define	EV_HGFX		5
static void sony_nc_notify(struct acpi_device *device, u32 event)
{
	u8 ev = 0;
	int value = 0;
	char *env[2] = { NULL };

	dprintk("sony_nc_notify, event: 0x%.2x\n", event);

	/* handles related events */
	if (event >= 0x90) {
		unsigned int result = 0, handle = 0;

		/* the event should corrispond to the offset of the method */
		unsigned int offset = event - 0x90;

		handle = handles->cap[offset];
		switch (handle) {
		/* list of handles known for generating events */
		case 0x0100:
		case 0x0127:
			/* hotkey event, a key has been pressed, retrieve it */
			value = sony_nc_hotkeys_decode(handle);
			if (value > 0) /* known event */
				sony_laptop_report_input_event(value);
			else /* restore the original event */
			    value = event;

			ev = EV_HOTKEYS;
			break;

		case 0x0143:
			sony_call_snc_handle(handle, 0x2000, &result);
			/* event reasons are reverted */
			value = (result & 0x03) == 1 ? 2 : 1;
			dprintk("sony_nc_notify, ALS event received (reason:"
				       " %s change)\n", value == 1 ? "light" :
				       "backlight");

			env[0] = (value == 1) ? "ALS=1" : "ALS=2";
			kobject_uevent_env(&device->dev.kobj, KOBJ_CHANGE, env);

			ev = EV_ALS;
			break;

		case 0x012f:
		case 0x0137:
			sony_call_snc_handle(handle, 0x0800, &result);
			value = result & 0x03;
			dprintk("sony_nc_notify, ALS event received (reason:"
					" %s change)\n", value == 1 ? "light" :
					"backlight");
			if (value == 1) /* lighting change reason */
				sony_nc_als_event_handler();

			env[0] = (value == 1) ? "ALS=1" : "ALS=2";
			kobject_uevent_env(&device->dev.kobj, KOBJ_CHANGE, env);

			ev = EV_ALS;
			break;

		case 0x0124:
		case 0x0135:
			sony_call_snc_handle(handle, 0x0100, &result);
			result &= 0x03;
			dprintk("sony_nc_notify, RFKILL event received "
					"(reason: %s)\n", result == 1 ?
					"switch state changed" : "battery");

			if (result == 1) { /* hw swtich event */
				sony_nc_rfkill_update();
				value = sony_nc_get_rfkill_hwblock();
			} else if (result == 2) { /* battery event */
				/*  we might need to change the WWAN rfkill
				    state when the battery state changes
				 */
				sony_nc_rfkill_update_wwan();
				return;
			}

			ev = EV_RFKILL;
			break;

		case 0x0134:
		case 0x0147:
			ev = 4;
			value = EV_GSENSOR;
			/* hdd protection event, notify userspace */

			env[0] = "HDD_SHOCK=1";
			kobject_uevent_env(&device->dev.kobj, KOBJ_CHANGE, env);

			break;

		case 0x0128:
		case 0x0146:
			/* Hybrid GFX switching, 1 */
			sony_call_snc_handle(handle, 0x0000, &result);
			dprintk("sony_nc_notify, Hybrid GFX event received "
					"(reason: %s)\n", (result & 0x01) ?
					"switch position change" : "unknown");

			/* verify the switch state
			   (1: discrete GFX, 0: integrated GFX)*/
			result = 0;
			sony_call_snc_handle(handle, 0x0100, &result);

			/* sony_laptop_report_input_event(); */

			ev = EV_HGFX;
			value = result & 0xff;
			break;

		default:
			value = event;
			dprintk("Unknowk event for handle: 0x%x\n", handle);
			break;
		}

		/* clear the event (and the event reason when present) */
		acpi_callsetfunc(sony_nc_acpi_handle, "SN05", 1 << offset,
				&result);
	} else {
		ev = 1;
		sony_laptop_report_input_event(event);
	}

	acpi_bus_generate_proc_event(device, ev, value);
	acpi_bus_generate_netlink_event(device->pnp.device_class,
					dev_name(&device->dev), ev, value);
}

static int sony_nc_add(struct acpi_device *device)
{
	acpi_status status;
	int result = 0;
	acpi_handle handle;
	struct sony_nc_value *item;

	pr_info("%s v%s\n", SONY_NC_DRIVER_NAME, SONY_LAPTOP_DRIVER_VERSION);

	sony_nc_acpi_device = device;
	strcpy(acpi_device_class(device), "sony/hotkey");

	sony_nc_acpi_handle = device->handle;

	/* read device status */
	result = acpi_bus_get_status(device);
	/* bail IFF the above call was successful
	   and the device is not present */
	if (!result && !device->status.present) {
		dprintk("Device not present\n");
		result = -ENODEV;
		goto outwalk;
	}

	result = sony_pf_add();
	if (result)
		goto outpresent;

	if (debug) {
		status = acpi_walk_namespace(ACPI_TYPE_METHOD,
				sony_nc_acpi_handle, 1, sony_walk_callback,
				NULL, NULL, NULL);
		if (ACPI_FAILURE(status)) {
			pr_warn("unable to walk acpi resources\n");
			result = -ENODEV;
			goto outpresent;
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

		if (sony_nc_snc_setup(sony_pf_device))
			goto outsnc;
	}

	/* setup input devices and helper fifo */
	result = sony_laptop_setup_input(device);
	if (result) {
		pr_err("Unable to create input devices\n");
		goto outsnc;
	}

	if (acpi_video_backlight_support()) {
		pr_info("brightness ignored, must be "
			"controlled by ACPI video driver\n");
	} else {
		sony_nc_backlight_setup();
	}

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
	for (item = sony_nc_values; item->name; ++item)
		device_remove_file(&sony_pf_device->dev, &item->devattr);

	sony_nc_backlight_cleanup();

	sony_laptop_remove_input();

outsnc:
	sony_nc_snc_cleanup(sony_pf_device);

outpresent:
	sony_pf_remove();

outwalk:
	return result;
}

static int sony_nc_remove(struct acpi_device *device, int type)
{
	struct sony_nc_value *item;

	sony_nc_backlight_cleanup();

	sony_nc_acpi_device = NULL;

	for (item = sony_nc_values; item->name; ++item)
		device_remove_file(&sony_pf_device->dev, &item->devattr);

	sony_nc_snc_cleanup(sony_pf_device);
	sony_pf_remove();
	sony_laptop_remove_input();
	dprintk(SONY_NC_DRIVER_NAME " removed.\n");

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
			pr_err("%s: %d\n", __func__, ret);
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

		sony_nc_snc_resume();
	}

	/* set the last requested brightness level */
	if (sony_backlight_device &&
		sony_backlight_ops.update_status(sony_backlight_device) < 0)
		pr_warn("unable to restore brightness level\n");

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
		.notify = sony_nc_notify,
		},
};

/*********** SPIC (SNY6001) Device ***********/

#define SONYPI_DEVICE_TYPE1	0x00000001
#define SONYPI_DEVICE_TYPE2	0x00000002
#define SONYPI_DEVICE_TYPE3	0x00000004

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
	{ 0x59, SONYPI_EVENT_IGNORE },
	{ 0x5a, SONYPI_EVENT_IGNORE },
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

	wait_on_command(inb_p(spic_dev.cur_ioport->io1.minimum + 4) & 2,
			ITERATIONS_LONG);
	outb(dev, spic_dev.cur_ioport->io1.minimum + 4);
	wait_on_command(inb_p(spic_dev.cur_ioport->io1.minimum + 4) & 2,
			ITERATIONS_LONG);
	outb(fn, spic_dev.cur_ioport->io1.minimum);
	wait_on_command(inb_p(spic_dev.cur_ioport->io1.minimum + 4) & 2,
			ITERATIONS_LONG);
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

	pr_info("detected Type%d model\n",
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
#define SONYPI_CAMERA_SHUTTER_MASK		0x7

#define SONYPI_CAMERA_SHUTDOWN_REQUEST		7
#define SONYPI_CAMERA_CONTROL			0x10

#define SONYPI_CAMERA_STATUS			7
#define SONYPI_CAMERA_STATUS_READY		0x2
#define SONYPI_CAMERA_STATUS_POSITION		0x4

#define SONYPI_DIRECTION_BACKWARDS		0x4

#define SONYPI_CAMERA_REVISION			8
#define SONYPI_CAMERA_ROMVERSION		9

static int __sony_pic_camera_ready(void)
{
	u8 v;

	v = sony_pic_call2(0x8f, SONYPI_CAMERA_STATUS);
	return (v != 0xff && (v & SONYPI_CAMERA_STATUS_READY));
}

static int __sony_pic_camera_off(void)
{
	if (!camera) {
		pr_warn("camera control not enabled\n");
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
		pr_warn("camera control not enabled\n");
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
		pr_warn("failed to power on camera\n");
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
		wait_on_command(sony_pic_call3(0x90, SONYPI_CAMERA_BRIGHTNESS,
				value),	ITERATIONS_SHORT);
		break;
	case SONY_PIC_COMMAND_SETCAMERACONTRAST:
		wait_on_command(sony_pic_call3(0x90, SONYPI_CAMERA_CONTRAST,
				value),	ITERATIONS_SHORT);
		break;
	case SONY_PIC_COMMAND_SETCAMERAHUE:
		wait_on_command(sony_pic_call3(0x90, SONYPI_CAMERA_HUE, value),
				ITERATIONS_SHORT);
		break;
	case SONY_PIC_COMMAND_SETCAMERACOLOR:
		wait_on_command(sony_pic_call3(0x90, SONYPI_CAMERA_COLOR,
				value),	ITERATIONS_SHORT);
		break;
	case SONY_PIC_COMMAND_SETCAMERASHARPNESS:
		wait_on_command(sony_pic_call3(0x90, SONYPI_CAMERA_SHARPNESS,
				value),	ITERATIONS_SHORT);
		break;
	case SONY_PIC_COMMAND_SETCAMERAPICTURE:
		wait_on_command(sony_pic_call3(0x90, SONYPI_CAMERA_PICTURE,
				value),	ITERATIONS_SHORT);
		break;
	case SONY_PIC_COMMAND_SETCAMERAAGC:
		wait_on_command(sony_pic_call3(0x90, SONYPI_CAMERA_AGC, value),
				ITERATIONS_SHORT);
		break;
	default:
		pr_err("sony_pic_camera_command invalid: %d\n", command);
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

	if (strict_strtoul(buffer, 10, &value))
		return -EINVAL;

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

	if (strict_strtoul(buffer, 10, &value))
		return -EINVAL;

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

	if (strict_strtoul(buffer, 10, &value))
		return -EINVAL;

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
	struct kfifo		fifo;
	spinlock_t		fifo_lock;
	wait_queue_head_t	fifo_proc_list;
	atomic_t		open_count;
};
static struct sonypi_compat_s sonypi_compat = {
	.open_count = ATOMIC_INIT(0),
};

static int sonypi_misc_fasync(int fd, struct file *filp, int on)
{
	return fasync_helper(fd, filp, on, &sonypi_compat.fifo_async);
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

	spin_lock_irqsave(&sonypi_compat.fifo_lock, flags);

	if (atomic_inc_return(&sonypi_compat.open_count) == 1)
		kfifo_reset(&sonypi_compat.fifo);

	spin_unlock_irqrestore(&sonypi_compat.fifo_lock, flags);

	return 0;
}

static ssize_t sonypi_misc_read(struct file *file, char __user *buf,
				size_t count, loff_t *pos)
{
	ssize_t ret;
	unsigned char c;

	if ((kfifo_len(&sonypi_compat.fifo) == 0) &&
	    (file->f_flags & O_NONBLOCK))
		return -EAGAIN;

	ret = wait_event_interruptible(sonypi_compat.fifo_proc_list,
				       kfifo_len(&sonypi_compat.fifo) != 0);
	if (ret)
		return ret;

	while (ret < count &&
	       (kfifo_out_locked(&sonypi_compat.fifo, &c, sizeof(c),
			  &sonypi_compat.fifo_lock) == sizeof(c))) {
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
	if (kfifo_len(&sonypi_compat.fifo))
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
	unsigned int value;

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
	.llseek		= noop_llseek,
};

static struct miscdevice sonypi_misc_device = {
	.minor		= MISC_DYNAMIC_MINOR,
	.name		= "sonypi",
	.fops		= &sonypi_misc_fops,
};

static void sonypi_compat_report_event(u8 event)
{
	kfifo_in_locked(&sonypi_compat.fifo, (unsigned char *)&event,
			sizeof(event), &sonypi_compat.fifo_lock);
	kill_fasync(&sonypi_compat.fifo_async, SIGIO, POLL_IN);
	wake_up_interruptible(&sonypi_compat.fifo_proc_list);
}

static int sonypi_compat_init(void)
{
	int error;

	spin_lock_init(&sonypi_compat.fifo_lock);
	error =
	 kfifo_alloc(&sonypi_compat.fifo, SONY_LAPTOP_BUF_SIZE, GFP_KERNEL);
	if (error) {
		pr_err("kfifo_alloc failed\n");
		return error;
	}

	init_waitqueue_head(&sonypi_compat.fifo_proc_list);

	if (minor != -1)
		sonypi_misc_device.minor = minor;
	error = misc_register(&sonypi_misc_device);
	if (error) {
		pr_err("misc_register failed\n");
		goto err_free_kfifo;
	}
	if (minor == -1)
		pr_info("device allocated minor is %d\n",
			sonypi_misc_device.minor);

	return 0;

err_free_kfifo:
	kfifo_free(&sonypi_compat.fifo);
	return error;
}

static void sonypi_compat_exit(void)
{
	misc_deregister(&sonypi_misc_device);
	kfifo_free(&sonypi_compat.fifo);
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
			struct sony_pic_ioport *ioport =
					kzalloc(sizeof(*ioport), GFP_KERNEL);
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
					pr_warn("Invalid IRQ %d\n",
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
				list_first_entry(&dev->ioports,
						struct sony_pic_ioport, list);
			if (!io) {
				dprintk("Blank IO resource\n");
				return AE_OK;
			}

			if (!ioport->io1.minimum) {
				memcpy(&ioport->io1, io, sizeof(*io));
				dprintk("IO1 at 0x%.4x (0x%.2x)\n",
						ioport->io1.minimum,
						ioport->io1.address_length);
			} else if (!ioport->io2.minimum) {
				memcpy(&ioport->io2, io, sizeof(*io));
				dprintk("IO2 at 0x%.4x (0x%.2x)\n",
						ioport->io2.minimum,
						ioport->io2.address_length);
			} else {
				pr_err("Unknown SPIC Type, "
					"more than 2 IO Ports\n");
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
		pr_warn("Unable to read status\n");
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
		pr_warn("Failure evaluating %s\n", METHOD_NAME__PRS);
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
		pr_err("Error evaluating _SRS\n");
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
				/* some events may require ignoring */
				if (!device_event)
					return IRQ_HANDLED;
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
		pr_err("Couldn't disable device\n");
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

	pr_info("%s v%s\n", SONY_PIC_DRIVER_NAME, SONY_LAPTOP_DRIVER_VERSION);

	spic_dev.acpi_dev = device;
	strcpy(acpi_device_class(device), "sony/hotkey");
	sony_pic_detect_device_type(&spic_dev);
	mutex_init(&spic_dev.lock);

	/* read _PRS resources */
	result = sony_pic_possible_resources(device);
	if (result) {
		pr_err("Unable to read possible resources\n");
		goto err_free_resources;
	}

	/* setup input devices and helper fifo */
	result = sony_laptop_setup_input(device);
	if (result) {
		pr_err("Unable to create input devices\n");
		goto err_free_resources;
	}

	if (sonypi_compat_init())
		goto err_remove_input;

	/* request io port */
	list_for_each_entry_reverse(io, &spic_dev.ioports, list) {
		if (request_region(io->io1.minimum, io->io1.address_length,
					"Sony Programmable I/O Device")) {
			dprintk("I/O port1: 0x%.4x (0x%.4x) + 0x%.2x\n",
					io->io1.minimum, io->io1.maximum,
					io->io1.address_length);
			/* Type 1 have 2 ioports */
			if (io->io2.minimum) {
				if (request_region(io->io2.minimum,
					io->io2.address_length,
					"Sony Programmable I/O Device")) {
					dprintk("I/O port2: 0x%.4x (0x%.4x) "
							"+ 0x%.2x\n",
							io->io2.minimum,
							io->io2.maximum,
							io->io2.address_length);
					spic_dev.cur_ioport = io;
					break;
				} else {
					dprintk("Unable to get I/O port2: "
							"0x%.4x (0x%.4x) "
							"+ 0x%.2x\n",
							io->io2.minimum,
							io->io2.maximum,
							io->io2.address_length);
					release_region(io->io1.minimum,
							io->io1.address_length);
				}
			} else {
				spic_dev.cur_ioport = io;
				break;
			}
		}
	}
	if (!spic_dev.cur_ioport) {
		pr_err("Failed to request_region\n");
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
		pr_err("Failed to request_irq\n");
		result = -ENODEV;
		goto err_release_region;
	}

	/* set resource status _SRS */
	result = sony_pic_enable(device, spic_dev.cur_ioport, spic_dev.cur_irq);
	if (result) {
		pr_err("Couldn't enable device\n");
		goto err_free_irq;
	}

	spic_dev.bluetooth_power = -1;
	/* create device attributes */
	result = sony_pf_add();
	if (result)
		goto err_disable_device;

	result = sysfs_create_group(&sony_pf_device->dev.kobj,
					&spic_attribute_group);
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
			pr_err("Unable to register SPIC driver\n");
			goto out;
		}
		spic_drv_registered = 1;
	}

	result = acpi_bus_register_driver(&sony_nc_driver);
	if (result) {
		pr_err("Unable to register SNC driver\n");
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
