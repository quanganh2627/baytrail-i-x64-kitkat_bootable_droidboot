#ifndef _DROIDBOOT_H_
#define _DROIDBOOT_H_

#include <errno.h>
#include <string.h>

#define MEGABYTE	(1024 * 1024)

void disable_autoboot(void);
void start_default_kernel(void);

/* global libdiskconfig data structure representing the intended layout of
 * the internal disk, as read from /etc/disk_layout.conf */
extern struct disk_info *disk_info;

/* Some configuration settings */
#ifndef USE_AUTOBOOT
#define USE_AUTOBOOT		0
#endif
#ifndef AUTOBOOT_DELAY_SECS
#define AUTOBOOT_DELAY_SECS	8
#endif
#ifndef SCRATCH_SIZE
#define SCRATCH_SIZE		(400 * MEGABYTE)
#endif

#define DISK_CONFIG_LOCATION	"/system/etc/disk_layout.conf"
#define DROIDBOOT_VERSION	"0.7"

#endif
