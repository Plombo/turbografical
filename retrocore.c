#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "SDL.h"
#include "libretro.h"
#include "retrocore.h"
#include "util.h"
#include "config.h"

#include <glib.h>
#include <gdk/gdk.h>

// exported globals
struct video_frame g_frames[2] = {{0}, {0}};
int g_next_frame = 0;
double target_frame_time = 0.165;
char *g_current_game_path = NULL;

// local globals
static bool running = false;
static int64_t frame_count = 0;
static uint8_t last_sram[2048] = {0};
static Uint64 start_time = 0;
static Uint64 pause_time;
static bool paused = false;
static char *g_save_state_path = NULL;
static char *g_load_state_path = NULL;

static struct {
    SDL_AudioDeviceID device;
    uint64_t samples_played;
    int sample_rate;
} g_audio = {0};


static struct {
	void *handle;
	bool initialized;

	void (*retro_init)(void);
	void (*retro_deinit)(void);
	unsigned (*retro_api_version)(void);
	void (*retro_get_system_info)(struct retro_system_info *info);
	void (*retro_get_system_av_info)(struct retro_system_av_info *info);
	void (*retro_set_controller_port_device)(unsigned port, unsigned device);
	void (*retro_reset)(void);
	void (*retro_run)(void);
	size_t (*retro_serialize_size)(void);
	bool (*retro_serialize)(void *data, size_t size);
	bool (*retro_unserialize)(const void *data, size_t size);
//	void retro_cheat_reset(void);
//	void retro_cheat_set(unsigned index, bool enabled, const char *code);
	bool (*retro_load_game)(const struct retro_game_info *game);
//	bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info);
	void (*retro_unload_game)(void);
//	unsigned retro_get_region(void);
	void *(*retro_get_memory_data)(unsigned id);
	size_t (*retro_get_memory_size)(unsigned id);
} g_retro;


struct keymap {
	unsigned k;
	unsigned rk;
};

static unsigned g_joy[RETRO_DEVICE_ID_JOYPAD_R3+1] = { 0 };

#define load_sym(V, S) do {\
    if (!((*(void**)&V) = SDL_LoadFunction(g_retro.handle, #S))) \
        die("Failed to load symbol '" #S "'': %s", SDL_GetError()); \
	} while (0)
#define load_retro_sym(S) load_sym(g_retro.S, S)


static void die(const char *fmt, ...)
{
	char buffer[4096];

	va_list va;
	va_start(va, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, va);
	va_end(va);

	fputs(buffer, stderr);
	fputc('\n', stderr);
	fflush(stderr);

	exit(EXIT_FAILURE);
}

// Return the master timer (time in seconds since system power on)
double retrocore_time(void)
{
    if (start_time == 0)
        start_time = SDL_GetPerformanceCounter();
    if (paused)
        return (pause_time - start_time) / (double)SDL_GetPerformanceFrequency();
    return (SDL_GetPerformanceCounter() - start_time) / (double)SDL_GetPerformanceFrequency();
}

// Pause the emulation.
void retrocore_pause(void)
{
    if (paused)
        return;

    paused = true;
    pause_time = SDL_GetPerformanceCounter();
    SDL_PauseAudioDevice(g_audio.device, 1);
}

// Unpauses the emulation.
void retrocore_unpause(void)
{
    if (!paused)
        return;

    paused = false;
    start_time += SDL_GetPerformanceCounter() - pause_time;
    SDL_PauseAudioDevice(g_audio.device, 0);
}

void retrocore_toggle_pause(void)
{
    if (paused)
        retrocore_unpause();
    else
        retrocore_pause();
}

void handle_key_event(unsigned keyval, bool pressed)
{
    // account for caps lock; 'A' and 'a' are the same thing for our purposes
    if (keyval >= 'A' && keyval <= 'Z')
    {
        keyval = tolower(keyval);
    }

    int i;
    for (i = 0; g_config.g_binds[i]; ++i)
    {
        if (g_config.g_binds[i] == keyval)
            g_joy[i] = pressed;
    }
}

static void video_refresh(const void *data, unsigned width, unsigned height, size_t pitch)
{
    g_mutex_lock(&g_frame_lock);
    // Check "running" here because if it's false, the GUI thread is waiting for this thread to
    // exit, so waiting on the condition would cause a deadlock.
    if (running && retrocore_time() < g_frames[g_next_frame].presentation_time)
    {
        g_cond_wait(&g_ready_cond, &g_frame_lock);
        //printf("Frame %li woke up %.1f ms early at %.3f s\n", frame_count, (frame_count * target_frame_time - retrocore_time()) * 1000, retrocore_time());
    }

    g_next_frame = !g_next_frame;
    struct video_frame *frame = &g_frames[g_next_frame];

    if (frame->width != width || frame->height != height || frame->data == NULL)
    {
		printf("resolution changed to %u*%u\n", width, height);
		frame->data = realloc(frame->data, width * height * 2);
	}

    frame->frame_count = frame_count;
    frame->presentation_time = frame_count * target_frame_time;
    frame->width = width;
    frame->height = height;
    if (data && data != RETRO_HW_FRAME_BUFFER_VALID)
    {
        const uint8_t *src = (const uint8_t*) data;
        uint8_t *dst = (uint8_t*) frame->data;
        for (int y = 0; y < height; ++y)
        {
            memcpy(dst, src, width * 2);
            src += pitch;
            dst += width * 2;
        }
    }
    //if (retrocore_time() >= frame->presentation_time)
    //    printf("Frame %li finished %.1f ms late at %.3f s\n", frame_count, (retrocore_time() - frame->presentation_time) * 1000, retrocore_time());
    g_mutex_unlock(&g_frame_lock);
}

static void video_deinit()
{
    free(g_frames[0].data);
    free(g_frames[1].data);
    memset(g_frames, 0, sizeof(g_frames));
    g_next_frame = 0;
}


static void audio_init(int frequency)
{
    SDL_AudioSpec desired;
    SDL_AudioSpec obtained;

    SDL_zero(desired);
    SDL_zero(obtained);

    desired.format = AUDIO_S16;
    desired.freq   = frequency;
    desired.channels = 2;
    desired.samples = 1596;

    g_audio.device = SDL_OpenAudioDevice(NULL, 0, &desired, &obtained, 0);
    if (!g_audio.device)
        die("Failed to open playback device: %s", SDL_GetError());

    SDL_PauseAudioDevice(g_audio.device, 0);
    g_audio.sample_rate = frequency;
}


static void audio_deinit()
{
    SDL_CloseAudioDevice(g_audio.device);
    g_audio.samples_played = 0;
}

static size_t audio_write(const int16_t *buf, unsigned frames)
{
    // If there's been a break in audio playback, and the audio is more than 100 ms
    // behind where it should be, change the clock to re-sync video to audio.
    if (SDL_GetQueuedAudioSize(g_audio.device) == 0)
    {
        double regular_time = retrocore_time();
        double audio_time = (double)g_audio.samples_played / g_audio.sample_rate;
        double difference = (regular_time - audio_time);
        if (difference >= .05) // 50 ms
        {
            start_time += (regular_time - audio_time) * SDL_GetPerformanceFrequency();
            printf("resync video; move %f ms\n", difference * 1000);
        }
        //else
        //    printf("queue empty but not resyncing video; difference is only %.1f ms\n", difference * 1000);
    }

    int ret = SDL_QueueAudio(g_audio.device, buf, sizeof(*buf) * frames * 2);
    g_audio.samples_played += frames;
    return frames;
}


static void core_log(enum retro_log_level level, const char *fmt, ...)
{
	char buffer[4096] = {0};
	static const char * levelstr[] = { "dbg", "inf", "wrn", "err" };
	va_list va;

	va_start(va, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, va);
	va_end(va);

	if (level == 0)
		return;

	fprintf(stderr, "[%s] %s", levelstr[level], buffer);
	fflush(stderr);

	if (level == RETRO_LOG_ERROR)
		exit(EXIT_FAILURE);
}

static bool core_environment(unsigned cmd, void *data)
{
	bool *bval;

	switch (cmd) {
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
		struct retro_log_callback *cb = (struct retro_log_callback *)data;
		cb->log = core_log;
        return true;
	}
	case RETRO_ENVIRONMENT_GET_CAN_DUPE:
		bval = (bool*)data;
		*bval = true;
        return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
		const enum retro_pixel_format *fmt = (enum retro_pixel_format *)data;

		if (*fmt != RETRO_PIXEL_FORMAT_RGB565)
			return false;

		return true;
	}
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = ".";
        return true;
	default:
		core_log(RETRO_LOG_DEBUG, "Unhandled env #%u", cmd);
		return false;
	}

    return false;
}


static void core_input_poll(void)
{
}


static int16_t core_input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
	if (port || index || device != RETRO_DEVICE_JOYPAD)
		return 0;

	return g_joy[id];
}


static void core_audio_sample(int16_t left, int16_t right)
{
	int16_t buf[2] = {left, right};
	audio_write(buf, 1);
}


static size_t core_audio_sample_batch(const int16_t *data, size_t frames)
{
	return audio_write(data, frames);
}


static void core_load(const char *sofile)
{
	void (*set_environment)(retro_environment_t) = NULL;
	void (*set_video_refresh)(retro_video_refresh_t) = NULL;
	void (*set_input_poll)(retro_input_poll_t) = NULL;
	void (*set_input_state)(retro_input_state_t) = NULL;
	void (*set_audio_sample)(retro_audio_sample_t) = NULL;
	void (*set_audio_sample_batch)(retro_audio_sample_batch_t) = NULL;
	memset(&g_retro, 0, sizeof(g_retro));
    g_retro.handle = SDL_LoadObject(sofile);

	if (!g_retro.handle)
        die("Failed to load core: %s", SDL_GetError());

	load_retro_sym(retro_init);
	load_retro_sym(retro_deinit);
	load_retro_sym(retro_api_version);
	load_retro_sym(retro_get_system_info);
	load_retro_sym(retro_get_system_av_info);
	load_retro_sym(retro_set_controller_port_device);
	load_retro_sym(retro_reset);
	load_retro_sym(retro_run);
	load_retro_sym(retro_load_game);
	load_retro_sym(retro_unload_game);
	load_retro_sym(retro_get_memory_size);
	load_retro_sym(retro_get_memory_data);
	load_retro_sym(retro_serialize_size);
	load_retro_sym(retro_serialize);
	load_retro_sym(retro_unserialize);

	load_sym(set_environment, retro_set_environment);
	load_sym(set_video_refresh, retro_set_video_refresh);
	load_sym(set_input_poll, retro_set_input_poll);
	load_sym(set_input_state, retro_set_input_state);
	load_sym(set_audio_sample, retro_set_audio_sample);
	load_sym(set_audio_sample_batch, retro_set_audio_sample_batch);

	set_environment(core_environment);
	set_video_refresh(video_refresh);
	set_input_poll(core_input_poll);
	set_input_state(core_input_state);
	set_audio_sample(core_audio_sample);
	set_audio_sample_batch(core_audio_sample_batch);

	puts("Core loaded");
}


static void core_load_game(const char *filename)
{
	struct retro_system_av_info av = {0};
	struct retro_system_info system = {0};
	struct retro_game_info info = { filename, 0 };

    SDL_RWops *file = SDL_RWFromFile(filename, "rb");

    if (!file)
        die("Failed to load %s: %s", filename, SDL_GetError());

    info.path = filename;
    info.meta = "";
    info.data = NULL;
    info.size = SDL_RWsize(file);

	g_retro.retro_get_system_info(&system);

	if (!g_retro.retro_load_game(&info))
		die("The core failed to load the content.");

	g_retro.retro_get_system_av_info(&av);
	audio_init(av.timing.sample_rate);
	target_frame_time = 1.0 / av.timing.fps;

    SDL_RWclose(file);
}

static void core_unload()
{
	if (g_retro.initialized)
	{
	    g_retro.retro_unload_game();
		g_retro.retro_deinit();
		g_retro.initialized = false;
    }

	if (g_retro.handle)
	{
        SDL_UnloadObject(g_retro.handle);
        g_retro.handle = NULL;
    }
}

static bool load_sram(const char *path)
{
    FILE *fp;
    void *sram = g_retro.retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    size_t size = g_retro.retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);

    if (!sram) return false;
    g_assert(size == sizeof(last_sram));
    fp = fopen(path, "rb");
    if (!fp) return false;
    if (fread(last_sram, 1, size, fp) != size)
    {
        fclose(fp);
        return false;
    }
    fclose(fp);
    memcpy(sram, last_sram, size);
    return true;
}

static bool save_sram(const char *path)
{
    FILE *fp;
    void *sram = g_retro.retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    size_t size = g_retro.retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);

    if (!sram) return false;
    fp = fopen(path, "wb");
    if (!fp) return false;
    if (fwrite(sram, 1, size, fp) != size)
    {
        fclose(fp);
        return false;
    }
    fclose(fp);
    return true;
}

bool load_state_actual(const char *save_path)
{
    size_t size = g_retro.retro_serialize_size();
    void *state = malloc(size);

    FILE *fp = fopen(save_path, "rb");
    if (!fp) goto fail;
    if (fread(state, 1, size, fp) != size) goto fail2;
    fclose(fp);

    bool result = g_retro.retro_unserialize(state, size);
    free(state);

    if (result)
    {
        // Set loaded SRAM as "current" so that the SRAM on disk won't be overwritten by
        // an accidental state load.
        memcpy(last_sram, g_retro.retro_get_memory_data(RETRO_MEMORY_SAVE_RAM), sizeof(last_sram));
    }

    return result;

fail2:
    fclose(fp);
fail:
    free(state);
    return false;
}

bool save_state_actual(const char *save_path)
{
    size_t size = g_retro.retro_serialize_size();
    void *state = malloc(size);
    if (!g_retro.retro_serialize(state, size)) goto fail;

    FILE *fp = fopen(save_path, "wb");
    if (!fp) goto fail;
    if (fwrite(state, 1, size, fp) != size) goto fail2;
    fclose(fp);

    printf("Saved state of size %zu to %s\n", size, save_path);
    free(state);
    return true;

fail2:
    fclose(fp);
fail:
    free(state);
    return false;
}

void retrocore_load_state(const char *path)
{
    g_load_state_path = strdup(path);
}

void retrocore_save_state(const char *path)
{
    g_save_state_path = strdup(path);
}

void retrocore_init(const char *core_path)
{
    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) < 0)
        die("Failed to initialize SDL");

    // Load the core.
    core_load(core_path);
}

void retrocore_load_game(const char *game_path)
{
    // Initializes the core.
    g_retro.retro_init();
	g_retro.initialized = true;

    // Load the game.
    core_load_game(game_path);
    g_current_game_path = strdup(game_path);

    // Load save data (SRAM) from disk.
    char *save_path = string_replace_extension(g_current_game_path, ".sav");
    printf("save path=%s\n", save_path);
    if (load_sram(save_path))
        printf("Loaded SRAM from %s\n", save_path);
    else
        printf("No saved data found\n");
    free(save_path);
    memcpy(last_sram, g_retro.retro_get_memory_data(RETRO_MEMORY_SAVE_RAM), sizeof(last_sram));

    // Configure the player input devices.
    g_retro.retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
}

gpointer retrocore_run_game(gpointer data)
{
    start_time = 0;
    running = true;
    paused = false;

    while (running)
    {
		g_retro.retro_run();

        // SRAM updated?
        void *sram = g_retro.retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
        if (memcmp(last_sram, sram, sizeof(last_sram)) != 0)
        {
            printf("SRAM updated!\n");
            char *save_path = string_replace_extension(g_current_game_path, ".sav");
            if (save_sram(save_path))
                printf("Saved SRAM to %s\n", save_path);
            else
                printf("Failed to save SRAM to %s\n", save_path);
            free(save_path);
            memcpy(last_sram, sram, sizeof(last_sram));
        }

        if (g_save_state_path)
        {
            save_state_actual(g_save_state_path);
            free(g_save_state_path);
            g_save_state_path = NULL;
        }

        if (g_load_state_path)
        {
            load_state_actual(g_load_state_path);
            free(g_load_state_path);
            g_load_state_path = NULL;
        }

		++frame_count;
	}

    // The game is being closed, so unload everything.
    core_unload();
	audio_deinit();
	video_deinit();

    free(g_current_game_path);
    g_current_game_path = NULL;
    frame_count = 0;
    memset(last_sram, 0, sizeof(last_sram));

    return NULL;
}

void retrocore_close_game()
{
    running = false;
}

