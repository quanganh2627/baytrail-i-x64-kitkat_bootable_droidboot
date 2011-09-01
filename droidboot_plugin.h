/*
 * Copyright 2011 Intel Corporation
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

#ifndef DROIDBOOT_PLUGIN_H
#define DROIDBOOT_PLUGIN_H

#include <droidboot_ui.h>

typedef int (*flash_func)(void *data, unsigned sz);

int aboot_register_flash_cmd(char *key, flash_func callback);

/* publish a variable readable by the built-in getvar command */
void fastboot_publish(const char *name, const char *value);

#endif

