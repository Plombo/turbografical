#ifndef RETROCORE_H
#define RETROCORE_H

#include <stdbool.h>
#include <glib.h>
#include "libretro.h"

struct video_frame {
    unsigned width;
    unsigned height;
    unsigned pitch;
    float bottom;
    float right;
    void *data;
    unsigned frame_count;
};

extern struct video_frame g_current_frame;
extern struct retro_game_geometry g_geometry;

void handle_key_event(unsigned keyval, bool pressed);

void retrocore_init(const char *core_path, const char *game_path);
gpointer retrocore_run_game(gpointer data);

#endif

