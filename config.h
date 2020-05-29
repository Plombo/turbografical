/*
 * Copyright (c) 2020 Bryan Cain
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef CONFIG_H
#define CONFIG_H

#include "libretro.h"

struct config {
    unsigned int video_scale; // valid values: 1, 2, 3, or 4
    unsigned int g_binds[RETRO_DEVICE_ID_JOYPAD_R3 + 1];
};

extern struct config g_config;
void set_default_config(void);

#endif

