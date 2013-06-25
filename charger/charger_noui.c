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

/**
 * File used in case of Droidboot is used in "no ui" mode (#undef USE_GUI)
 * for instance for engeneering builds for bring-up boards without ui.
 * In this mode "charger" which embeds the get_battery_level function, is not
 * used any more, so create a new one to let libintelprov still accessing it.
 *
 * */

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "droidboot_ui.h"

#define UTIL_MAX_PATH_LEN  256 /* Max number of characters for a full file path */
#define KERN_SYS_PS_PATH   "/sys/devices"
#define CAPACITY_FILE_NAME "capacity" /* kernel handles current battery capacity
                                         in that file */

/**
 * Looks for the file whose name is the string pointed to by path and opens it.
 * Usage: int fd = util_fnd_open(path, fname);
 * @param path: the directory path to look from
 * @param fname: pointer to the string of the filename
 * @return Upon successful completion returns the file descriptor integer value.
 * Otherwise, 0 is returned.
 */
int util_fnd_open(char *path, char *fname)
{
    struct dirent *dir_read;
    int path_len = 0;
    char new_path[UTIL_MAX_PATH_LEN] = {'\0'};
    int is_found = 0;
    int fd = 0;

    DIR *d = opendir(path);
    if (!d) {
        pr_error("Failed to open dir %s\n", path);
        return 0;
    }

    while (((dir_read = readdir(d)) != NULL) && !is_found) {
        /* prepare a buffer with full path to be used multiple times later on */
        path_len = strlen(path) + strlen(dir_read->d_name);
        sprintf(new_path, "%s/%s", path, dir_read->d_name);

        if ((dir_read->d_type == DT_DIR) &&
                (strcmp(dir_read->d_name, ".") != 0) &&
                (strcmp(dir_read->d_name, "..") != 0)) {
            if (path_len >= UTIL_MAX_PATH_LEN) {
                pr_error("Path length exceed %d characters: %s\n", path_len, path);
                return 0;
            }

            /* this is a dir look for recursively */
            fd = util_fnd_open(&new_path[0], fname);
            if (fd != 0) {
                is_found = 1;
            }
        }
        /* recursive end condition */
        if (dir_read->d_type == DT_REG) {
            if (strcmp(dir_read->d_name, fname) == 0) {
                is_found = 1;
               fd = open(&new_path[0], O_RDONLY);
                if (fd == -1) {
                    pr_error("Can't open file: %s\n", &new_path[0]);
                    fd = 0;
                }
            }
        }
    }
    return fd;
}


/**
 * This function allows to get the battery level from droidboot when graphics and
 * so charger not used.
 * @return Battery capacity value stored within /sys/class/power_supply/..._battery/capacity
 * */
int get_battery_level(void)
{
    char buf[32] = {'\0'};
    int cap_val = 0;
    int fd = 0;
    int cnt = 0;

    fd = util_fnd_open(KERN_SYS_PS_PATH, CAPACITY_FILE_NAME);
    if (fd != 0) {
        cnt = read(fd, buf, sizeof(buf)-1);
        if (cnt > 0) {
            buf[cnt] = '\0';
            if (buf[cnt - 1] == '\n') {
               cnt--;
               buf[cnt] = '\0';
            }
        }
        close(fd);
    }
    cap_val = atoi(buf);
    if (cap_val > 100)
        return -1;

    return cap_val;
}


