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

#include <errno.h>
#include <fcntl.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cutils/android_reboot.h>
#include <cutils/hashmap.h>
#include <diskconfig/diskconfig.h>

#include "fastboot.h"
#include "droidboot.h"
#include "droidboot_util.h"
#include "droidboot_plugin.h"
#include "droidboot_ui.h"

#define CMD_SYSTEM		"system"
#define CMD_PARTITION		"partition"
#define CMD_SHOWTEXT		"showtext"

Hashmap *flash_cmds;

static bool strcompare(void *keyA, void *keyB)
{
	return !strcmp(keyA, keyB);
}

static int strhash(void *key)
{
	return hashmapHash(key, strlen((char *)key));
}

static int aboot_register_cmd(Hashmap *map, char *key, void *callback)
{
	char *k;

	k = strdup(key);
	if (!k) {
		pr_error("memory allocation error!");
		return -1;
	}
	if (hashmapGet(map, k)) {
		pr_error("key collision '%s'", k);
		free(k);
		return -1;
	}
	hashmapPut(map, k, callback);
	pr_verbose("Registered plugin function %p (%s) with table %p",
			callback, k, map);
	return 0;
}

int aboot_register_flash_cmd(char *key, flash_func callback)
{
	return aboot_register_cmd(flash_cmds, key, callback);
}

/* Erase a named partition by creating a new empty partition on top of
 * its device node. No parameters. */
static void cmd_erase(const char *part_name, void *data, unsigned sz)
{
	struct part_info *ptn;

	pr_info("%s: %s\n", __func__, part_name);
	ptn = find_part(disk_info, part_name);
	if (ptn == NULL) {
		fastboot_fail("unknown partition name");
		return;
	}

	pr_debug("Erasing %s.\n", part_name);
	if (erase_partition(ptn))
		fastboot_fail("Can't erase partition");
	else
		fastboot_okay("");

}


static void do_sw_update(void *data, unsigned sz)
{
	struct part_info *cacheptn;

	cacheptn = find_part(disk_info, CACHE_PTN);
	if (!cacheptn) {
		pr_error("Couldn't find " CACHE_PTN " partition. Is your "
				"disk_layout.conf valid?");
		return;
	}
	if (mount_partition(cacheptn)) {
		pr_error("Couldn't mount " CACHE_PTN "partition");
		return;
	}
	/* Remove any old copy hanging around */
	unlink("/mnt/" CACHE_PTN "/droidboot.update.zip");

	/* Once the update is applied this file is deleted */
	if (named_file_write("/mnt/" CACHE_PTN "/droidboot.update.zip",
				data, sz)) {
		pr_error("Couldn't write update package to " CACHE_PTN
				" partition.");
		unmount_partition(cacheptn);
		return;
	}
	unmount_partition(cacheptn);
	apply_sw_update(CACHE_VOLUME "/droidboot.update.zip", 1);
}

/* Image command. Allows user to send a single gzipped file which
 * will be decompressed and written to a destination location. Typical
 * usage is to write to a disk device node, in order to flash a raw
 * partition, but can be used to write any file.
 *
 * The parameter part_name can be one of several possibilities:
 *
 * "disk" : Write directly to the disk node specified in disk_layout.conf,
 *          whatever it is named there.
 * "update" : Writes a signed OTA Update package to the data partition,
 *            writes the command file for the recovery console, and
 *            reboots into recovery console to apply the update
 * <name> : Look in the flash_cmds table and execute the callback function.
 *          If not found, lookup the named partition in disk_layout.conf 
 *          and write to its corresponding device node
 */
static void cmd_flash(const char *part_name, void *data, unsigned sz)
{
	FILE *fp = NULL;
	char *cmd = NULL;
	char *device;
	char *cmd_base;
	struct part_info *ptn = NULL;
	unsigned char *data_bytes = (unsigned char *)data;
	int free_device = 0;
	int do_ext_checks = 0;
	flash_func cb;

	if (!strcmp(part_name, "disk")) {
		device = disk_info->device;
	} else if (!strcmp(part_name, "update")) {
		do_sw_update(data, sz);
		fastboot_fail("could not stage software update package");
		return;
	} else if ( (cb = hashmapGet(flash_cmds, (char *)part_name)) ) {
		/* Use our table of flash functions registered by platform
		 * specific plugin libraries */
		int cbret;
		cbret = cb(data, sz);
		if (cbret) {
			pr_error("%s flash failed!\n", part_name);
			fastboot_fail(part_name);
		} else
			fastboot_okay("");
		return;
	} else {
		free_device = 1;
		device = find_part_device(disk_info, part_name);
		if (!device) {
			fastboot_fail("unknown partition specified");
			return;
		}
		ptn = find_part(disk_info, part_name);
	}

	pr_debug("Writing %u bytes to destination device: %s\n", sz, device);
	if (!is_valid_blkdev(device)) {
		fastboot_fail("invalid destination node. partition disks?");
		goto out;
	}

	/* Check for a gzip header, and use gzip to decompress if present.
	 * See http://www.gzip.org/zlib/rfc-gzip.html#file-format */
	if (sz > 2 && data_bytes[0] == 0x1f &&
			data_bytes[1] == 0x8b && data_bytes[3] == 8) {
		cmd_base =
		    "/system/bin/gzip -c -d | /system/bin/dd of=%s bs=8192";
	} else {
		cmd_base = "/system/bin/dd of=%s bs=8192";
	}

	if (asprintf(&cmd, cmd_base, device) < 0) {
		pr_perror("asprintf");
		cmd = NULL;
		fastboot_fail("memory allocation error");
		goto out;
	}

	pr_verbose("command: %s\n", cmd);
	fp = popen(cmd, "w");
	if (!fp) {
		pr_perror("popen");
		fastboot_fail("popen failure");
		goto out;
	}
	free(cmd);
	cmd = NULL;

	if (sz != fwrite(data, 1, sz, fp)) {
		pr_perror("fwrite");
		fastboot_fail("image write failure");
		goto out;
	}
	pclose(fp);
	fp = NULL;
	sync();

	pr_debug("wrote %u bytes to %s\n", sz, device);

	/* Check if we wrote to the base device node. If so,
	 * re-sync the partition table in case we wrote out
	 * a new one */
	if (!strcmp(device, disk_info->device)) {
		int fd = open(device, O_RDWR);
		if (fd < 0) {
			fastboot_fail("could not open device node");
			goto out;
		}
		pr_verbose("sync partition table\n");
		ioctl(fd, BLKRRPART, NULL);
		close(fd);
	}

	/* Make sure this is really an ext4 partition before we try to
	 * run some disk checks and resize it, ptn->type isn't sufficient
	 * information */
	if (ptn && ptn->type == PC_PART_TYPE_LINUX) {
		if (check_ext_superblock(ptn, &do_ext_checks)) {
			fastboot_fail("couldn't check superblock");
			goto out;
		}
	}
	if (do_ext_checks) {
		if (ext4_filesystem_checks(device)) {
			fastboot_fail("ext4 filesystem error");
			goto out;
		}
	}

	fastboot_okay("");
out:
	if (fp)
		pclose(fp);
	if (cmd)
		free(cmd);
	if (device && free_device)
		free(device);
}

static void cmd_oem(const char *arg, void *data, unsigned sz)
{
	const char *command;
	pr_verbose("%s: <%s>\n", __FUNCTION__, arg);

	while (*arg == ' ')
		arg++;
	command = arg;

	if (strncmp(command, CMD_SYSTEM, strlen(CMD_SYSTEM)) == 0) {
		int retval;
		arg += strlen(CMD_SYSTEM);
		while (*arg == ' ')
			arg++;
		retval = execute_command(arg);
		if (retval != 0) {
			pr_error("\nfails: %s (return value %d)\n", arg, retval);
			fastboot_fail("OEM system command failed");
		} else {
			pr_verbose("\nsucceeds: %s\n", arg);
			fastboot_okay("");
		}
	} else if (strncmp(command, CMD_PARTITION,
				strlen(CMD_PARTITION)) == 0) {
		/* FIXME: Done at boot, no-op. Remove this command! */
		fastboot_okay("");
	} else if (strncmp(command, CMD_SHOWTEXT,
				strlen(CMD_SHOWTEXT)) == 0) {
		ui_show_text(1);
		fastboot_okay("");
	} else {
		fastboot_fail("unknown OEM command");
	}
	return;
}

static void cmd_boot(const char *arg, void *data, unsigned sz)
{
	fastboot_fail("boot command stubbed on this platform!");
}

static void cmd_reboot(const char *arg, void *data, unsigned sz)
{
	fastboot_okay("");
	sync();
	pr_info("Rebooting!\n");
	android_reboot(ANDROID_RB_RESTART, 0, 0);
	pr_error("Reboot failed");
}

static void cmd_reboot_bl(const char *arg, void *data, unsigned sz)
{
	fastboot_okay("");
	sync();
	pr_info("Restarting Droidboot...\n");
	android_reboot(ANDROID_RB_RESTART2, 0, "fastboot");
	pr_error("Reboot failed");
}

static void cmd_continue(const char *arg, void *data, unsigned sz)
{
	start_default_kernel();
	fastboot_fail("Unable to boot default kernel!");
}

void aboot_register_commands(void)
{
	fastboot_register("oem", cmd_oem);
	fastboot_register("boot", cmd_boot);
	fastboot_register("reboot", cmd_reboot);
	fastboot_register("reboot-bootloader", cmd_reboot_bl);
	fastboot_register("erase:", cmd_erase);
	fastboot_register("flash:", cmd_flash);
	fastboot_register("continue", cmd_continue);

	fastboot_publish("product", DEVICE_NAME);
	fastboot_publish("kernel", "droidboot");
	fastboot_publish("droidboot", DROIDBOOT_VERSION);

	flash_cmds = hashmapCreate(8, strhash, strcompare);
	if (!flash_cmds) {
		pr_error("Memory allocation error");
		die();
	}
}
