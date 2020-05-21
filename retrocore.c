#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "SDL.h"
#include "libretro.h"
#include "retrocore.h"

#include <glib.h>
#include <gdk/gdk.h>

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>

static SDL_AudioDeviceID g_pcm = 0;
static struct retro_frame_time_callback runloop_frame_time;
static retro_usec_t runloop_frame_time_last = 0;
static struct retro_audio_callback audio_callback;
static int64_t frame_count = 0;
double target_frame_time = 0.165;

static char *current_game_path = NULL;
static uint8_t last_sram[2048] = {0};

struct retro_game_geometry g_geometry = {0};
struct video_frame g_frames[2] = {{0}, {0}};
int g_next_frame = 0;

static Uint64 start_time = 0;

static float g_scale = 3;
bool running = false;

static struct {
	GLuint pitch;
	GLint tex_w, tex_h;
	GLuint clip_w, clip_h;

	GLuint pixfmt;
	GLuint pixtype;
	GLuint bpp;
} g_video  = {0};


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

static struct keymap g_binds[] = {
    { GDK_KEY_x, RETRO_DEVICE_ID_JOYPAD_A },
    { GDK_KEY_z, RETRO_DEVICE_ID_JOYPAD_B },
    { GDK_KEY_a, RETRO_DEVICE_ID_JOYPAD_Y },
    { GDK_KEY_s, RETRO_DEVICE_ID_JOYPAD_X },
    { GDK_KEY_Up, RETRO_DEVICE_ID_JOYPAD_UP },
    { GDK_KEY_Down, RETRO_DEVICE_ID_JOYPAD_DOWN },
    { GDK_KEY_Left, RETRO_DEVICE_ID_JOYPAD_LEFT },
    { GDK_KEY_Right, RETRO_DEVICE_ID_JOYPAD_RIGHT },
    { GDK_KEY_Return, RETRO_DEVICE_ID_JOYPAD_START },
    { GDK_KEY_BackSpace, RETRO_DEVICE_ID_JOYPAD_SELECT },
    { GDK_KEY_q, RETRO_DEVICE_ID_JOYPAD_L },
    { GDK_KEY_w, RETRO_DEVICE_ID_JOYPAD_R },
    { 0, 0 }
};

static unsigned g_joy[RETRO_DEVICE_ID_JOYPAD_R3+1] = { 0 };

#define load_sym(V, S) do {\
    if (!((*(void**)&V) = SDL_LoadFunction(g_retro.handle, #S))) \
        die("Failed to load symbol '" #S "'': %s", SDL_GetError()); \
	} while (0)
#define load_retro_sym(S) load_sym(g_retro.S, S)


static void die(const char *fmt, ...) {
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

double retrocore_time(void)
{
    if (start_time == 0)
        start_time = SDL_GetPerformanceCounter();
    return (SDL_GetPerformanceCounter() - start_time) / (double)SDL_GetPerformanceFrequency();
}

void handle_key_event(unsigned keyval, bool pressed)
{
    // account for caps lock; 'A' and 'a' are the same thing for our purposes
    if (keyval >= 'A' && keyval <= 'Z')
    {
        keyval = tolower(keyval);
    }

    int i;
    for (i = 0; g_binds[i].k || g_binds[i].rk; ++i)
    {
        if (g_binds[i].k == keyval)
            g_joy[g_binds[i].rk] = pressed;
    }
}


static void resize_to_aspect(double ratio, int sw, int sh, int *dw, int *dh) {
	*dw = sw;
	*dh = sh;

	if (ratio <= 0)
		ratio = (double)sw / sh;

	if ((float)sw / sh < 1)
		*dw = *dh * ratio;
	else
		*dh = *dw / ratio;
}


static void video_configure(const struct retro_game_geometry *geom) {
	int nwidth, nheight;

	resize_to_aspect(geom->aspect_ratio, geom->base_width * 1, geom->base_height * 1, &nwidth, &nheight);

	nwidth *= g_scale;
	nheight *= g_scale;

    g_video.pitch = geom->base_width * g_video.bpp;

	g_video.tex_w = geom->max_width;
	g_video.tex_h = geom->max_height;
	g_video.clip_w = geom->base_width;
	g_video.clip_h = geom->base_height;

    g_geometry = *geom;
}


static bool video_set_pixel_format(unsigned format) {
	switch (format) {
	case RETRO_PIXEL_FORMAT_0RGB1555:
		g_video.pixfmt = GL_UNSIGNED_SHORT_5_5_5_1;
		g_video.pixtype = GL_BGRA;
		g_video.bpp = sizeof(uint16_t);
		break;
	case RETRO_PIXEL_FORMAT_XRGB8888:
		g_video.pixfmt = GL_UNSIGNED_INT_8_8_8_8_REV;
		g_video.pixtype = GL_BGRA;
		g_video.bpp = sizeof(uint32_t);
		break;
	case RETRO_PIXEL_FORMAT_RGB565:
		g_video.pixfmt  = GL_UNSIGNED_SHORT_5_6_5;
		g_video.pixtype = GL_RGB;
		g_video.bpp = sizeof(uint16_t);
		break;
	default:
		die("Unknown pixel type %u", format);
	}

	return true;
}


static void video_refresh(const void *data, unsigned width, unsigned height, unsigned pitch) {
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

    if (g_video.clip_w != width || g_video.clip_h != height || frame->data == NULL)
    {
		g_video.clip_h = height;
		g_video.clip_w = width;
		printf("resolution changed to %u*%u\n", width, height);

		frame->data = realloc(frame->data, pitch * height);
	}

    frame->frame_count = frame_count;
    frame->presentation_time = frame_count * target_frame_time;
    frame->width = width;
    frame->height = height;
    frame->pitch = pitch;
    frame->bottom = (float)g_video.clip_h / g_video.tex_h;
    frame->right  = (float)g_video.clip_w / g_video.tex_w;
    if (data && data != RETRO_HW_FRAME_BUFFER_VALID) {
        memcpy(frame->data, data, pitch * height);
    }
    //if (retrocore_time() >= frame->presentation_time)
    //    printf("Frame %li finished %.1f ms late at %.3f s\n", frame_count, (retrocore_time() - frame->presentation_time) * 1000, retrocore_time());
    g_mutex_unlock(&g_frame_lock);
}

static void video_deinit() {
    if (g_frames[0].data)
        free(g_frames[0].data);
    if (g_frames[1].data)
        free(g_frames[1].data);
    memset(g_frames, 0, sizeof(g_frames));
    g_next_frame = 0;
}


static void audio_init(int frequency) {
    SDL_AudioSpec desired;
    SDL_AudioSpec obtained;

    SDL_zero(desired);
    SDL_zero(obtained);

    desired.format = AUDIO_S16;
    desired.freq   = frequency;
    desired.channels = 2;
    desired.samples = 4096;

    g_pcm = SDL_OpenAudioDevice(NULL, 0, &desired, &obtained, 0);
    if (!g_pcm)
        die("Failed to open playback device: %s", SDL_GetError());

    SDL_PauseAudioDevice(g_pcm, 0);

    // Let the core know that the audio device has been initialized.
    if (audio_callback.set_state) {
        audio_callback.set_state(true);
    }
}


static void audio_deinit() {
    SDL_CloseAudioDevice(g_pcm);
}

static size_t audio_write(const int16_t *buf, unsigned frames) {
    int ret = SDL_QueueAudio(g_pcm, buf, sizeof(*buf) * frames * 2);
    //printf("queued audio: %zu bytes now, %u bytes total, ret %i\n", sizeof(*buf) * frames * 2, SDL_GetQueuedAudioSize(g_pcm), ret);
    return frames;
}


static void core_log(enum retro_log_level level, const char *fmt, ...) {
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

static bool core_environment(unsigned cmd, void *data) {
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

		if (*fmt > RETRO_PIXEL_FORMAT_RGB565)
			return false;

		return video_set_pixel_format(*fmt);
	}
    case RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK: {
        const struct retro_frame_time_callback *frame_time =
            (const struct retro_frame_time_callback*)data;
        runloop_frame_time = *frame_time;
        break;
    }
    case RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK: {
        struct retro_audio_callback *audio_cb = (struct retro_audio_callback*)data;
        audio_callback = *audio_cb;
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


static void core_video_refresh(const void *data, unsigned width, unsigned height, size_t pitch) {
    video_refresh(data, width, height, pitch);
}


static void core_input_poll(void) {
}


static int16_t core_input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
	if (port || index || device != RETRO_DEVICE_JOYPAD)
		return 0;

	return g_joy[id];
}


static void core_audio_sample(int16_t left, int16_t right) {
	int16_t buf[2] = {left, right};
	audio_write(buf, 1);
}


static size_t core_audio_sample_batch(const int16_t *data, size_t frames) {
	return audio_write(data, frames);
}


static void core_load(const char *sofile) {
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
	set_video_refresh(core_video_refresh);
	set_input_poll(core_input_poll);
	set_input_state(core_input_state);
	set_audio_sample(core_audio_sample);
	set_audio_sample_batch(core_audio_sample_batch);

	puts("Core loaded");
}


static void core_load_game(const char *filename) {
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

	if (!system.need_fullpath) {
        info.data = SDL_malloc(info.size);

        if (!info.data)
            die("Failed to allocate memory for the content");

        if (!SDL_RWread(file, (void*)info.data, info.size, 1))
            die("Failed to read file data: %s", SDL_GetError());
	}

	if (!g_retro.retro_load_game(&info))
		die("The core failed to load the content.");

	g_retro.retro_get_system_av_info(&av);

	video_configure(&av.geometry);
	audio_init(av.timing.sample_rate);
	target_frame_time = 1.0 / av.timing.fps;

    if (info.data)
        SDL_free((void*)info.data);

    SDL_RWclose(file);

    // Now that we have the system info, set the window title.
    // TODO
    //char window_title[255];
    //snprintf(window_title, sizeof(window_title), "sdlarch %s %s", system.library_name, system.library_version);
    //SDL_SetWindowTitle(g_win, window_title);
}

/**
 * cpu_features_get_time_usec:
 *
 * Gets time in microseconds.
 *
 * Returns: time in microseconds.
 **/
retro_time_t cpu_features_get_time_usec(void) {
    return (retro_time_t)SDL_GetTicks();
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

// returns a newly allocated string; it's the caller's responsibility to free it
// example: string_replace_extension("/path/to/gamename.pce", ".sav") -> "/path/to/gamename.sav"
static char * string_replace_extension(const char *original, const char *extension)
{
    char *dot_pos = strrchr(original, '.'), *result;
    size_t orig_len, ext_len = strlen(extension);
    if (!dot_pos || strchr(dot_pos, '/') || strchr(dot_pos, '\\'))
    {
        // file has no extension
        orig_len = strlen(original);
    }
    else
    {
        // file has an extension that starts with dot_pos
        orig_len = dot_pos - original;
    }
    result = malloc(orig_len + ext_len + 1);
    memcpy(result, original, orig_len);
    memcpy(result + orig_len, extension, ext_len);
    result[orig_len + ext_len] = 0;
    return result;
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

bool retrocore_load_state(unsigned slot)
{
    size_t size = g_retro.retro_serialize_size();
    void *state = malloc(size);

    char extension[] = {".state.0"};
    extension[7] = '0' + slot;
    char *save_path = string_replace_extension(current_game_path, extension);
    FILE *fp = fopen(save_path, "rb");
    if (!fp) goto fail;
    if (fread(state, 1, size, fp) != size) goto fail2;
    fclose(fp);

    bool result = g_retro.retro_unserialize(state, size);
    free(state);
    free(save_path);

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
    free(save_path);
    free(state);
    return false;
}

bool retrocore_save_state(unsigned slot)
{
    size_t size = g_retro.retro_serialize_size();
    void *state = malloc(size);
    if (!g_retro.retro_serialize(state, size)) goto fail;

    char extension[] = {".state.0"};
    extension[7] = '0' + slot;
    char *save_path = string_replace_extension(current_game_path, extension);
    FILE *fp = fopen(save_path, "wb");
    if (!fp) goto fail2;
    if (fwrite(state, 1, size, fp) != size) goto fail3;
    fclose(fp);

    printf("Saved state of size %zu to %s\n", size, save_path);
    free(save_path);
    free(state);
    return true;

fail3:
    fclose(fp);
fail2:
    free(save_path);
fail:
    free(state);
    return false;
}

static void noop() {}

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
    current_game_path = strdup(game_path);

    // Load save data (SRAM) from disk.
    char *save_path = string_replace_extension(current_game_path, ".sav");
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

    while (running) {
        // Update the game loop timer.
        if (runloop_frame_time.callback) {
            retro_time_t current = cpu_features_get_time_usec();
            retro_time_t delta = current - runloop_frame_time_last;

            if (!runloop_frame_time_last)
                delta = runloop_frame_time.reference;
            runloop_frame_time_last = current;
            runloop_frame_time.callback(delta * 1000);
        }

        // Ask the core to emit the audio.
        if (audio_callback.callback) {
            audio_callback.callback();
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
		g_retro.retro_run();

        // SRAM updated?
        void *sram = g_retro.retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
        if (memcmp(last_sram, sram, sizeof(last_sram)) != 0)
        {
            printf("SRAM updated!\n");
            char *save_path = string_replace_extension(current_game_path, ".sav");
            if (save_sram(save_path))
                printf("Saved SRAM to %s\n", save_path);
            else
                printf("Failed to save SRAM to %s\n", save_path);
            free(save_path);
            memcpy(last_sram, sram, sizeof(last_sram));
        }

		++frame_count;
	}

    // The game is being closed, so unload everything.
    core_unload();
	audio_deinit();
	video_deinit();

    free(current_game_path);
    current_game_path = NULL;
    frame_count = 0;
    memset(last_sram, 0, sizeof(last_sram));

    return NULL;
}

void retrocore_close_game()
{
    running = false;
}

