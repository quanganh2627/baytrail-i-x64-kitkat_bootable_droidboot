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
#define LOG_TAG "fastboot"

#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <pthread.h>

#include "droidboot.h"
#include "droidboot_ui.h"
#include "fastboot.h"
#include "droidboot_util.h"

struct fastboot_cmd {
	struct fastboot_cmd *next;
	const char *prefix;
	unsigned prefix_len;
	void (*handle) (const char *arg, void *data, unsigned sz);
};

struct fastboot_var {
	struct fastboot_var *next;
	const char *name;
	const char *value;
};

static struct fastboot_cmd *cmdlist;

void fastboot_register(const char *prefix,
		       void (*handle) (const char *arg, void *data,
				       unsigned sz))
{
	struct fastboot_cmd *cmd;
	cmd = malloc(sizeof(*cmd));
	if (cmd) {
		cmd->prefix = prefix;
		cmd->prefix_len = strlen(prefix);
		cmd->handle = handle;
		cmd->next = cmdlist;
		cmdlist = cmd;
	}
}

static struct fastboot_var *varlist;

void fastboot_publish(const char *name, const char *value)
{
	struct fastboot_var *var;
	var = malloc(sizeof(*var));
	if (var) {
		var->name = name;
		var->value = value;
		var->next = varlist;
		varlist = var;
	}
}

const char *fastboot_getvar(const char *name)
{
	struct fastboot_var *var;
	for (var = varlist; var; var = var->next)
		if (!strcmp(name, var->name))
			return (var->value);
	return NULL;
}

static unsigned char buffer[4096];

static void *download_base;
static unsigned download_max;
static unsigned download_size;

#define STATE_OFFLINE	0
#define STATE_COMMAND	1
#define STATE_COMPLETE	2
#define STATE_ERROR	3

static unsigned fastboot_state = STATE_OFFLINE;
int fb_fp = -1;
int enable_fp;

static int usb_read(void *_buf, unsigned len)
{
	int r = 0;
	unsigned xfer;
	unsigned char *buf = _buf;
	int count = 0;

	if (fastboot_state == STATE_ERROR)
		goto oops;

	pr_verbose("usb_read %d\n", len);
	while (len > 0) {
		xfer = (len > 4096) ? 4096 : len;

		r = read(fb_fp, buf, xfer);
		if (r < 0) {
			pr_perror("read");
			goto oops;
		}

		count += r;
		buf += r;
		len -= r;

		/* short transfer? */
		if ((unsigned int)r != xfer)
			break;
	}
	pr_verbose("usb_read complete\n");
	return count;

oops:
	fastboot_state = STATE_ERROR;
	return -1;
}

static int usb_write(void *buf, unsigned len)
{
	int r;

	if (fastboot_state == STATE_ERROR)
		goto oops;

	r = write(fb_fp, buf, len);
	if (r < 0) {
		pr_perror("write");
		goto oops;
	}

	return r;

oops:
	fastboot_state = STATE_ERROR;
	return -1;
}

void fastboot_ack(const char *code, const char *reason)
{
	char response[64];

	if (fastboot_state != STATE_COMMAND)
		return;

	if (reason == 0)
		reason = "";

	snprintf(response, 64, "%s%s", code, reason);
	fastboot_state = STATE_COMPLETE;

	usb_write(response, strlen(response));

}

void fastboot_fail(const char *reason)
{
	pr_error("ack FAIL %s\n", reason);
	fastboot_ack("FAIL", reason);
}

void fastboot_okay(const char *info)
{
	pr_debug("ack OKAY %s\n", info);
	fastboot_ack("OKAY", info);
}

static void cmd_getvar(const char *arg, void *data, unsigned sz)
{
	struct fastboot_var *var;

	pr_debug("fastboot: cmd_getvar %s\n", arg);
	for (var = varlist; var; var = var->next) {
		if (!strcmp(var->name, arg)) {
			fastboot_okay(var->value);
			return;
		}
	}
	fastboot_okay("");
}

static void cmd_download(const char *arg, void *data, unsigned sz)
{
	char response[64];
	unsigned len;
	int r;

	len = strtoul(arg, NULL, 16);
	pr_debug("fastboot: cmd_download %d bytes\n", len);

	download_size = 0;
	if (len > download_max) {
		fastboot_fail("data too large");
		return;
	}

	sprintf(response, "DATA%08x", len);
	if (usb_write(response, strlen(response)) < 0)
		return;

	r = usb_read(download_base, len);
	if ((r < 0) || ((unsigned int)r != len)) {
		pr_error("fastboot: cmd_download errro only got %d bytes\n", r);
		fastboot_state = STATE_ERROR;
		return;
	}
	download_size = len;
	fastboot_okay("");
}

static void fastboot_command_loop(void)
{
	struct fastboot_cmd *cmd;
	int r;
	pr_debug("fastboot: processing commands\n");

again:
	while (fastboot_state != STATE_ERROR) {
		memset(buffer, 0, 64);
		r = usb_read(buffer, 64);
		if (r < 0)
			break;
		buffer[r] = 0;
		pr_debug("fastboot got command: %s\n", buffer);

		for (cmd = cmdlist; cmd; cmd = cmd->next) {
			if (memcmp(buffer, cmd->prefix, cmd->prefix_len))
				continue;
			disable_autoboot();
			fastboot_state = STATE_COMMAND;
			ui_show_indeterminate_progress();
			pthread_mutex_lock(&action_mutex);
			cmd->handle((const char *)buffer + cmd->prefix_len,
				    (void *)download_base, download_size);
			pthread_mutex_unlock(&action_mutex);
			ui_reset_progress();
			if (fastboot_state == STATE_COMMAND)
				fastboot_fail("unknown reason");
			goto again;
		}
		pr_error("unknown command '%s'\n", buffer);
		fastboot_fail("unknown command");

	}
	fastboot_state = STATE_OFFLINE;
	pr_error("fastboot: oops!\n");
}


static int fastboot_handler(void *arg)
{
	for (;;) {
		fb_fp = open("/dev/android_adb", O_RDWR);
		if (fb_fp < 1) {
			pr_error("Can't open ADB device node (%s),"
					" trying again\n",
					strerror(errno));
			sleep(1);
			continue;
		}
		fastboot_command_loop();
		close(fb_fp);
		fb_fp = -1;
	}
	return 0;
}

int fastboot_init(unsigned size)
{
	pr_verbose("fastboot_init()\n");
	download_max = size;
	download_base = malloc(size);
	if (download_base == NULL) {
		pr_error("scratch malloc of %u failed in fastboot."
			" Unable to continue.\n\n", size);
		die();
	}

	fastboot_register("getvar:", cmd_getvar);
	fastboot_register("download:", cmd_download);
	fastboot_publish("version", "0.5");

	fastboot_handler(NULL);

	return 0;
}
