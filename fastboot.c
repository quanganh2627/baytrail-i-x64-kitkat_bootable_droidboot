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
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <pthread.h>
#include <cutils/properties.h>
#include <netinet/in.h>
#include <poll.h>

#include "volumeutils/roots.h"
#include "droidboot.h"
#include "droidboot_ui.h"
#include "fastboot.h"
#include "droidboot_util.h"

#define FILEMODE  S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH
#define MAGIC_LENGTH 64

extern int g_disable_fboot_ui;

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
static int tmp_fp = -1;
int enable_fp;

static int usb_read(void *_buf, unsigned len)
{
	int r = 0;
	unsigned xfer;
	unsigned char *buf = _buf;
	int count = 0;
	unsigned const len_orig = len;

	if (fastboot_state == STATE_ERROR)
		goto oops;

	pr_verbose("usb_read %d\n", len);
	while (len > 0) {
		xfer = (len > 4096) ? 4096 : len;

		r = read(fb_fp, buf, xfer);
		if (r < 0) {
			pr_warning("read");
			goto oops;
		} else if (r == 0) {
			pr_info("Connection closed\n");
			goto oops;
		}
		if (tmp_fp < 0) {
			buf += r;
		} else {
			/* we dont have enough memory in scratch
			   write directly in /cache */
			int w = 0, rv;
			while (w < r) {
				rv = write(tmp_fp, buf+w, r-w);
				if (rv == -1) {
					pr_perror("write to tmpfile");
					goto oops;
				}
				w += rv;
			}
		}
		count += r;
		len -= r;

		/* Fastboot proto is badly specified. Corner case where it will fail:
		 * file download is MAGIC_LENGTH. Transport has MTU < MAGIC_LENGTH.
		 * Will be necessary to retry after short read, but break below will
		 * prevent it.
		 */
		if (len_orig == MAGIC_LENGTH)
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

	pr_verbose("usb_write %d\n", len);
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
	char response[MAGIC_LENGTH];

	if (fastboot_state != STATE_COMMAND)
		return;

	if (reason == 0)
		reason = "";

	snprintf(response, MAGIC_LENGTH, "%s%s", code, reason);
	fastboot_state = STATE_COMPLETE;

	usb_write(response, strlen(response));

}

void fastboot_info(const char *info)
{
	char response[65];

	if (fastboot_state != STATE_COMMAND)
		return;

	snprintf(response, sizeof(response), "%s%s", "INFO", info);
	usb_write(response, strlen(response));
}

#define TEMP_BUFFER_SIZE		512
#define RESULT_FAIL_STRING		"RESULT: FAIL("
void fastboot_fail(const char *reason)
{
	char buf[TEMP_BUFFER_SIZE];

	sprintf(buf, RESULT_FAIL_STRING);
	strncat(buf, reason, TEMP_BUFFER_SIZE - 2 - strlen(RESULT_FAIL_STRING));
	strcat(buf, ")");

	if (!g_disable_fboot_ui) {
		ui_msg(ALERT, buf);
		ui_stop_process_bar();
	}
	fastboot_ack("FAIL", reason);
}

void fastboot_okay(const char *info)
{
	if (!g_disable_fboot_ui) {
		ui_msg(TIPS, "RESULT: OKAY");
		ui_stop_process_bar();
	}
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
	char response[MAGIC_LENGTH];
	unsigned len;
	int r;

	len = strtoul(arg, NULL, 16);
	if (!g_disable_fboot_ui)
		ui_print("RECEIVE DATA...\n");
	pr_debug("fastboot: cmd_download %d bytes\n", len);

	download_size = 0;
	if (len > download_max) {
		if (tmp_fp >=0 )
			close(tmp_fp);
		ensure_path_mounted(FASTBOOT_DOWNLOAD_TMP_FILE);
		tmp_fp = open(FASTBOOT_DOWNLOAD_TMP_FILE, O_RDWR | O_CREAT | O_TRUNC, FILEMODE);
		if (tmp_fp < 0) {
			fastboot_fail("unable to create download file");
			return;
		}
	}

	sprintf(response, "DATA%08x", len);
	if (usb_write(response, strlen(response)) < 0)
		return;

	r = usb_read(download_base, len);
	if ((r < 0) || ((unsigned int)r != len)) {
		pr_error("fastboot: cmd_download error only got %d bytes\n", r);
		fastboot_state = STATE_ERROR;
		return;
	}
	if (tmp_fp >=0) {
		close(tmp_fp);
		tmp_fp = -1;
		strcpy(download_base, FASTBOOT_DOWNLOAD_TMP_FILE);
		len = strlen(FASTBOOT_DOWNLOAD_TMP_FILE);
	}
	download_size = len;
	fastboot_okay("");
}

int fastboot_in_process = 0;
static void fastboot_command_loop(void)
{
	struct fastboot_cmd *cmd;
	int r;
	if (!g_disable_fboot_ui)
		ui_print("FASTBOOT CMD WAITING...\n");
	pr_debug("fastboot: processing commands\n");

again:
	while (fastboot_state != STATE_ERROR) {
		memset(buffer, 0, MAGIC_LENGTH);
		r = usb_read(buffer, MAGIC_LENGTH);
		if (r < 0)
			break;
		buffer[r] = 0;
		pr_debug("fastboot got command: %s\n", buffer);

		for (cmd = cmdlist; cmd; cmd = cmd->next) {
			if (memcmp(buffer, cmd->prefix, cmd->prefix_len))
				continue;
			fastboot_state = STATE_COMMAND;
			fastboot_in_process = 1;
			if (!g_disable_fboot_ui) {
				ui_set_screen_state(1);
				ui_msg(TIPS, "CMD(%s)...", buffer);
				ui_start_process_bar();
			}
			cmd->handle((const char *)buffer + cmd->prefix_len,
				    (void *)download_base, download_size);
			fastboot_in_process = 0;
			if (fastboot_state == STATE_COMMAND)
				fastboot_fail("unknown reason");
			goto again;
		}
		pr_error("unknown command '%s'\n", buffer);
		fastboot_fail("unknown command");

	}
	fastboot_state = STATE_OFFLINE;
	if (!g_disable_fboot_ui)
		ui_print("FASTBOOT OFFLINE!\n");
	pr_warning("fastboot: oops!\n");
}

static int open_tcp(void)
{
	pr_verbose("Beginning TCP init\n");
	int tcp_fd = -1;
	int portno = 1234;
	struct sockaddr_in serv_addr;

	pr_verbose("Allocating socket\n");
	tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (tcp_fd < 0) {
		pr_error("Socket creation failed: %s\n", strerror(errno));
		return -1;
	}

	memset(&serv_addr, sizeof(serv_addr), 0);
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	serv_addr.sin_port = htons(portno);
	pr_verbose("Binding socket\n");
	if (bind(tcp_fd, (struct sockaddr *) &serv_addr,
		 sizeof(serv_addr)) < 0) {
		pr_error("Bind failure: %s\n", strerror(errno));
		close(tcp_fd);
		return -1;
	}

	pr_verbose("Listening socket\n");
	if (listen(tcp_fd,5)) {
		pr_error("Listen failure: %s\n", strerror(errno));
		close(tcp_fd);
		return -1;
	}

	pr_info("Listening on TCP port %d\n", portno);
	return tcp_fd;
}

static int open_usb(void)
{
	int usb_fp;
	static int printed = 0;

	usb_fp = open("/dev/android_adb", O_RDWR);

	if (!printed) {
		if (usb_fp < 1)
			pr_info("Can't open ADB device node (%s),"
				" Listening on TCP only.\n",
				strerror(errno));
		else
			pr_info("Listening on /dev/android_adb\n");
		printed = 1;
	}

	return usb_fp;
}

static int fastboot_handler(void *arg)
{
	int usb_fd_idx = 0;
	int tcp_fd_idx = 1;
	int const nfds = 2;
	struct pollfd fds[nfds];

	memset(&fds, sizeof fds, 0);

	fds[usb_fd_idx].fd = -1;
	fds[tcp_fd_idx].fd = -1;

	for (;;) {
		if (fds[usb_fd_idx].fd == -1)
			fds[usb_fd_idx].fd = open_usb();
		if (fds[tcp_fd_idx].fd == -1)
			fds[tcp_fd_idx].fd = open_tcp();

		if (fds[usb_fd_idx].fd >= 0)
			fds[usb_fd_idx].events |= POLLIN;
		if (fds[tcp_fd_idx].fd >= 0)
			fds[tcp_fd_idx].events |= POLLIN;

		while (poll(fds, nfds, -1) == -1) {
			if (errno == EINTR)
				continue;
			pr_error("Poll failed: %s\n", strerror(errno));
			return -1;
		}

		if (fds[usb_fd_idx].revents & POLLIN) {
			fb_fp = fds[usb_fd_idx].fd;
			fastboot_command_loop();
			close(fb_fp);
			fds[usb_fd_idx].fd = -1;
		}

		if (fds[tcp_fd_idx].revents & POLLIN) {
			fb_fp = accept(fds[tcp_fd_idx].fd, NULL, NULL);
			if (fb_fp < 0)
				pr_error("Accept failure: %s\n", strerror(errno));
			else
				fastboot_command_loop();
			close(fb_fp);
		}
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

	/* We setup functional settings on USB to declare Fastboot Device */
	property_set("sys.usb.config", "adb");
	fastboot_handler(NULL);

	return 0;
}
