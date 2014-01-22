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

//#define DEBUG_UEVENTS

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtc.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#include <cutils/android_reboot.h>
#include <cutils/klog.h>
#include <cutils/list.h>
#include <cutils/misc.h>
#include <cutils/uevent.h>
#include <hardware_legacy/power.h>

#include "minui/minui.h"
#include "charger.h"

#ifndef max
#define max(a,b) ((a) > (b) ? (a) : (b))
#endif

#ifndef min
#define min(a,b) ((a) < (b) ? (a) : (b))
#endif

#define ARRAY_SIZE(x)           (sizeof(x)/sizeof(x[0]))

#define MSEC_PER_SEC            (1000LL)
#define NSEC_PER_MSEC           (1000000LL)

#define BATTERY_FULL_THRESH     95

#define LAST_KMSG_PATH          "/proc/last_kmsg"
#define LAST_KMSG_MAX_SZ        (32 * 1024)

#define LOGE(x...) do { KLOG_ERROR("charger", x); } while (0)
#define LOGI(x...) do { KLOG_INFO("charger", x); } while (0)
#define LOGV(x...) do { KLOG_DEBUG("charger", x); } while (0)

#define SPID_FMLY_FILE_NAME	"/sys/spid/platform_family_id"
#define MOOR_PF_ID		"0008"

struct key_state {
    bool pending;
    bool down;
    int64_t timestamp;
};

struct power_supply {
    struct listnode list;
    char name[256];
    char type[32];
    bool online;
    bool valid;
    char cap_path[PATH_MAX];
};

struct frame {
    const char *name;
    int disp_time;
    int min_capacity;
    bool level_only;

    gr_surface surface;
};

struct animation {
    bool run;

    struct frame *frames;
    int cur_frame;
    int num_frames;
    int allocated_frames;

    int cur_cycle;
    int num_cycles;

    int anim_thresh;

    /* current capacity being animated */
    int capacity;
};

struct charger {
    int64_t next_screen_transition;
    int64_t next_key_check;
    int64_t next_pwr_check;
    int64_t next_cap_check;

    struct key_state keys[KEY_MAX + 1];
    int uevent_fd;

    struct listnode supplies;
    int num_supplies;
    int num_supplies_online;

    struct animation *batt_anim;
    gr_surface surf_unknown;

    struct power_supply *battery;

    int min_charge;
    int mode;
    enum charger_exit_state state;

    int max_temp;

    int64_t power_key_ms;
    int64_t batt_unknown_ms;
    int64_t unplug_shutdown_ms;
    int64_t cap_poll_ms;
};

struct uevent {
    const char *action;
    const char *path;
    const char *subsystem;
    const char *ps_name;
    const char *ps_type;
    const char *ps_online;
};

static struct frame batt_anim_frames[] = {
    {
        .name = "charger/battery_0",
        .disp_time = 750,
        .min_capacity = 0,
    },
    {
        .name = "charger/battery_0a",
        .disp_time = 750,
        .min_capacity = 20,
    },
    {
        .name = "charger/battery_1",
        .disp_time = 750,
        .min_capacity = 20,
    },
    {
        .name = "charger/battery_1a",
        .disp_time = 750,
        .min_capacity = 40,
    },
    {
        .name = "charger/battery_2",
        .disp_time = 750,
        .min_capacity = 40,
    },
    {
        .name = "charger/battery_3",
        .disp_time = 750,
        .min_capacity = 60,
    },
    {
        .name = "charger/battery_4",
        .disp_time = 750,
        .min_capacity = 80,
    },
    {
        .name = "charger/battery_5",
        .disp_time = 750,
        .min_capacity = BATTERY_FULL_THRESH,
    },
};

static struct animation battery_animation = {
    .frames = batt_anim_frames,
    .num_frames = ARRAY_SIZE(batt_anim_frames),
    .num_cycles = 3,
};

static struct charger charger_state = {
    .batt_anim = &battery_animation,
};

static int char_width;
static int char_height;
static pthread_t charger_thread;

extern int set_screen_state(int);

/* current time in milliseconds */
static int64_t curr_time_ms(void)
{
    struct timespec tm;
    clock_gettime(CLOCK_MONOTONIC, &tm);
    return tm.tv_sec * MSEC_PER_SEC + (tm.tv_nsec / NSEC_PER_MSEC);
}

static void clear_screen(void)
{
    gr_color(0, 0, 0, 255);
    gr_fill(0, 0, gr_fb_width(), gr_fb_height());
};

#define MAX_KLOG_WRITE_BUF_SZ 256

static void dump_last_kmsg(void)
{
    char *buf;
    char *ptr;
    unsigned sz = 0;
    int len;

    LOGI("\n");
    LOGI("*************** LAST KMSG ***************\n");
    LOGI("\n");
    buf = load_file(LAST_KMSG_PATH, &sz);
    if (!buf || !sz) {
        LOGI("last_kmsg not found. Cold reset?\n");
        goto out;
    }

    len = min(sz, LAST_KMSG_MAX_SZ);
    ptr = buf + (sz - len);

    while (len > 0) {
        int cnt = min(len, MAX_KLOG_WRITE_BUF_SZ);
        char yoink;
        char *nl;

        nl = memrchr(ptr, '\n', cnt - 1);
        if (nl)
            cnt = nl - ptr + 1;

        yoink = ptr[cnt];
        ptr[cnt] = '\0';
        klog_write(6, "<6>%s", ptr);
        ptr[cnt] = yoink;

        len -= cnt;
        ptr += cnt;
    }

    free(buf);

out:
    LOGI("\n");
    LOGI("************* END LAST KMSG *************\n");
    LOGI("\n");
}

static int read_file(const char *path, char *buf, size_t sz)
{
    int fd;
    size_t cnt;

    fd = open(path, O_RDONLY, 0);
    if (fd < 0)
        goto err;

    cnt = read(fd, buf, sz - 1);
    if (cnt <= 0)
        goto err;
    buf[cnt] = '\0';
    if (buf[cnt - 1] == '\n') {
        cnt--;
        buf[cnt] = '\0';
    }

    close(fd);
    return cnt;

err:
    if (fd >= 0)
        close(fd);
    return -1;
}

static void kick_animation(struct animation *anim)
{
    anim->run = true;
}

static void reset_animation(struct animation *anim)
{
    anim->cur_cycle = 0;
    anim->cur_frame = 0;
    anim->run = false;
}

static int read_file_int(const char *path, int *val)
{
    char buf[32];
    int ret;
    int tmp;
    char *end;

    ret = read_file(path, buf, sizeof(buf));
    if (ret < 0)
        return -1;

    tmp = strtol(buf, &end, 0);
    if (end == buf ||
        ((end < buf+sizeof(buf)) && (*end != '\n' && *end != '\0')))
        goto err;

    *val = tmp;
    return 0;

err:
    return -1;
}

static int get_battery_capacity(struct charger *charger)
{
    int ret;
    int batt_cap = -1;

    if (!charger->battery)
        return -1;

    ret = read_file_int(charger->battery->cap_path, &batt_cap);
    if (ret < 0 || batt_cap > 100)
        return -1;

    return batt_cap;
}

/* This function allows to get the battery level from droidboot */
int get_battery_level(void)
{
    struct charger *charger = &charger_state;

    // returns -1 in case of error
    return get_battery_capacity(charger);
}

static struct power_supply *find_supply(struct charger *charger,
                                        const char *name)
{
    struct listnode *node;
    struct power_supply *supply;

    list_for_each(node, &charger->supplies) {
        supply = node_to_item(node, struct power_supply, list);
        if (!strncmp(name, supply->name, sizeof(supply->name)))
            return supply;
    }
    return NULL;
}

static struct power_supply *add_supply(struct charger *charger,
                                       const char *name, const char *type,
                                       const char *path, bool online)
{
    struct power_supply *supply;

    supply = calloc(1, sizeof(struct power_supply));
    if (!supply)
        return NULL;

    strlcpy(supply->name, name, sizeof(supply->name));
    strlcpy(supply->type, type, sizeof(supply->type));
    snprintf(supply->cap_path, sizeof(supply->cap_path),
             "/sys/%s/capacity", path);
    supply->online = online;
    list_add_tail(&charger->supplies, &supply->list);
    charger->num_supplies++;
    LOGV("... added %s %s %d\n", supply->name, supply->type, online);
    return supply;
}

static void remove_supply(struct charger *charger, struct power_supply *supply)
{
    if (!supply)
        return;
    list_remove(&supply->list);
    charger->num_supplies--;
    free(supply);
}

static void parse_uevent(const char *msg, struct uevent *uevent)
{
    uevent->action = "";
    uevent->path = "";
    uevent->subsystem = "";
    uevent->ps_name = "";
    uevent->ps_online = "";
    uevent->ps_type = "";

    /* currently ignoring SEQNUM */
    while (*msg) {
#ifdef DEBUG_UEVENTS
        LOGV("uevent str: %s\n", msg);
#endif
        if (!strncmp(msg, "ACTION=", 7)) {
            msg += 7;
            uevent->action = msg;
        } else if (!strncmp(msg, "DEVPATH=", 8)) {
            msg += 8;
            uevent->path = msg;
        } else if (!strncmp(msg, "SUBSYSTEM=", 10)) {
            msg += 10;
            uevent->subsystem = msg;
        } else if (!strncmp(msg, "POWER_SUPPLY_NAME=", 18)) {
            msg += 18;
            uevent->ps_name = msg;
        } else if (!strncmp(msg, "POWER_SUPPLY_ONLINE=", 20)) {
            msg += 20;
            uevent->ps_online = msg;
        } else if (!strncmp(msg, "POWER_SUPPLY_TYPE=", 18)) {
            msg += 18;
            uevent->ps_type = msg;
        }

        /* advance to after the next \0 */
        while (*msg++)
            ;
    }

    LOGV("event { '%s', '%s', '%s', '%s', '%s', '%s' }\n",
         uevent->action, uevent->path, uevent->subsystem,
         uevent->ps_name, uevent->ps_type, uevent->ps_online);
}

static int process_ps_uevent(struct charger *charger, struct uevent *uevent)
{
    int online;
    char ps_type[32];
    struct power_supply *supply = NULL;
    bool was_online = false;
    bool battery = false;
    int old_supplies_online = charger->num_supplies_online;
    int ret = -1;

    if (uevent->ps_type[0] == '\0') {
        char *path;

        if (uevent->path[0] == '\0')
            goto error;
        ret = asprintf(&path, "/sys/%s/type", uevent->path);
        if (ret <= 0) {
            ret = -1;
            goto error;
        }
        ret = read_file(path, ps_type, sizeof(ps_type));
        free(path);
        if (ret < 0) {
            LOGE("Failed to read /sys/%s/type\n", uevent->path);
            goto error;
        }
    } else {
        strlcpy(ps_type, uevent->ps_type, sizeof(ps_type));
    }

    if (!strncmp(ps_type, "Battery", 7))
        battery = true;

    online = atoi(uevent->ps_online);
    supply = find_supply(charger, uevent->ps_name);
    if (supply) {
        was_online = supply->online;
        supply->online = online;
    }

    if (!strcmp(uevent->action, "add")) {
        if (!supply) {
            supply = add_supply(charger, uevent->ps_name, ps_type, uevent->path,
                                online);
            if (!supply) {
                LOGE("cannot add supply '%s' (%s %d)\n", uevent->ps_name,
                     uevent->ps_type, online);
                ret = -1;
                goto error;
            }
            /* only pick up the first battery for now */
            if (battery && !charger->battery)
                charger->battery = supply;
        } else {
            LOGE("supply '%s' already exists..\n", uevent->ps_name);
        }
    } else if (!strcmp(uevent->action, "remove")) {
        if (supply) {
            if (charger->battery == supply)
                charger->battery = NULL;
            remove_supply(charger, supply);
            supply = NULL;
        }
    } else if (!strcmp(uevent->action, "change")) {
        if (!supply) {
            LOGE("power supply '%s' not found ('%s' %d)\n",
                 uevent->ps_name, ps_type, online);
            ret = -1;
            goto error;
        }
    } else {
        LOGE("Unknown uevent action \"%s\"\n", uevent->action);
        ret = -1;
        goto error;
    }

    /* allow battery to be managed in the supply list but make it not
     * contribute to online power supplies. */
    if (!battery) {
        if (was_online && !online)
            charger->num_supplies_online--;
        else if (supply && !was_online && online)
            charger->num_supplies_online++;
    }

    /* If supply list changes, start animating again */
    if (charger->num_supplies_online != old_supplies_online)
        kick_animation(charger->batt_anim);

    LOGI("power supply %s (%s) %s (action=%s num_online=%d num_supplies=%d)\n",
         uevent->ps_name, ps_type, battery ? "" : online ? "online" : "offline",
         uevent->action, charger->num_supplies_online, charger->num_supplies);

    return 0;

error:
    LOGE("Failed to process event { '%s', '%s', '%s', '%s', '%s', '%s' }\n",
         uevent->action, uevent->path, uevent->subsystem,
         uevent->ps_name, uevent->ps_type, uevent->ps_online);

    return ret;

}

static int process_uevent(struct charger *charger, struct uevent *uevent)
{
    if (!strcmp(uevent->subsystem, "power_supply"))
        return process_ps_uevent(charger, uevent);
    return 0;
}

#define UEVENT_MSG_LEN  1024
static int handle_uevent_fd(struct charger *charger, int fd)
{
    char msg[UEVENT_MSG_LEN+2];
    int n;
    int ret = 0;

    if (fd < 0)
        return -1;

    while (true) {
        struct uevent uevent;

        n = uevent_kernel_multicast_recv(fd, msg, UEVENT_MSG_LEN);
        if (n <= 0)
            break;
        if (n >= UEVENT_MSG_LEN)   /* overflow -- discard */
            continue;

        msg[n] = '\0';
        msg[n+1] = '\0';

        parse_uevent(msg, &uevent);
        ret = process_uevent(charger, &uevent);
        if (ret < 0 ) {
            LOGE("Did not process event %s %s\n", uevent.subsystem, uevent.action);
            break;
        }
    }

    return ret;
}

static int uevent_callback(int fd, short revents, void *data)
{
    struct charger *charger = data;

    if (!(revents & POLLIN))
        return -1;
    return handle_uevent_fd(charger, fd);
}

/* force the kernel to regenerate the change events for the existing
 * devices, if valid */
static void do_coldboot(struct charger *charger, DIR *d, const char *event,
                        bool follow_links, int max_depth)
{
    struct dirent *de;
    int dfd, fd, nbwr;

    dfd = dirfd(d);
    if (dfd >= 0) {
        fd = openat(dfd, "uevent", O_WRONLY);
        if (fd >= 0) {
            nbwr = write(fd, event, strlen(event));
            close(fd);
            if (nbwr <= 0) {
                LOGE("Failed to write event %s\n", event);
                goto close_dfd;
            }
            handle_uevent_fd(charger, charger->uevent_fd);
        }
    }
    else {
        LOGE("Failed to get dir file descriptor %d\n", dfd);
        goto out;
    }

    while ((de = readdir(d)) && max_depth > 0) {
        DIR *d2;

        LOGV("looking at '%s'\n", de->d_name);

        if ((de->d_type != DT_DIR && !(de->d_type == DT_LNK && follow_links)) ||
           de->d_name[0] == '.') {
            LOGV("skipping '%s' type %d (depth=%d follow=%d)\n",
                 de->d_name, de->d_type, max_depth, follow_links);
            continue;
        }
        LOGV("can descend into '%s'\n", de->d_name);

        fd = openat(dfd, de->d_name, O_RDONLY | O_DIRECTORY);
        if (fd < 0) {
            LOGE("cannot openat %d '%s' (%d: %s)\n", dfd, de->d_name,
                errno, strerror(errno));
            continue;
        }

        d2 = fdopendir(fd);
        if (d2 != 0) {
            LOGV("opened '%s'\n", de->d_name);
            do_coldboot(charger, d2, event, follow_links, max_depth - 1);
            closedir(d2);
        }
        close(fd);
    }
close_dfd:
    close(dfd);
out:
    return;
}

static void coldboot(struct charger *charger, const char *path,
                     const char *event)
{
    char str[256];

    LOGV("doing coldboot '%s' in '%s'\n", event, path);
    DIR *d = opendir(path);
    if (d) {
        str[255] = 0;
        snprintf(str, sizeof(str) - 1, "%s\n", event);
        do_coldboot(charger, d, str, true, 1);
        closedir(d);
    }
}

static int draw_text(const char *str, int x, int y)
{
    int str_len_px = gr_measure(str);

    if (x < 0)
        x = (gr_fb_width() - str_len_px) / 2;
    if (y < 0)
        y = (gr_fb_height() - char_height) / 2;
    gr_text(x, y, str);

    return y + char_height;
}

static void android_green(void)
{
    gr_color(0xa4, 0xc6, 0x39, 255);
}

/* returns the last y-offset of where the surface ends */
static int draw_surface_centered(gr_surface surface)
{
    int w;
    int h;
    int x;
    int y;

    w = gr_get_width(surface);
    h = gr_get_height(surface);
    x = (gr_fb_width() - w) / 2 ;
    y = (gr_fb_height() - h) / 2 ;

    LOGV("drawing surface %dx%d+%d+%d\n", w, h, x, y);
    gr_blit(surface, 0, 0, w, h, x, y);
    return y + h;
}

static void draw_unknown(struct charger *charger)
{
    int y;
    if (charger->surf_unknown) {
        draw_surface_centered(charger->surf_unknown);
    } else {
        android_green();
        y = draw_text("Charging!", -1, -1);
        draw_text("?\?/100", -1, y + 25);
    }
}

static void draw_battery(struct charger *charger)
{
    struct animation *batt_anim = charger->batt_anim;
    struct frame *frame = &batt_anim->frames[batt_anim->cur_frame];

    if (batt_anim->num_frames != 0) {
        draw_surface_centered(frame->surface);
        LOGV("drawing frame #%d name=%s min_cap=%d time=%d\n",
             batt_anim->cur_frame, frame->name, frame->min_capacity,
             frame->disp_time);
    }
}

static void redraw_screen(struct charger *charger)
{
    struct animation *batt_anim = charger->batt_anim;

    clear_screen();

    /* try to display *something* */
    if (batt_anim->capacity < 0 || batt_anim->num_frames == 0)
        draw_unknown(charger);
    else
        draw_battery(charger);
    gr_flip();
}

static void update_screen_state(struct charger *charger, int64_t now)
{
    struct animation *batt_anim = charger->batt_anim;
    int disp_time;
    int batt_cap;

    if (!batt_anim->run || now < charger->next_screen_transition)
        return;

    /* animation is over, blank screen and leave */
    if (batt_anim->cur_cycle == batt_anim->num_cycles) {
        reset_animation(batt_anim);
        charger->next_screen_transition = -1;
        gr_fb_blank(true);
        LOGV("[%lld] animation done\n", now);
        if (charger->num_supplies_online != 0)
            set_screen_state(0);
        else {
            /* Stop at the correct-level, as animation could have
               ended at the next level */
            batt_cap = get_battery_capacity(charger);
            if (batt_cap < batt_anim->frames[batt_anim->anim_thresh].min_capacity)
                batt_anim->cur_frame = batt_anim->anim_thresh - 1;
            else
                batt_anim->cur_frame = batt_anim->anim_thresh;

            redraw_screen(charger);
            reset_animation(batt_anim);
        }
        return;
    }

    disp_time = batt_anim->frames[batt_anim->cur_frame].disp_time;

    /* animation starting, set up the animation */
    if (batt_anim->cur_frame == 0) {
        LOGV("[%lld] animation starting\n", now);
        batt_cap = get_battery_capacity(charger);
        if (batt_cap >= 0 && batt_anim->num_frames != 0) {
            int i;

            /* find first frame given current capacity */
            for (i = 1; i < batt_anim->num_frames; i++) {
                if (batt_cap < batt_anim->frames[i].min_capacity)
                    break;
            }
            batt_anim->cur_frame = i - 1;
            /* Run animation only till the next segment */
            if (i == batt_anim->num_frames)
                batt_anim->anim_thresh = batt_anim->cur_frame;
            else
                batt_anim->anim_thresh = batt_anim->cur_frame + 1;

            /* show the first frame for twice as long */
            disp_time = batt_anim->frames[batt_anim->cur_frame].disp_time * 2;
        }

        batt_anim->capacity = batt_cap;
    }

    /* unblank the screen  on first cycle */
    if (batt_anim->cur_cycle == 0)
        gr_fb_blank(false);

    /* draw the new frame (@ cur_frame) */
    redraw_screen(charger);

    /* if we don't have anim frames, we only have one image, so just bump
     * the cycle counter and exit
     */
    if (batt_anim->num_frames == 0 || batt_anim->capacity < 0) {
        LOGV("[%lld] animation missing or unknown battery status\n", now);
        charger->next_screen_transition = now + charger->batt_unknown_ms;
        batt_anim->cur_cycle++;
        return;
    }

    /* schedule next screen transition */
    charger->next_screen_transition = now + disp_time;


    /* advance frame cntr to the next valid frame if we are actually charging
     */
    if (charger->num_supplies_online != 0) {
        batt_anim->cur_frame++;

        /* if the frame is used for level-only, that is only show it when it's
         * the current level, skip it during the animation.
         */
        while (batt_anim->cur_frame < batt_anim->num_frames &&
               batt_anim->frames[batt_anim->cur_frame].level_only)
            batt_anim->cur_frame++;
        if (batt_anim->cur_frame > batt_anim->anim_thresh) {
            batt_anim->cur_cycle++;
            batt_anim->cur_frame = 0;

            /* don't reset the cycle counter, since we use that as a signal
             * in a test above to check if animation is over
             */
        }
    } else {
        /* Stop animating if we're not charging, but keep visible until
         * cycle count expires */
        batt_anim->cur_frame = 0;
        batt_anim->cur_cycle++;
    }
}

static int set_key_callback(int code, int value, void *data)
{
    struct charger *charger = data;
    int64_t now = curr_time_ms();
    int down = !!value;

    if (code > KEY_MAX)
        return -1;

    /* ignore events that don't modify our state */
    if (charger->keys[code].down == down)
        return 0;

    /* only record the down even timestamp, as the amount
     * of time the key spent not being pressed is not useful */
    if (down)
        charger->keys[code].timestamp = now;
    charger->keys[code].down = down;
    charger->keys[code].pending = true;
    if (down) {
        LOGV("[%lld] key[%d] down\n", now, code);
    } else {
        int64_t duration = now - charger->keys[code].timestamp;
        int64_t secs = duration / 1000;
        int64_t msecs = duration - secs * 1000;
        LOGV("[%lld] key[%d] up (was down for %lld.%lldsec)\n", now,
            code, secs, msecs);
    }

    return 0;
}

static void update_input_state(struct charger *charger,
                               struct input_event *ev)
{
    if (ev->type != EV_KEY)
        return;
    set_key_callback(ev->code, ev->value, charger);
}

static void set_next_key_check(struct charger *charger,
                               struct key_state *key,
                               int64_t timeout)
{
    int64_t then = key->timestamp + timeout;

    if (charger->next_key_check == -1 || then < charger->next_key_check)
        charger->next_key_check = then;
}

static void process_key(struct charger *charger, int code, int64_t now)
{
    struct key_state *key = &charger->keys[code];

    if (code == KEY_POWER) {
        int64_t proceed_timeout = key->timestamp + charger->power_key_ms;
        if (key->down && charger->power_key_ms >= 0) {
            if (now < proceed_timeout) {
                /* if the key is pressed but timeout hasn't expired,
                 * make sure we wake up at the right-ish time to check
                 */
                set_next_key_check(charger, key, charger->power_key_ms);
                set_screen_state(1);
                kick_animation(charger->batt_anim);
            }
        } else {
            /* if the power key got released, force screen state cycle.
             * check if key was hold down longer than power on time
             * to decide to turn on the device. We do the check when key
             * is relaesed to avoid race condition between cold reset
             * and forced shutdown when key is hold down for 7 seconds
             */
            if (!key->down && key->pending) {
                if (now >= proceed_timeout && charger->power_key_ms >= 0) {
                    if (get_battery_capacity(charger) >= charger->min_charge) {
                        LOGI("[%lld] power button press+hold, exiting\n", now);
                        charger->state = CHARGER_PROCEED;
                    } else {
                        LOGI("[%lld] ignore press+hold power, battery level "
                                "less than minimum\n", now);
                    }
                }
                set_screen_state(1);
                kick_animation(charger->batt_anim);
            }
        }
    }

    key->pending = false;
}

static void handle_input_state(struct charger *charger, int64_t now)
{
    process_key(charger, KEY_POWER, now);

    if (charger->next_key_check != -1 && now > charger->next_key_check)
        charger->next_key_check = -1;
}

static void handle_power_supply_state(struct charger *charger, int64_t now)
{
    if (charger->unplug_shutdown_ms < 0)
        return;

    if (charger->num_supplies_online == 0) {
        if (charger->next_pwr_check == -1) {
            set_screen_state(1);
            charger->next_pwr_check = now + charger->unplug_shutdown_ms;
            LOGI("[%lld] device unplugged: shutting down in %lld (@ %lld)\n",
                 now, charger->unplug_shutdown_ms, charger->next_pwr_check);
        } else if (now >= charger->next_pwr_check) {
            LOGI("[%lld] shutting down (no online supplies)\n", now);
            /* Subsequent battery level check may change this to CHARGER_PROCEED */
            charger->state = CHARGER_SHUTDOWN;
        } else {
            /* otherwise we already have a shutdown timer scheduled */
        }
    } else {
        /* online supply present, reset shutdown timer if set */
        if (charger->next_pwr_check != -1) {
            LOGI("[%lld] device plugged in: shutdown cancelled\n", now);
            kick_animation(charger->batt_anim);
        }
        charger->next_pwr_check = -1;
    }
}

/* Periodic check of current battery level to see if it exceeds the
 * minimum threshold */
static void handle_capacity_state(struct charger *charger, int64_t now)
{
    int charge_pct;

    if (!charger->min_charge || charger->mode == MODE_CHARGER)
        return; /* We don't care */

    if (!charger->battery) {
        LOGE("Told to wait until battery is at %d%%, but no "
                "battery detected at all. Exiting.\n", charger->min_charge);
        charger->state = CHARGER_PROCEED;
        return;
    }

    charge_pct = get_battery_capacity(charger);
    if (charge_pct >= charger->min_charge) {
        LOGI("[%lld] battery capacity %d%% >= %d%%, exiting\n", now,
                charge_pct, charger->min_charge);
        charger->state = CHARGER_PROCEED;
    } else
        LOGV("[%lld] battery capacity %d%% < %d%%\n", now,
                charge_pct, charger->min_charge);

    if (charger->num_supplies_online == 0)
        charger->next_cap_check = -1;
    else
        charger->next_cap_check = now + charger->cap_poll_ms;
}

static int get_max_temp(void)
{
    int max_temp = -1;
    char buf[256];
    char *p;
    FILE *fd;

    fd = fopen(TEMP_CONF_FILE, "r");
    if (fd == NULL) {
        LOGE("Unable to open thermal config file, "
             "setting default threshold(%d)\n", DEFAULT_TEMP_THRESH);
        return DEFAULT_TEMP_THRESH;
    }

    while (fgets(buf, 256, fd) && strncmp(buf, TEMP_ZONE, strlen(TEMP_ZONE)));
    while (fgets(buf, 256, fd)) {
        p = strstr(buf, TEMP_NAME);
        if (p != NULL) {
            p += strlen(TEMP_NAME)+1;
            max_temp = atoi(p);
            LOGV("max-temp from config-file: %d\n", max_temp);
            break;
        }
    }

    fclose(fd);
    return max_temp;
}

static bool is_platform(const char *data)
{
    int ret;
    char spid_val[4];
    FILE *spid_fd;

    spid_fd = fopen(SPID_FMLY_FILE_NAME, "r");
    if (spid_fd == NULL) {
        LOGE("Unable to open file %s\n", SPID_FMLY_FILE_NAME);
        goto out;
    }

    ret = fseek(spid_fd, 0, SEEK_SET);
    if (ret != 0) {
        LOGE("Unable to set file position %d\n", ret);
        goto close_spid_fd;
    }

    ret = fscanf(spid_fd, "%s\n", spid_val);
    if (ret <= 0) {
        LOGE("Unable to read file %d\n", ret);
        goto close_spid_fd;
    }
    /* check the platform family id */
    if (!strcmp(spid_val, data))
	return true;
    else
	return false;

close_spid_fd:
    fclose(spid_fd);
out:
    return false;
}

static void handle_temperature_state(struct charger *charger)
{
    int temp, ret;
    FILE *temp_fd;

    if (charger->max_temp == -1)
        goto out;

    temp_fd = fopen(SYS_TEMP_INT, "r");
    if (temp_fd == NULL) {
        LOGE("Unable to open file %s\n", SYS_TEMP_INT);
        goto out;
    }

    ret = fseek(temp_fd, 0, SEEK_SET);
    if (ret != 0) {
        LOGE("Unable to set file position %d\n", ret);
        goto close_temp_fd;
    }

    ret  = fscanf(temp_fd, "%d\n", &temp);
    if (ret <= 0) {
        LOGE("Unable to read file %d\n", ret);
        goto close_temp_fd;
    }

    /* TODO: This is workaround for BZ: 165821. This will be removed later. */
    /* For Moorefield-Check platform family id */
    if (!is_platform(MOOR_PF_ID)) {

	if (temp >= charger->max_temp) {
            set_screen_state(1);
            LOGI("Temperature(%d) is higher than threshold(%d), "
                 "shutting down system.\n", temp, charger->max_temp);
            charger->state = CHARGER_SHUTDOWN;
            system("echo 1 > /sys/module/intel_mid_osip/parameters/force_shutdown_occured");
        }
    }

close_temp_fd:
    fclose(temp_fd);
out:
    return;
}

int write_alarm_to_osnib(int mode)
{
    int devfd, errNo, ret;

    devfd = open(IPC_DEVICE_NAME, O_RDWR);
    if (devfd < 0) {
        LOGE("unable to open the DEVICE %s\n", IPC_DEVICE_NAME);
        ret = -1;
        goto err1;
    }

    errNo = ioctl(devfd, IPC_WRITE_ALARM_TO_OSNIB, &mode);
    if (errNo < 0) {
        LOGE("ioctl for DEVICE %s, returns error-%d\n",
                        IPC_DEVICE_NAME, errNo);
        ret = -1;
        goto err2;
    }
    ret = 0;

err2:
    close(devfd);
err1:
    return ret;
}

void *handle_rtc_alarm_event(void *arg)
{
    struct charger *charger = (struct charger *) arg;
    unsigned long data;
    int rtc_fd, ret;
    int batt_cap;
    struct rtc_wkalrm alarm;

    write_alarm_to_osnib(ALARM_CLEAR);

    rtc_fd = open(RTC_FILE, O_RDONLY, 0);
    if (rtc_fd < 0) {
        LOGE("Unable to open the DEVICE %s\n", RTC_FILE);
        goto err1;
    }

    /* RTC alarm set ? */
    ret = ioctl(rtc_fd, RTC_WKALM_RD, &alarm);
    if (ret == -1) {
        LOGE("ioctl(RTC_WKALM_RD) failed\n");
        goto err2;
    }

    if (!alarm.enabled)
        LOGI("no RTC wake-alarm set\n");
    else {
        LOGI("RTC wake-alarm set: %04d-%02d-%02d %02d:%02d:%02d\n",
                alarm.time.tm_year+1900,
                alarm.time.tm_mon+1,
                alarm.time.tm_mday,
                alarm.time.tm_hour,
                alarm.time.tm_min,
                alarm.time.tm_sec);

        /* Enable alarm interrupts */
        ret = ioctl(rtc_fd, RTC_AIE_ON, 0);
        if (ret == -1) {
             LOGE("rtc ioctl RTC_AIE_ON error\n");
             goto err2;
        }
    }

    if (!alarm.pending)
        LOGI("no RTC wake-alarm pending\n");
    else
        LOGI("RTC wake-alarm pending\n");

    /* This blocks until the alarm ring causes an interrupt */
    ret = read(rtc_fd, &data, sizeof(unsigned long));
    if (ret < 0) {
        LOGE("rtc read error\n");
        goto err2;
    }

    batt_cap = get_battery_capacity(charger);
    if (batt_cap >= charger->min_charge) {
        LOGI("RTC alarm rang, Rebooting to MOS");

        if (write_alarm_to_osnib(ALARM_SET))
            LOGE("Error in setting alarm-flag to OSNIB");

        android_reboot(ANDROID_RB_RESTART2, 0, "android");
    } else {
        LOGI("RTC alarm rang, capacity:%d less than minimum threshold:%d, "
             "cannot boot to MOS", batt_cap, charger->min_charge);
    }

err2:
    close(rtc_fd);
err1:
    return NULL;
}

static void wait_next_event(struct charger *charger, int64_t now)
{
    int64_t next_event = INT64_MAX;
    int64_t timeout;
    int ret;

    LOGV("[%lld] next screen: %lld next key: %lld next pwr: %lld "
            "next cap: %lld\n", now, charger->next_screen_transition,
            charger->next_key_check, charger->next_pwr_check,
            charger->next_cap_check);

    if (charger->next_screen_transition != -1)
        next_event = charger->next_screen_transition;
    if (charger->next_key_check != -1 && charger->next_key_check < next_event)
        next_event = charger->next_key_check;
    if (charger->next_pwr_check != -1 && charger->next_pwr_check < next_event)
        next_event = charger->next_pwr_check;
    if (charger->next_cap_check != -1 && charger->next_cap_check < next_event)
        next_event = charger->next_cap_check;

    if (next_event != -1 && next_event != INT64_MAX)
        timeout = max(0, next_event - now);
    else
        timeout = -1;
    LOGV("[%lld] blocking (%lld)\n", now, timeout);
    ret = ev_wait((int)timeout);
    if (!ret)
        ev_dispatch();
}

static int input_callback(int fd, short revents, void *data)
{
    struct charger *charger = data;
    struct input_event ev;
    int ret;

    ret = ev_get_input(fd, revents, &ev);
    if (ret)
        return -1;
    update_input_state(charger, &ev);
    return 0;
}

static enum charger_exit_state charger_event_loop()
{
    struct charger *charger = &charger_state;

    while (true) {
        int64_t now = curr_time_ms();

        LOGV("[%lld] event_loop()\n", now);
        handle_input_state(charger, now);
        handle_power_supply_state(charger, now);
        handle_capacity_state(charger, now);
        handle_temperature_state(charger);

        if (charger->state != CHARGER_CHARGE)
            return charger->state;

        /* do screen update last in case any of the above want to start
         * screen transitions (animations, etc)
         */
        update_screen_state(charger, now);
        wait_next_event(charger, now);
    }
}

static void free_surfaces(struct animation *anim)
{
    for ( ; anim->allocated_frames > 0; anim->allocated_frames--)
        res_free_surface(anim->frames[anim->allocated_frames - 1].surface);
}

enum charger_exit_state charger_run(int min_charge, int mode, int power_key_ms,
        int batt_unknown_ms, int unplug_shutdown_ms, int cap_poll_ms)
{
    int fd, i, ret;
    int64_t now = curr_time_ms() - 1;
    struct charger *charger = &charger_state;
    enum charger_exit_state out_state;

    dump_last_kmsg();

    if (mode == MODE_CHARGER)
        LOGI("--------------- STARTING CHARGER MODE FOR COS ---------------\n");
    else
        LOGI("--------------- STARTING CHARGER MODE TEMPORARILY ---------------\n");

    gr_font_size(&char_width, &char_height);

    list_init(&charger->supplies);
    charger->min_charge = min_charge;
    charger->mode = mode;
    charger->state = CHARGER_CHARGE;
    charger->power_key_ms = power_key_ms;
    charger->batt_unknown_ms = batt_unknown_ms;
    charger->unplug_shutdown_ms = unplug_shutdown_ms;
    charger->cap_poll_ms = cap_poll_ms;

    charger->max_temp = get_max_temp();
    if (charger->max_temp == -1)
        LOGE("Error in getting maximum temperature threshold");

    if (pthread_create(&charger_thread, NULL, handle_rtc_alarm_event, charger) != 0)
        LOGE("Error in creating rtc-alarm thread\n");

    ev_exit();
    ev_init(input_callback, charger);

    fd = uevent_open_socket(64*1024, true);
    if (fd >= 0) {
        fcntl(fd, F_SETFL, O_NONBLOCK);
        ev_add_fd(fd, uevent_callback, charger);
    }
    else
        LOGE("Failed to create uevent socket\n");

    charger->uevent_fd = fd;
    coldboot(charger, "/sys/class/power_supply", "add");

    ret = res_create_surface("charger/battery_fail", &charger->surf_unknown);
    if (ret < 0) {
        LOGE("Cannot load image\n");
        charger->surf_unknown = NULL;
    }

    for (i = 0; i < charger->batt_anim->num_frames; i++) {
        struct frame *frame = &charger->batt_anim->frames[i];

        ret = res_create_surface(frame->name, &frame->surface);
        if (ret < 0) {
            LOGE("Cannot load image %s\n", frame->name);
            free_surfaces(charger->batt_anim);
            charger->batt_anim->num_frames = 0;
            charger->batt_anim->num_cycles = 1;
            break;
        }
        charger->batt_anim->allocated_frames++;
    }

    ev_sync_key_state(set_key_callback, charger);

    charger->next_screen_transition = now - 1;
    charger->next_key_check = -1;
    charger->next_pwr_check = -1;
    charger->next_cap_check = -1;

    reset_animation(charger->batt_anim);
    kick_animation(charger->batt_anim);

    out_state = charger_event_loop();
    if (out_state == CHARGER_PROCEED && min_charge)
        gr_fb_blank(false);
    free_surfaces(charger->batt_anim);
    ev_exit();
    set_screen_state(1);

    return out_state;
}

