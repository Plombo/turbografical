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

// Path to the current ROM or disc image, or NULL if nothing is loaded.
extern char *g_current_game_path;

extern struct video_frame g_frames[];
extern int g_next_frame;
extern double target_frame_time;
extern struct retro_game_geometry g_geometry;

// returns time from core startup in seconds
double retrocore_time(void);

void retrocore_pause(void);
void retrocore_unpause(void);
void retrocore_toggle_pause(void);

bool retrocore_load_state(const char *path);
bool retrocore_save_state(const char *path);

void handle_key_event(unsigned keyval, bool pressed);

void retrocore_init(const char *core_path);
void retrocore_load_game(const char *game_path);
gpointer retrocore_run_game(gpointer data);
void retrocore_close_game();

#endif

