#ifndef _DROIDBOOT_H_
#define _DROIDBOOT_H_

#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>

#include <droidboot_fstab.h>

#define MEGABYTE	(1024 * 1024)

void disable_autoboot(void);
void start_default_kernel(void);

/* Inspect a volume looking for an automatic SW update. If it's
 * there, provision filesystems and apply it. */
int try_update_sw(Volume *vol, int use_countdown);

/* global libdiskconfig data structure representing the intended layout of
 * the internal disk, as read from /etc/disk_layout.conf */
extern struct disk_info *disk_info;

/* Serialize all disk operations. Grabbed by fastboot any time it is
 * performing a command, and also any worker thread handlers */
extern pthread_mutex_t action_mutex;

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
#define RECOVERY_FSTAB_LOCATION	"/system/etc/recovery.fstab"
#define DROIDBOOT_VERSION       "01.01"

/* In disk_layout.conf */
#define CACHE_PTN		"cache"
#define DATA_PTN		"userdata"
#define SECONDSTAGEBOOT_PTN	"2ndstageboot"

/* Volume entry in recovery.fstab for the SD card */
#define SDCARD_VOLUME		"/sdcard"
#define CACHE_VOLUME		"/cache"

#endif
