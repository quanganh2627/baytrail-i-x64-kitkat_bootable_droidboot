#ifndef _DROIDBOOT_H_
#define _DROIDBOOT_H_

#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>

#include <volumeutils/roots.h>
#include <volumeutils/mounts.h>

#define MEGABYTE	(1024 * 1024)

#define DISK_CONFIG_LOCATION	"/system/etc/disk_layout.conf"
#define RECOVERY_FSTAB_LOCATION	"/system/etc/recovery.fstab"
#define DROIDBOOT_VERSION       "01.04"

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
#define MODE_NON_CHARGER        0

#endif
