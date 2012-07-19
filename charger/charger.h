/*
 * Copyright (C) 2011 The Android Open Source Project
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

#ifndef _CHARGER_H_
#define _CHARGER_H_

#include <stdint.h>

#include <cutils/list.h>

#include "minui/minui.h"

enum charger_exit_state {
    CHARGER_CHARGE,
    CHARGER_SHUTDOWN,
    CHARGER_PROCEED
};

#define MODE_CHARGER            1

#define SYS_TEMP_INT   "/sys/class/thermal/thermal_zone0/temp"
#define TEMP_CONF_FILE "/etc/MID_thermal.conf"
#define TEMP_ZONE      "<platform-config>"
#define TEMP_NAME      "RECOVERY_OS_THERMAL_SHUTDOWN"
#define DEFAULT_TEMP_THRESH 73000

#define RTC_FILE                 "/dev/rtc0"
#define IPC_DEVICE_NAME          "/dev/mid_ipc"
#define IPC_WRITE_ALARM_TO_OSNIB 0xC5
#define ALARM_SET                1
#define ALARM_CLEAR              0

/* Begin battery charging animation.
 *
 * Preconditions:
 * - libminui gr_init() must have been run
 * - klog_init() must have been run if you want log messages in the dmesg
 * - Any existing libminui ev_init() callbacks will be removed when this
 * function runs
 *
 * Parameters:
 * - min_charge: Minimum battery level to reach before exiting. If the
 * battery level is already greater than or equal to this value, this
 * returns immediately with CHARGER_PROCEED. Set to 0 if there is no
 * minimum level.
 * - power_key_ms: Number of milliseconds the power button must be held
 * to exit with CHARGER_PROCEED. This input is ignored if min_charge is
 * specified and the battery level is below it. Set to -1 to disable this
 * feature.
 * - batt_unknown_ms: For the case where the battery fuel gauge data cannot
 * be read, how long in milliseconds to show the 'unknown battery' graphic
 * before blanking the screen
 * - unplug_shutdown_ms: How long to wait when power is disconnected to
 * exit with CHARGER_SHUTDOWN. Set to -1 to disable this feature.
 * - cap_poll_ms: If min_charge is set, interval to poll the battery capacity
 * to check whether it meets min_charge
 *
 * Return Value:
 * CHARGER_SHUTDOWN: The device should be powered off
 * CHARGER_PROCEED: The device should proceed into Android or whatever
 * process was being blocked by min_charge. For the latter case (min_charge
 * set), the display is automatically un-blanked.
 */
enum charger_exit_state charger_run(int min_charge, int mode, int power_key_ms,
        int batt_unknown_ms, int unplug_shutdown_ms, int cap_poll_ms);

#endif
