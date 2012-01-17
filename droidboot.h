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

/* If set, apply this update on 'fastboot continue' */
extern char *g_update_location;

#define DISK_CONFIG_LOCATION	"/system/etc/disk_layout.conf"
#define RECOVERY_FSTAB_LOCATION	"/system/etc/recovery.fstab"
#define DROIDBOOT_VERSION       "01.03"

/* In disk_layout.conf */
#define CACHE_PTN		"cache"
#define DATA_PTN		"userdata"

/* Volume entry in recovery.fstab for the SD card */
#define SDCARD_VOLUME		"/sdcard"
#define CACHE_VOLUME		"/cache"

#define MSEC_PER_SEC            (1000LL)

#define BATTERY_UNKNOWN_TIME    (2 * MSEC_PER_SEC)
#define POWER_ON_KEY_TIME       (2 * MSEC_PER_SEC)
#define UNPLUGGED_SHUTDOWN_TIME (30 * MSEC_PER_SEC)
#define CAPACITY_POLL_INTERVAL  (5 * MSEC_PER_SEC)

#endif
