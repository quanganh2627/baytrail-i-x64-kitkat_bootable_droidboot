/*
 * Copyright 2013 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "droidboot_installer.h"

int install_from_device(const char *device,
		const char *fs_type,
		int (*device_init) (const char *, int))
{
	int ret = -1;
	struct stat sb;

	if (!strlen(device)) {
		pr_error("installer device ignored\n");
		goto error;
	}

	/* initialize device if needed, using provided callback */
	if(device_init)
		if ( (ret = device_init(device, 1)) != 0 ) {
			pr_error("Failed to initialize device '%s'\n", device);
			goto error;
		}

	pr_info("Trying to install using device '%s'\n", device);

	mkdir("/installer/", 0600);

	if ( (ret = mount(device, "/installer", fs_type, 0, NULL)) < 0) {
		pr_error("Failed to mount device '%s' "
				"as installer partition using fs_type "
				"'%s'\n", device, fs_type);
		goto error;
	}

	if ( (ret = stat("/installer/installer.cmd", &sb)) == -1) {
		pr_error("Failed to stat installer.cmd file on "
				"device '%s'\n", device);
		ret = -1;
		goto error;
	}

	if (!sb.st_size) {
		pr_error("installer.cmd file is empty "
				"device '%s'\n", device);
		ret = -1;
		goto error;
	}

	pr_info("Valid installer medium found on '%s'\n", device);
	ret = 0;
error:
	return ret;
}

int installer_handle_cmd(struct fastboot_cmd *cmd, char *buffer)
{
	void *download_base;
	int download_size;
	char *file;
	int fd;
	struct stat sb;

	if (!cmd)
		return -1;

	if (!buffer)
		return -1;

	if (!strncmp(cmd->prefix, "flash:", sizeof("flash:"))) {
		file = strchr(buffer, '#');

		if (file == NULL) {
			pr_error("Invalid data with command\n");
			return -1;
		}

		*file = '\0';
		if ( (fd = open((file + 1), O_RDWR)) == -1)
			return -1;

		if (fstat(fd, &sb) == -1) {
			close(fd);
			return -1;
		}

		download_size = sb.st_size;
		download_base = mmap(NULL, download_size,
				PROT_READ|PROT_WRITE, MAP_FILE|MAP_SHARED, fd, 0);
		if (download_base == MAP_FAILED) {
			close(fd);
			return -1;
		}

		cmd->handle((const char *)buffer + cmd->prefix_len,
				download_base, download_size);
		munmap(download_base, download_size);
		close(fd);

	} else {
		download_base = NULL;
		download_size = 0;
		cmd->handle((const char *)buffer + cmd->prefix_len,
				download_base, download_size);
	}

	return 0;
}

void installer_install()
{
	FILE *fp = NULL;
	char buffer[BUFSIZ];
	struct fastboot_cmd *cmd;

	pr_error("Valid installer medium found.\n");

	if (! (fp = fopen("/installer/installer.cmd", "r"))) {
		pr_error("Failed to open "
				" /installer/installer.cmd file");
		return;
	}

	while(fgets(buffer, sizeof(buffer), fp)) {
		if (buffer[strlen(buffer)-1] == '\n')
			buffer[strlen(buffer)-1] = '\0';

		for (cmd = cmdlist; cmd; cmd = cmd->next) {
			if (memcmp(buffer, cmd->prefix, cmd->prefix_len))
				continue;

			if (installer_handle_cmd(cmd, buffer))
				break;
		}
	}

	fclose(fp);
}

void *installer_thread(void *arg)
{

	pr_info("Installer procedure started.");

	if (!install_from_device(g_installer_remote_dev, "nfs", NULL))
		goto installer_found;

	if (!install_from_device(g_installer_usb_dev, "vfat", NULL))
		goto installer_found;

	if (!install_from_device(g_installer_sdcard_dev, "vfat", NULL))
		goto installer_found;

	if (!install_from_device(g_installer_internal_dev, "vfat", NULL))
		goto installer_found;

	pr_error("No valid installer medium found.\n");
	return NULL;

installer_found:

	installer_install();

	if (umount("/installer") < 0) {
		pr_error("Failed to umount /installer\n");
	}
	return NULL;
}
