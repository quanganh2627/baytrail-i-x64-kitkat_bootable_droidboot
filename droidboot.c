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
#include <minui.h>

#include <diskconfig/diskconfig.h>

#include "aboot.h"
#include "util.h"
#include "droidboot.h"
#include "fastboot.h"
#include "droidboot_ui.h"
#include "fstab.h"

/* libdiskconfig data structure representing the intended layout of the
 * internal disk, as read from /etc/disk_layout.conf */
struct disk_info *disk_info;

/* Synchronize operations which touch EMMC. Fastboot holds this any time it
 * executes a command. Threads which touch the disk should do likewise. */
pthread_mutex_t action_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Not bothering with concurrency control as this is just a flag
 * that gets cleared */
static int autoboot_enabled = USE_AUTOBOOT;

#define AUTOBOOT_FRAC   (1.0 / AUTOBOOT_DELAY_SECS)


/* Ensure the device's disk is set up in a sane way, such that it's possible
 * to apply a full OTA update */
static void provisioning_checks(void)
{
	struct part_info *cacheptn, *dataptn;

	LOGD("Preparing device for provisioning...");

	/* Set up the partition table */
	if (apply_disk_config(disk_info, 0)) {
		LOGE("Couldn't apply disk configuration");
		goto out;
	}

	/* Format /cache and /data partitions */
	cacheptn = find_part(disk_info, "cache");
	if (!cacheptn) {
		LOGE("Couldn't find cache partition. Is your "
				"disk_layout.conf valid?");
		goto out;
	}
	dataptn = find_part(disk_info, "userdata");
	if (!dataptn) {
		LOGE("Couldn't find data partition. Is your "
				"disk_layout.conf valid?");
		goto out;
	}
	if (erase_partition(dataptn))
		goto out;
	if (erase_partition(cacheptn))
		goto out;
	return;
out:
	/* Basic things that should work didn't. Time to freak out */
	die();
}

/* Volume entry in recovery.fstab for the SD card */
#define SDCARD_VOLUME		"/sdcard"

/* Location SD card gets mounted */
#define SDCARD_MOUNT_DIR	"/mnt/sdcard"

static char *detect_sw_update(void)
{
	Volume *sdv;
	char *filename;
	struct stat statbuf;
	char *ret = NULL;

	LOGI("Checking for SW update package %s.auto-ota.zip on SD card...",
			DEVICE_NAME);
	sdv = volume_for_path(SDCARD_VOLUME);
	if (!sdv) {
		LOGD("/sdcard not defined in recovery fstab");
		return NULL;
	}

	if (asprintf(&filename, "%s/%s.auto-ota.zip", SDCARD_MOUNT_DIR,
				DEVICE_NAME) < 0) {
		LOGPERROR("asprintf");
		die();
	}

	if (mount_partition_device(sdv->device, sdv->fs_type,
			SDCARD_MOUNT_DIR)) {
		if (!sdv->device2 || mount_partition_device(sdv->device2,
				sdv->fs_type, SDCARD_MOUNT_DIR))
		{
			LOGI("Couldn't mount SD card or no card inserted");
			return NULL;
		}
	}

	if (stat(filename, &statbuf)) {
		if (errno == ENOENT)
			LOGI("Coudln't find %s", filename);
		else
			LOGPERROR("stat");
		free(filename);
	} else {
		ret = filename;
		LOGI("OTA Update package found: %s", filename);
	}

	umount(SDCARD_MOUNT_DIR);
	return ret;
}


static void *autoupdate_thread(void *arg)
{
	unsigned int sleep_time = AUTOBOOT_DELAY_SECS;
	char *update_location = (char *)arg;

	ui_show_text(1);
	LOGI("Detected SD card automatic SW update: %s", update_location);
	for (; sleep_time; sleep_time--) {
		LOGI("Automatic recovery mode reboot in %d seconds.\n", sleep_time);
		sleep(1);
		if (!autoboot_enabled) {
			return NULL;
		}
	}

	/* Apply SW update */
	pthread_mutex_lock(&action_mutex);
	provisioning_checks();
	apply_sw_update(update_location + 4, 0);
	pthread_mutex_unlock(&action_mutex);

	/* Shouldn't get here */
	LOGE("Failed to apply SW update in %s", update_location);
	return NULL;
}


static void *autoboot_thread(void *arg)
{
	unsigned int sleep_time = AUTOBOOT_DELAY_SECS;

	ui_show_progress(1.0, AUTOBOOT_DELAY_SECS);

	for (; sleep_time; sleep_time--) {
		LOGI("Automatic boot in %d seconds.\n", sleep_time);
		sleep(1);
		if (!autoboot_enabled) {
			ui_reset_progress();
			return NULL;
		}
	}
	ui_reset_progress();
	ui_show_text(1);
	start_default_kernel();

	return NULL;
}


static void *input_listener_thread(void *arg)
{
	LOGV("begin input listener thread\n");

	while (1) {
		struct input_event event;
		if (ev_get(&event, 0)) {
			LOGPERROR("ev_get");
			break;
		}
		LOGI("Event type: %x, code: %x, value: %x\n",
				event.type, event.code,
				event.value);
		switch (event.type) {
		case EV_KEY:
		case EV_REL:
			disable_autoboot();
			goto out;
		default:
			continue;
		}
	}
out:
	LOGV("exit input listener thread\n");

	return NULL;
}


void disable_autoboot(void)
{
	if (autoboot_enabled) {
		autoboot_enabled = 0;
		LOGD("Automatic boot action disabled.\n");
	}
}


void start_default_kernel(void)
{
	struct part_info *ptn;
	ptn = find_part(disk_info, "2ndstageboot");

	if (mount_partition(ptn)) {
		LOGE("Can't mount second-stage boot partition!\n");
		return;
	}

	kexec_linux("/mnt/2ndstageboot/kernel",
			"/mnt/2ndstageboot/ramdisk.img",
			"/mnt/2ndstageboot/cmdline");
	/* Failed if we get here */
	LOGE("kexec failed");
}


int main(int argc, char **argv)
{
	char *config_location;
	char *update_fn;
	pthread_t t_auto, t_input;

	ui_init();
	ev_init();

	ui_set_background(BACKGROUND_ICON_INSTALLING);

	LOGI(" -- Droidboot %s for %s --\n", DROIDBOOT_VERSION, DEVICE_NAME);
	if (argc > 1)
		config_location = argv[1];
	else
		config_location = DISK_CONFIG_LOCATION;

	/* Read the recovery.fstab, which is used to determine the device
	 * node for the SD card */
	load_volume_table();

	LOGD("Reading disk layout from %s\n", config_location);
	disk_info = load_diskconfig(config_location, NULL);
	dump_disk_config(disk_info);

	aboot_register_commands();

	update_fn = detect_sw_update();
	if (update_fn) {
		autoboot_enabled = 1;
		if (pthread_create(&t_auto, NULL, autoupdate_thread,
					update_fn)) {
			LOGPERROR("pthread_create");
			die();
		}
	} else if (autoboot_enabled) {
		if (pthread_create(&t_auto, NULL, autoboot_thread, NULL)) {
			LOGPERROR("pthread_create");
			die();
		}
	}
	if (autoboot_enabled) {
		if (pthread_create(&t_input, NULL, input_listener_thread,
					NULL)) {
			LOGPERROR("pthread_create");
			die();
		}
	}

	LOGI("Listening for the fastboot protocol "
			"on the USB OTG.\n");

	fastboot_init(SCRATCH_SIZE);

	/* Shouldn't get here */
	exit(1);
}
