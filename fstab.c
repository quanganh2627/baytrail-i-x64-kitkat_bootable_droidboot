/*
 * Copyright (C) 2007 The Android Open Source Project
 * Copyright (C) 2011 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <ctype.h>
#include <stdio.h>

#include "droidboot_fstab.h"
#include "droidboot.h"
#include "droidboot_ui.h"
#include "droidboot_util.h"

static int num_volumes = 0;
static Volume *device_volumes = NULL;

static int parse_options(char *options, Volume * volume)
{
	char *option;
	while ((option = strtok(options, ","))) {
		options = NULL;

		if (strncmp(option, "length=", 7) == 0) {
			volume->length = strtoll(option + 7, NULL, 10);
		} else {
			pr_error("bad option \"%s\"\n", option);
			return -1;
		}
	}
	return 0;
}

void load_volume_table()
{
	int alloc = 2;
	device_volumes = malloc(alloc * sizeof(Volume));
        if (!device_volumes) {
		pr_perror("malloc");
		die();
	}

	// Insert an entry for /tmp, which is the ramdisk and is always mounted.
	device_volumes[0].mount_point = "/tmp";
	device_volumes[0].fs_type = "ramdisk";
	device_volumes[0].device = NULL;
	device_volumes[0].device2 = NULL;
	device_volumes[0].length = 0;
	num_volumes = 1;

	FILE *fstab = fopen(RECOVERY_FSTAB_LOCATION, "r");
	if (fstab == NULL) {
		pr_error("failed to open %s (%s)\n",
		     RECOVERY_FSTAB_LOCATION, strerror(errno));
		return;
	}

	char buffer[1024];
	int i;
	while (fgets(buffer, sizeof(buffer) - 1, fstab)) {
		for (i = 0; buffer[i] && isspace(buffer[i]); ++i) ;
		if (buffer[i] == '\0' || buffer[i] == '#')
			continue;

		char *mount_point = strtok(buffer + i, " \t\n");
		char *fs_type = strtok(NULL, " \t\n");
		char *device = strtok(NULL, " \t\n");
		// lines may optionally have a second device, to use if
		// mounting the first one fails.
		char *options = NULL;
		char *device2 = strtok(NULL, " \t\n");
		if (device2) {
			if (device2[0] == '/') {
				options = strtok(NULL, " \t\n");
			} else {
				options = device2;
				device2 = NULL;
			}
		}

		if (mount_point && fs_type && device) {
			while (num_volumes >= alloc) {
				alloc *= 2;
				device_volumes =
				    realloc(device_volumes,
					    alloc * sizeof(Volume));
			}
			device_volumes[num_volumes].mount_point =
			    strdup(mount_point);
			device_volumes[num_volumes].fs_type = strdup(fs_type);
			device_volumes[num_volumes].device = strdup(device);
			device_volumes[num_volumes].device2 =
			    device2 ? strdup(device2) : NULL;

			device_volumes[num_volumes].length = 0;
			if (parse_options(options, device_volumes + num_volumes)
			    != 0) {
				pr_error("skipping malformed recovery.fstab line: %s\n", buffer);
			} else {
				++num_volumes;
			}
		} else {
			pr_error("skipping malformed recovery.fstab line: %s\n",
			     buffer);
		}
	}

	fclose(fstab);

	printf("recovery filesystem table\n");
	printf("=========================\n");
	for (i = 0; i < num_volumes; ++i) {
		Volume *v = &device_volumes[i];
		printf("  %d %s %s %s %s %lld\n", i, v->mount_point, v->fs_type,
		       v->device, v->device2, v->length);
	}
	printf("\n");
}

Volume *volume_for_path(const char *path)
{
	int i;
	for (i = 0; i < num_volumes; ++i) {
		Volume *v = device_volumes + i;
		int len = strlen(v->mount_point);
		if (strncmp(path, v->mount_point, len) == 0 &&
		    (path[len] == '\0' || path[len] == '/')) {
			return v;
		}
	}
	return NULL;
}

Volume *volume_for_device(const char *device)
{
	int i;
	for (i = 0; i < num_volumes; ++i) {
		Volume *v = device_volumes + i;
		if (!v->device)
			continue;
		if (!strcmp(device, v->device) ||
		    (v->device2 && !strcmp(device, v->device2))) {
			return v;
		}
	}
	return NULL;
}
