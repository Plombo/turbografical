#include <string.h>
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
static double target_frame_time = 0.165;
static unsigned frame_count = 0;

struct retro_game_geometry g_geometry = {0};
struct video_frame g_current_frame = {0};

static float g_scale = 3;
bool running = true;

static struct {
	GLuint tex_id;
    GLuint fbo_id;
    GLuint rbo_id;

    int glmajor;
    int glminor;


	GLuint pitch;
	GLint tex_w, tex_h;
	GLuint clip_w, clip_h;

	GLuint pixfmt;
	GLuint pixtype;
	GLuint bpp;

    struct retro_hw_render_callback hw;
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
//	size_t retro_serialize_size(void);
//	bool retro_serialize(void *data, size_t size);
//	bool retro_unserialize(const void *data, size_t size);
//	void retro_cheat_reset(void);
//	void retro_cheat_set(unsigned index, bool enabled, const char *code);
	bool (*retro_load_game)(const struct retro_game_info *game);
//	bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info);
	void (*retro_unload_game)(void);
//	unsigned retro_get_region(void);
//	void *retro_get_memory_data(unsigned id);
//	size_t retro_get_memory_size(unsigned id);
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

void handle_key_event(unsigned keyval, bool pressed)
{
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

    g_video.hw.context_reset();
}


static bool video_set_pixel_format(unsigned format) {
	if (g_video.tex_id)
		die("Tried to change pixel format after initialization.");

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
    if (g_video.clip_w != width || g_video.clip_h != height || g_current_frame.data == NULL)
    {
		g_video.clip_h = height;
		g_video.clip_w = width;

		g_current_frame.data = realloc(g_current_frame.data, pitch * height * g_video.bpp);
	}

    g_current_frame.frame_count = frame_count;
    g_current_frame.width = width;
    g_current_frame.height = height;
    g_current_frame.pitch = pitch;
    g_current_frame.bottom = (float)g_video.clip_h / g_video.tex_h;
    g_current_frame.right  = (float)g_video.clip_w / g_video.tex_w;
    if (data && data != RETRO_HW_FRAME_BUFFER_VALID) {
        memcpy(g_current_frame.data, data, pitch * height * g_video.bpp);
    }
}

static void video_deinit() {
	if (g_video.tex_id)
		glDeleteTextures(1, &g_video.tex_id);

	g_video.tex_id = 0;
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
    SDL_QueueAudio(g_pcm, buf, sizeof(*buf) * frames * 2);
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

static uintptr_t core_get_current_framebuffer() {
    return g_video.fbo_id;
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
    case RETRO_ENVIRONMENT_SET_HW_RENDER: {
        struct retro_hw_render_callback *hw = (struct retro_hw_render_callback*)data;
        hw->get_current_framebuffer = core_get_current_framebuffer;
        hw->get_proc_address = (retro_hw_get_proc_address_t)SDL_GL_GetProcAddress;
        g_video.hw = *hw;
        return true;
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

	g_retro.retro_init();
	g_retro.initialized = true;

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

static void core_unload() {
	if (g_retro.initialized)
		g_retro.retro_deinit();

	if (g_retro.handle)
        SDL_UnloadObject(g_retro.handle);
}

static void noop() {}

void retrocore_init(const char *core_path, const char *game_path)
{
    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) < 0)
        die("Failed to initialize SDL");

    g_video.hw.version_major = 4;
    g_video.hw.version_minor = 5;
    g_video.hw.context_type  = RETRO_HW_CONTEXT_OPENGL_CORE;
    g_video.hw.context_reset   = noop;
    g_video.hw.context_destroy = noop;

    // Load the core.
    core_load(core_path);

    // Load the game.
    core_load_game(game_path);

    // Configure the player input devices.
    g_retro.retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
}

gpointer retrocore_run_game(gpointer data)
{
    Uint64 start_time = 0;
    Uint64 last_frame_time = 0;

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

        if (start_time == 0) start_time = SDL_GetPerformanceCounter();
        double time_since_start = (SDL_GetPerformanceCounter() - start_time) / (double)SDL_GetPerformanceFrequency();
        while (time_since_start < frame_count * target_frame_time)
        {
            g_usleep((frame_count * target_frame_time - time_since_start) * 1000000);
            time_since_start = (SDL_GetPerformanceCounter() - start_time) / (double)SDL_GetPerformanceFrequency();
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
		g_retro.retro_run();
		//printf("frame time: %.1f ms\n", (SDL_GetPerformanceCounter() - last_frame_time) / (double)SDL_GetPerformanceFrequency() * 1000);
		last_frame_time = SDL_GetPerformanceCounter();
		++frame_count;
	}
}

void retrocore_quit()
{
    core_unload();
	audio_deinit();
	video_deinit();

    SDL_Quit();
}
