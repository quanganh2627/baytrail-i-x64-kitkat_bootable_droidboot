/*
 * Copyright (C) 2007 The Android Open Source Project
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

/*
 * This is a simplified subset of the ui code in bootable/recovery/
 */

#ifndef _DROIDBOOT_UI_H_
#define _DROIDBOOT_UI_H_
#include "logd.h"

#ifndef LOG_TAG
#define LOG_TAG "droidboot"
#endif
#define VERBOSE_DEBUG 0

#define pr_perror(x)	pr_error("%s failed: %s\n", x, strerror(errno))

#define LOGW(format, ...) \
    __libc_android_log_print(ANDROID_LOG_WARN, LOG_TAG, (format), ##__VA_ARGS__ )
#define LOGI(format, ...) \
    __libc_android_log_print(ANDROID_LOG_INFO, LOG_TAG, (format), ##__VA_ARGS__ )
#define LOGV(format, ...) \
    __libc_android_log_print(ANDROID_LOG_VERBOSE, LOG_TAG, (format), ##__VA_ARGS__ )
#define LOGD(format, ...) \
    __libc_android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, (format), ##__VA_ARGS__ )
#define pr_warning(format, ...) \
	__libc_android_log_print(ANDROID_LOG_WARN, LOG_TAG, (format), ##__VA_ARGS__ )
#define pr_info(format, ...) \
	__libc_android_log_print(ANDROID_LOG_INFO, LOG_TAG, (format), ##__VA_ARGS__ )
#define pr_debug(format, ...) \
	__libc_android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, (format), ##__VA_ARGS__ )

#if VERBOSE_DEBUG
#define pr_verbose(format, ...) \
	__libc_android_log_print(ANDROID_LOG_VERBOSE, LOG_TAG, (format), ##__VA_ARGS__ )
#else
#define pr_verbose(format, ...)				do { } while (0)
#endif

#ifdef USE_GUI

#define LOGE(format, ...) \
    do { \
        ui_print("E:" format, ##__VA_ARGS__); \
        __libc_android_log_print(ANDROID_LOG_ERROR, LOG_TAG, (format), ##__VA_ARGS__ ); \
    } while (0)
#define pr_error(format, ...) \
    do { \
        ui_print("E:" format, ##__VA_ARGS__); \
        __libc_android_log_print(ANDROID_LOG_ERROR, LOG_TAG, (format), ##__VA_ARGS__ ); \
    } while (0)

// Initialize the graphics system.
void ui_init();

// Write a message to the on-screen log shown with Alt-L (also to stderr).
// The screen is small, and users may need to report these messages to support,
// so keep the output short and not too cryptic.
void ui_print(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Set the icon (normally the only thing visible besides the progress bar).
enum {
	BACKGROUND_ICON_NONE,
	BACKGROUND_ICON_INSTALLING,
	BACKGROUND_ICON_ERROR,
	NUM_BACKGROUND_ICONS
};
void ui_set_background(int icon);

// Show a progress bar and define the scope of the next operation:
//   portion - fraction of the progress bar the next operation will use
//   seconds - expected time interval (progress bar moves at this minimum rate)
void ui_show_progress(float portion, int seconds);
void ui_set_progress(float fraction);  // 0.0 - 1.0 within the defined scope

// Default allocation of progress bar segments to operations
static const int VERIFICATION_PROGRESS_TIME = 60;
static const float VERIFICATION_PROGRESS_FRACTION = 0.25;
static const float DEFAULT_FILES_PROGRESS_FRACTION = 0.4;
static const float DEFAULT_IMAGE_PROGRESS_FRACTION = 0.1;

// Show a rotating "barberpole" for ongoing operations.  Updates automatically.
void ui_show_indeterminate_progress();

// Hide and reset the progress bar.
void ui_reset_progress();

void ui_show_text(int visible);

typedef struct {
	// number of frames in indeterminate progress bar animation
	int indeterminate_frames;

	// number of frames per second to try to maintain when animating
	int update_fps;

	// number of frames in installing animation.  may be zero for a
	// static installation icon.
	int installing_frames;

	// the install icon is animated by drawing images containing the
	// changing part over the base icon.  These specify the
	// coordinates of the upper-left corner.
	int install_overlay_offset_x;
	int install_overlay_offset_y;
} UIParameters;

#else /* !USE_GUI */

#define LOGE(format, ...) \
	__libc_android_log_print(ANDROID_LOG_ERROR, LOG_TAG, (format), ##__VA_ARGS__ );
#define pr_error(format, ...) \
	__libc_android_log_print(ANDROID_LOG_ERROR, LOG_TAG, (format), ##__VA_ARGS__ );

#define ui_init()				do { } while (0)
#define ui_print				pr_info
#define ui_set_background(x)			do { } while (0)
#define ui_show_progress(x, y)			do { } while (0)
#define ui_set_progress(x)			do { } while (0)
#define ui_show_indeterminate_progress()	do { } while (0)
#define ui_reset_progress()			do { } while (0)
#define ui_show_text(x)				do { } while (0)

#endif /* USE_GUI */
#endif
