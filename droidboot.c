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
#include <unistd.h>
#include <sys/mount.h>

#include <minui/minui.h>
#include <cutils/android_reboot.h>
#include <cutils/klog.h>
#include <charger/charger.h>
#include <diskconfig/diskconfig.h>

#include "aboot.h"
#include "droidboot_util.h"
#include "droidboot.h"
#include "fastboot.h"
#include "droidboot_ui.h"
#include "droidboot_fstab.h"

/* Generated by the makefile, this function defines the
 * RegisterDeviceExtensions() function, which calls all the
 * registration functions for device-specific extensions. */
#include "register.inc"

/* NOTE: Droidboot uses two sources of information about the disk. There
 * is disk_layout.conf which specifies the physical partition layout on
 * the disk via libdiskconfig. There is also recovery.fstab which gives
 * detail on the filesystems associates with these partitions, see fstab.c.
 * The device node is used to link these two sources when necessary; the
 * 'name' fields are typically not the same.
 *
 * It would be best to have this in a single data store, but we wanted to
 * leverage existing android mechanisms whenever possible, there are already
 * too many different places in the build where filesystem data is recorded.
 * So there is a little bit of ugly gymnastics involved when both sources
 * need to be used.
 */

/* libdiskconfig data structure representing the intended layout of the
 * internal disk, as read from /etc/disk_layout.conf */
struct disk_info *disk_info;


/* Default size of memory buffer for image data */
static int g_scratch_size = 400;

/* Minimum battery % before we do anything */
static int g_min_battery = 10;


static int input_callback(int fd, short revents, void *data)
{
	struct input_event ev;
	int ret;

	ret = ev_get_input(fd, revents, &ev);
	if (ret)
		return -1;

	pr_verbose("Event type: %x, code: %x, value: %x\n",
				ev.type, ev.code,
				ev.value);

	switch (ev.type) {
		case EV_KEY:
			break;
		default:
			break;
	}
	return 0;
}


static void *input_listener_thread(void *arg)
{
	pr_verbose("begin input listener thread\n");

	while (1) {
		if (!ev_wait(-1))
			ev_dispatch();
	}
	pr_verbose("exit input listener thread\n");

	return NULL;
}

void setup_disk_information(char *disk_layout_location)
{
	/* Read the recovery.fstab, which is used to for filesystem
	 * meta-data and also the sd card device node */
	load_volume_table();

	/* Read disk_layout.conf, which provides physical partition
	 * layout information */
	pr_debug("Reading disk layout from %s\n", disk_layout_location);
	disk_info = load_diskconfig(disk_layout_location, NULL);
	if (!disk_info) {
		pr_error("Disk layout unreadable");
		die();
	}
	process_disk_config(disk_info);
	dump_disk_config(disk_info);

	/* Set up the partition table */
	if (apply_disk_config(disk_info, 0)) {
		pr_error("Couldn't apply disk configuration");
		die();
	}
}


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
	} else {
		pr_error("Unknown parameter %s, ignoring\n", name);
	}
}


int main(int argc, char **argv)
{
	char *config_location;
	pthread_t t_input;

	/* initialize libminui */
	ui_init();

	pr_info(" -- Droidboot %s for %s --\n", DROIDBOOT_VERSION, DEVICE_NAME);
	import_kernel_cmdline(parse_cmdline_option);

#ifdef USE_GUI
	/* Enforce a minimum battery level */
	if (g_min_battery != 0) {
		pr_info("Verifying battery level >= %d%% before continuing\n",
				g_min_battery);
		klog_init();
		klog_set_level(8);

		switch (charger_run(g_min_battery, POWER_ON_KEY_TIME,
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
		ev_exit();
	}
#endif

	ev_init(input_callback, NULL);
	ui_set_background(BACKGROUND_ICON_INSTALLING);
	ui_show_text(1);

	if (argc > 1)
		config_location = argv[1];
	else
		config_location = DISK_CONFIG_LOCATION;

	setup_disk_information(config_location);

	aboot_register_commands();

	register_droidboot_plugins();

	if (pthread_create(&t_input, NULL, input_listener_thread,
					NULL)) {
		pr_perror("pthread_create");
		die();
	}

	pr_info("Listening for the fastboot protocol over USB.");
	fastboot_init(g_scratch_size * MEGABYTE);

	/* Shouldn't get here */
	exit(1);
}
