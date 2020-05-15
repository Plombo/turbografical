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
    int64_t frame_count;
    double presentation_time;
};

// Used to signal when the frontend has displayed a frame and is ready for a new one.
extern GMutex g_frame_lock;
extern GCond g_ready_cond;

extern struct video_frame g_frames[];
extern int g_next_frame;
extern double target_frame_time;
extern struct retro_game_geometry g_geometry;

// returns time from core startup in seconds
double retrocore_time(void);

void handle_key_event(unsigned keyval, bool pressed);

void retrocore_init(const char *core_path, const char *game_path);
gpointer retrocore_run_game(gpointer data);

#endif

