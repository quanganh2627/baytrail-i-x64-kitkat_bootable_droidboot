/*
 * Copyright (c) 2009, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *  * Neither the name of Google, Inc. nor the names of its contributors
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#define LOG_TAG "droidboot"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/reboot.h>
#include <unistd.h>
#include <sys/mount.h>
#include <minui/minui.h>
#include <cutils/android_reboot.h>
#include <cutils/properties.h>
#include <cutils/klog.h>
#include <cutils/hashmap.h>
#include <charger/charger.h>

#include "volumeutils/ufdisk.h"
#include "volumeutils/roots.h"
#include "aboot.h"
#include "droidboot_util.h"
#include "droidboot_plugin.h"
#include "droidboot_installer.h"
#include "droidboot.h"
#include "fastboot.h"
#include "droidboot_ui.h"

/* Generated by the makefile, this function defines the
 * register_droidboot_plugins() function, which calls all the
 * registration functions for device-specific extensions. */
#include "register.inc"

/* Default size of memory buffer for image data */
static int g_scratch_size = 0;

/* Minimum battery % before we do anything */
static int g_min_battery = 10;

/* Flag to enable/disable auto paritionning from kernel command line -
   Disabled by default */
static int g_auto_partition = 0;

/* Flag to enable/disable the mount of partitions from kernel command line -
   Enabled by default */
static int g_mount_partition = 1;

/*
 * Flag to disable the display on the screen when processing fastboot command.
 * displaying the command is very slow on byt-M (4s for getvar version)
 * this flag is a temporary workaround to have faster/more reliable flash.
 * */
int g_disable_fboot_ui = 0;

#ifdef USE_GUI

#define NO_ACTION           -1
#define HIGHLIGHT_UP        -2
#define HIGHLIGHT_DOWN      -3
#define SELECT_ITEM         -4

enum {
	ITEM_BOOTLOADER,
	ITEM_REBOOT,
	ITEM_RECOVERY,
	ITEM_POWEROFF
};

extern struct color white;
extern struct color green;
extern struct color brown;

static char **title, **info;
static struct color* title_clr[] = {&brown, NULL};
static struct color* info_clr[] = {&white, &white, &white, &white, &white, &white, &green, &green, &green, NULL};

static char* menu[] = {"REBOOT DROIDBOOT",
								   "REBOOT",
								   "RECOVERY",
								   "POWER OFF", NULL};

static int table_init(char*** table, int width, int height)
{
	int i;

	if ((*table = malloc(height * sizeof(char*))) == NULL)
		return -1;
	for (i = 0; i < height; i++) {
		if (((*table)[i] = malloc(width * sizeof(char))) == NULL)
			return -1;
		memset((*table)[i], 0, width);
	}
	return 0;
}

static void table_exit(char ** table, int height)
{
	int i;

	if (table) {
		for (i = 0; i < height; i++)
			if (table[i])   free(table[i]);
	}
}

#define SYSFS_FORCE_SHUTDOWN	"/sys/module/intel_mid_osip/parameters/force_shutdown_occured"
void force_shutdown()
{
	int fd;
	char c = '1';

	pr_info("[SHTDWN] %s, force shutdown", __func__);
	if ((fd = open(SYSFS_FORCE_SHUTDOWN, O_WRONLY)) < 0) {
		pr_error("[SHUTDOWN] Open %s error!\n", SYSFS_FORCE_SHUTDOWN);
	} else {
                if (write(fd, &c, 1) < 0)
                        pr_error("[SHUTDOWN] Write %s error!\n", SYSFS_FORCE_SHUTDOWN);
		close(fd);
	}

	sync();
	reboot(LINUX_REBOOT_CMD_POWER_OFF);
}

static void goto_recovery()
{
	if (ui_block_visible(INFO) == VISIBLE)
		table_exit(info, INFO_MAX);
	if (ui_block_visible(TITLE) == VISIBLE)
		table_exit(title, TITLE_MAX);
	sleep(1);
	android_reboot(ANDROID_RB_RESTART2, 0, "recovery");
	ui_msg(ALERT, "SWITCH TO RECOVERY FAILED!");
}

#define BUF_IFWI_SZ			10
#define BUF_PRODUCT_SZ		10
#define BUF_SERIALNUM_SZ		20
extern Hashmap *ui_cmds;
static int get_info()
{
	int i;
	char ifwi[BUF_IFWI_SZ], product[BUF_PRODUCT_SZ], serialnum[BUF_SERIALNUM_SZ];
	ui_func cb;

	cb = hashmapGet(ui_cmds, UI_GET_SYSTEM_INFO);
	if (cb == NULL) {
		pr_error("Get ui_cmd: %s error!\n", UI_GET_SYSTEM_INFO);
		return -1;
	}
	memset(ifwi, 0, BUF_IFWI_SZ);
	memset(product, 0, BUF_PRODUCT_SZ);
	memset(serialnum, 0, BUF_SERIALNUM_SZ);
	cb(IFWI_VERSION, ifwi, BUF_IFWI_SZ);
	cb(PRODUCT_NAME, product, BUF_PRODUCT_SZ);
	cb(SERIAL_NUM, serialnum, BUF_SERIALNUM_SZ);

	for(i = 0; i < MAX_COLS-1; i++) {
		info[0][i] = '_';
		info[5][i] = '_';
	}

	snprintf(info[1], MAX_COLS, "IFWI VERSION: %s", ifwi);
	snprintf(info[2], MAX_COLS, "SERIAL_NUM: %s", serialnum);
	snprintf(info[3], MAX_COLS, "DROIDBOOT VERSION: %s", DROIDBOOT_VERSION);
	snprintf(info[4], MAX_COLS, "PRODUCT: %s", product);
	snprintf(info[7], MAX_COLS, "SELECT - VOL_UP OR VOL_DOWN");
	snprintf(info[8], MAX_COLS, "EXCUTE - POWER OR CAMERA");

	return 0;
}

static int device_handle_key(int key_code, int visible)
{
	if (visible) {
		switch (key_code) {
			case KEY_VOLUMEDOWN:
				return HIGHLIGHT_DOWN;

			case KEY_VOLUMEUP:
				return HIGHLIGHT_UP;

			case KEY_POWER:
			case KEY_CAMERA:
				return SELECT_ITEM;
		}
	}
	return NO_ACTION;
}

static int get_menu_selection(char** items, int initial_selection) {
	ui_clear_key_queue();
	ui_start_menu(items, initial_selection);
	int selected = initial_selection;
	int chosen_item = -1;

	while (chosen_item < 0) {
		int key = ui_wait_key();
		int visible = ui_block_visible(MENU);
		int action = device_handle_key(key, visible);

		if (ui_get_screen_state() == 0)
			ui_set_screen_state(1);
		else
			switch (action) {
				case HIGHLIGHT_UP:
					--selected;
					selected = ui_menu_select(selected);
					break;
				case HIGHLIGHT_DOWN:
					++selected;
					selected = ui_menu_select(selected);
					break;
				case SELECT_ITEM:
					chosen_item = selected;
					break;
				case NO_ACTION:
					break;
			}
	}
	return chosen_item;
}

static void prompt_and_wait()
{
	for (;;) {
		int chosen_item = get_menu_selection(menu, 0);
		switch (chosen_item) {
			case ITEM_BOOTLOADER:
				sync();
				android_reboot(ANDROID_RB_RESTART2, 0, "bootloader");
				break;
			case ITEM_RECOVERY:
				sync();
				goto_recovery();
				break;
			case ITEM_REBOOT:
				sync();
				android_reboot(ANDROID_RB_RESTART2, 0, "android");
				break;
			case ITEM_POWEROFF:
				force_shutdown();
				break;
		}
	}
}
#endif

static void parse_cmdline_option(char *name)
{
	char *value = strchr(name, '=');

	if (value == 0)
		return;
	*value++ = 0;
	if (*name == 0)
		return;

	if (!strncmp(name, "droidboot", 9))
		pr_info("Got parameter %s = %s\n", name, value);
	else
		return;

	if (!strcmp(name, "droidboot.scratch")) {
		g_scratch_size = atoi(value);
	} else if (!strcmp(name, "droidboot.minbatt")) {
		g_min_battery = atoi(value);
	} else if (!strcmp(name, "droidboot.autopart")) {
		g_auto_partition = atoi(value);
	} else if (!strcmp(name, "droidboot.mountpart")) {
		g_mount_partition = atoi(value);
	} else if (!strcmp(name, "droidboot.disablefbootui")) {
		g_disable_fboot_ui = atoi(value);
	} else if (!strcmp(name, "droidboot.installer_usb")) {
		strncpy(g_installer_usb_dev, value, BUFSIZ);
		g_installer_usb_dev[BUFSIZ-1] = '\0';
	} else if (!strcmp(name, "droidboot.installer_sdcard")) {
		strncpy(g_installer_sdcard_dev, value, BUFSIZ);
		g_installer_sdcard_dev[BUFSIZ-1] = '\0';
	} else if (!strcmp(name, "droidboot.installer_internal")) {
		strncpy(g_installer_internal_dev, value, BUFSIZ);
		g_installer_internal_dev[BUFSIZ-1] = '\0';
	} else if (!strcmp(name, "droidboot.installer_remote")) {
		strncpy(g_installer_remote_dev, value, BUFSIZ);
		g_installer_remote_dev[BUFSIZ-1]='\0';
	} else if (!strcmp(name, "droidboot.use_installer")) {
		g_use_installer = atoi(value);
	} else if (!strcmp(name, "droidboot.installer_file")) {
		strncpy(g_installer_file, value, BUFSIZ);
		g_installer_file[BUFSIZ-1] = '\0';
	} else {
		pr_error("Unknown parameter %s, ignoring\n", name);
	}
}

static void *fastboot_thread(void *arg)
{
	pr_info("Listening for the fastboot protocol over USB.");
	ui_print("FASTBOOT INIT...\n");
	fastboot_init(g_scratch_size * MEGABYTE);
	return NULL;
}

extern int oem_partition_cmd_handler(int argc, char **argv);

int main(int argc, char **argv)
{
	freopen("/dev/console", "a", stdout); setbuf(stdout, NULL);
	freopen("/dev/console", "a", stderr); setbuf(stderr, NULL);

	pr_info(" -- Droidboot %s for %s --\n", DROIDBOOT_VERSION, DEVICE_NAME);
	import_kernel_cmdline(parse_cmdline_option);

	aboot_register_commands();
	register_droidboot_plugins();

#ifdef USE_GUI
	ui_init();
	/* Enforce a minimum battery level */
	if (g_min_battery != 0) {
		pr_info("Verifying battery level >= %d%% before continuing\n",
				g_min_battery);
		klog_init();
		klog_set_level(8);

		switch (charger_run(g_min_battery, MODE_NON_CHARGER,
					POWER_ON_KEY_TIME,
					BATTERY_UNKNOWN_TIME,
					UNPLUGGED_SHUTDOWN_TIME,
					CAPACITY_POLL_INTERVAL)) {
		case CHARGER_SHUTDOWN:
			android_reboot(ANDROID_RB_POWEROFF, 0, 0);
			break;
		case CHARGER_PROCEED:
			pr_info("Battery level is acceptable\n");
			break;
		default:
			pr_error("mysterious return value from charger_run()\n");
		}
	}

	ui_event_init();
	ui_set_background(BACKGROUND_ICON_BACKGROUND);

	//Init title table
	if (table_init(&title, MAX_COLS, TITLE_MAX) == 0) {
		snprintf(title[0], MAX_COLS, "DROIDBOOT PROVISION OS");
		ui_block_init(TITLE, (char**)title, title_clr);
	} else {
		pr_error("Init title table error!\n");
	}
	//Init info table
	if (table_init(&info, MAX_COLS, INFO_MAX) == 0) {
		if(get_info() < 0)
			pr_error("get_info error!\n");
		ui_block_init(INFO, (char**)info, info_clr);
	} else {
		pr_error("Init info table error!\n");
	}
	ui_block_show(MSG);
#endif

	/* Unset previous settings before switching to fastboot */
	property_set("sys.usb.config", "none");

	ui_show_process(VISIBLE);
	load_volume_table();

	// Create automatically partitions if they do not exist
	// and if specified in the kernel command line
	if (ufdisk_need_create_partition()) {
		if(g_auto_partition == 1) {
			// set the property to allow the partitioning
			property_set("sys.partitioning", "1");
			char *path[] = {0,"/etc/partition.tbl"};
			oem_partition_cmd_handler(2,(char **)path);
		}
	}

	// Unless specified in the kernel command line, by default,
	// the partitions are mounted !
	if(g_mount_partition  == 1)
		// set the property to mount the partitions
		property_set("sys.partitioning", "0");

#ifdef USE_GUI
	ui_block_show(TITLE);
	ui_block_show(INFO);
	ui_block_show(LOG);
	pthread_t th_fastboot;

	pthread_create(&th_fastboot, NULL, fastboot_thread, NULL);
#ifdef USE_INSTALLER
	pthread_t th_installer;
	if (g_use_installer)
		pthread_create(&th_installer, NULL, installer_thread, NULL);
#endif

	//wait for user's choice
	prompt_and_wait();
#else
#ifdef USE_INSTALLER
	if (g_use_installer)
		installer_thread(NULL);
#endif

	fastboot_thread(NULL);
#endif

	/* Shouldn't get here */
	return 0;
}
