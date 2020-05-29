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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gdk/gdk.h> // for keysyms
#include "config.h"
#include "libretro.h"

struct config g_config;

void set_default_config(void)
{
    g_config.video_scale = 2;
    memset(g_config.g_binds, 0, sizeof(g_config.g_binds));
    g_config.g_binds[RETRO_DEVICE_ID_JOYPAD_A] = GDK_KEY_x;
    g_config.g_binds[RETRO_DEVICE_ID_JOYPAD_B] = GDK_KEY_z;
    g_config.g_binds[RETRO_DEVICE_ID_JOYPAD_Y] = GDK_KEY_a;
    g_config.g_binds[RETRO_DEVICE_ID_JOYPAD_X] = GDK_KEY_s;
    g_config.g_binds[RETRO_DEVICE_ID_JOYPAD_UP] = GDK_KEY_Up;
    g_config.g_binds[RETRO_DEVICE_ID_JOYPAD_DOWN] = GDK_KEY_Down;
    g_config.g_binds[RETRO_DEVICE_ID_JOYPAD_LEFT] = GDK_KEY_Left;
    g_config.g_binds[RETRO_DEVICE_ID_JOYPAD_RIGHT] = GDK_KEY_Right;
    g_config.g_binds[RETRO_DEVICE_ID_JOYPAD_START] = GDK_KEY_Return;
    g_config.g_binds[RETRO_DEVICE_ID_JOYPAD_SELECT] = GDK_KEY_BackSpace;
}


