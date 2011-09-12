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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <diskconfig/diskconfig.h>

#include "fastboot.h"
#include "droidboot.h"
#include "droidboot_ui.h"

void die(void)
{
	LOGE("droidboot has encountered an unrecoverable problem, exiting!\n");
	exit(1);
}

int named_file_write(const char *filename, const unsigned char *what,
		size_t sz)
{
	int fd;
	int ret;
	fd = open(filename, O_RDWR | O_CREAT);
	if (fd < 0) {
		printf("file_write: Can't open file %s: %s\n",
				filename, strerror(errno));
		return -1;
	}

	while (sz) {
		ret = write(fd, what, sz);
		if (ret <= 0 && errno != EINTR) {
			printf("file_write: Failed to write to %s: %s\n",
					filename, strerror(errno));
			close(fd);
			return -1;
		}
		what += ret;
		sz -= ret;
	}
	close(fd);
	return 0;
}

int mount_partition(struct part_info *ptn)
{
	char *fstype;
	char *pdevice;
	char *mountpoint = NULL;
	int ret;
	int status = -1;

	pdevice = find_part_device(disk_info, ptn->name);
	if (!pdevice) {
		LOGPERROR("malloc");
		goto out;
	}

	/* FIXME: libdiskconfig partition types != filesystem types, but
	 * we'd change to change libdiskconfig to support that. Good
	 * enough for now. */
	switch (ptn->type) {
	case PC_PART_TYPE_LINUX:
		fstype = "ext4";
		break;
	default:
		fstype = "msdos";
	}

	ret = asprintf(&mountpoint, "/mnt/%s", ptn->name);
	if (ret < 0) {
		LOGPERROR("asprintf");
		goto out;
	}

	ret = mkdir(mountpoint, 0777);
	if (ret && errno != EEXIST) {
		LOGPERROR("mkdir");
		goto out;
	}

	LOGD("Mounting %s (%s) --> %s\n", pdevice,
			fstype, mountpoint);
	ret = mount(pdevice, mountpoint, fstype, MS_SYNCHRONOUS, "");
	if (ret && errno != EBUSY) {
		LOGPERROR("mount");
		goto out;
	}
	status = 0;
out:
	if (mountpoint)
		free(mountpoint);

	if (pdevice)
		free(pdevice);

	return status;
}


int execute_command(const char *cmd)
{
	int ret;

	LOGD("Executing: '%s'\n", cmd);
	ret = system(cmd);

	if (ret < 0) {
		LOGE("Error while trying to execute '%s': %s\n",
			cmd, strerror(errno));
		return ret;
	}
	ret = WEXITSTATUS(ret);
	LOGD("Done executing '%s' (retval=%d)\n", cmd, ret);

	return ret;
}


int is_valid_blkdev(const char *node)
{
	struct stat statbuf;
	if (stat(node, &statbuf)) {
		LOGPERROR("stat");
		return 0;
	}
	if (!S_ISBLK(statbuf.st_mode)) {
		LOGE("%s is not a block device", node);
		return 0;
	}
	return 1;
}


int kexec_linux(char *kernel, char *initrd, char *cmdline)
{
	char cmdline_buf[256];
	char kexec_cmd[512];
	int bytes_read;
	int ret;
	int fd;

	/* Read the kernel command line */
	fd = open(cmdline, O_RDONLY);
	if (fd < 0) {
		LOGE("can't open %s: %s", cmdline,
				strerror(errno));
		return -1;
	}
	bytes_read = read(fd, cmdline_buf, sizeof(cmdline_buf) - 1);
	if (bytes_read < 0) {
		LOGPERROR("read");
		return -1;
	}
	cmdline_buf[bytes_read] = '\0';

	/* Load the target kernel into RAM */
	snprintf(kexec_cmd, sizeof(kexec_cmd),
		"kexec -l %s --ramdisk=%s --command-line=\"%s\"",
		kernel, initrd, cmdline_buf);
	ret = execute_command(kexec_cmd);
	if (ret != 0) {
		LOGE("kexec load failed! (ret=%d)\n", ret);
		return -1;
	}
	fastboot_okay("");

	/* Pull the trigger */
	snprintf(kexec_cmd, sizeof(kexec_cmd),
		"kexec -e");
	sync();
	execute_command(kexec_cmd);

	/* Shouldn't get here! */
	LOGE("kexec failed!\n");
	return -1;
}



