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
#include <stdint.h>
#include <stdbool.h>
#include <gtk/gtk.h>
#include "util.h"
#include "retrocore.h"

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>

static GtkBuilder *builder = NULL;

static GLuint shader_program = 0;
static bool texture_inited = 0;

static GThread *emu_thread = NULL;
static char *rom_path = NULL;

GMutex g_frame_lock = {0};
GCond g_ready_cond = {0};

int64_t last_frame_count = -1;
static unsigned int state_slot = 0;

static bool g_fullscreen = false;
static gint64 g_last_mouse_movement = 0;
static GdkCursor *g_blank_cursor = NULL;


static gboolean render(GtkGLArea *area, GdkGLContext *context)
{
    int allocatedWidth = gtk_widget_get_allocated_width(GTK_WIDGET(area));
    int allocatedHeight = gtk_widget_get_allocated_height(GTK_WIDGET(area));

    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport(0, 0, allocatedWidth, allocatedHeight);

    // if the emulator isn't running, we're done after clearing to black
    if (!emu_thread)
        return true;

    double rx, ry;
    if ((double)allocatedWidth / allocatedHeight > (6.0/5.0))
    {
        rx = (allocatedHeight / 5.0) * 6.0 / allocatedWidth;
        ry = 1.0;
    }
    else
    {
        rx = 1.0;
        ry = (allocatedWidth / 6.0) * 5.0 / allocatedHeight;
    }

    GLint aspect_scale_loc = glGetUniformLocation(shader_program, "aspect_scale");
    glUniform2f(aspect_scale_loc, rx, ry);

    if (!texture_inited && g_geometry.max_width)
    {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g_geometry.max_width, g_geometry.max_height, 0,
                     GL_RGB, GL_UNSIGNED_SHORT_5_6_5, NULL);
        glUniform2f(glGetUniformLocation(shader_program, "tex_dims"), g_geometry.max_width, g_geometry.max_height);
        texture_inited = true;
    }

    g_mutex_lock(&g_frame_lock);
    struct video_frame *frame;
    double now = retrocore_time();
    if (now >= g_frames[g_next_frame].presentation_time &&
        (g_frames[g_next_frame].frame_count == last_frame_count + 1 ||
         now - g_frames[g_next_frame].presentation_time >= target_frame_time))
    {
        frame = &g_frames[g_next_frame];
        //printf("advancing to frame %li (%i)\n", frame->frame_count, g_next_frame);
        g_cond_signal(&g_ready_cond);
        //printf("signalled at %.3f s (%.3f s after frame %li pres time of %.3f s)\n", retrocore_time(), retrocore_time() - frame->presentation_time, frame->frame_count, frame->presentation_time);
    }
    else
    {
        frame = &g_frames[!g_next_frame];
        //printf("staying on frame %li (%i)\n", frame->frame_count, !g_next_frame);
        g_assert(retrocore_time() >= frame->presentation_time);
    }

    if (frame->data != NULL)
    {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 2); // it would be better to get the "2" from the provided pixel format
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame->width, frame->height,
            GL_RGB, GL_UNSIGNED_SHORT_5_6_5, frame->data);
        GLint coord_scale_loc = glGetUniformLocation(shader_program, "coord_scale");
        glUniform2f(coord_scale_loc, frame->right, frame->bottom);
        float scaleFactor = MAX((float)allocatedWidth * rx / frame->width,
                                (float)allocatedHeight * ry / frame->height);
        glUniform1f(glGetUniformLocation(shader_program, "scale_factor"), scaleFactor);
    }

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

#if 0 // code to test the frame pacing
    //static unsigned last_frame_count = (unsigned)-1;
    static unsigned frame_duration = 0;
    if (frame->frame_count == last_frame_count)
        ++frame_duration;
    else
    {
        if (frame_duration != 1)
            printf("Frame %li duration: %u\n", last_frame_count, frame_duration);
        while (++last_frame_count < frame->frame_count)
            printf("Frame %li duration: 0\n", last_frame_count);
        last_frame_count = frame->frame_count;
        frame_duration = 1;
    }
#endif

    g_mutex_unlock(&g_frame_lock);

    return TRUE;
}

static void on_realize_gl_area(GtkGLArea *area)
{
    gtk_gl_area_make_current(area);

    // Catch any errors from context creation
    if (gtk_gl_area_get_error(area) != NULL)
    {
        return;
    }

    float points[] = {
        -1.0f, -1.0f, 0.0f, 1.0f,
        -1.0f, 1.0f,  0.0f, 0.0f,
        1.0f, -1.0f,  1.0f, 1.0f,
        1.0f, 1.0f,   1.0f, 0.0f,
    };

    GLuint vbo = 0;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(points), points, GL_STATIC_DRAW);

    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glEnableVertexAttribArray(0); // TODO: get attrib location for these instead of guessing
    glEnableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    //glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float)*4, 0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(float)*4, (void*)(2 * sizeof(float)));

    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    const char* vertex_shader =
    "#version 150\n"
    "in vec2 position;\n"
    "in vec2 in_coord;\n"
    "out vec2 tex_coord;\n"
    "uniform vec2 aspect_scale;\n"
    "uniform vec2 coord_scale;\n"
    "void main() {\n"
    "    tex_coord = in_coord * coord_scale;\n"
    "    gl_Position = vec4(position * aspect_scale, 1.0, 1.0);\n"
    "}";

    const char* fragment_shader =
    "#version 150\n"
    "in vec2 tex_coord;\n"
    "out vec4 frag_color;\n"
    "uniform sampler2D texture;\n"
    "uniform vec2 tex_dims;\n"
	"uniform float scale_factor;\n"
    "void main() {\n"
    "   vec2 texel = tex_coord * tex_dims;\n"
    "   float region_range = 0.5 - 0.5 / scale_factor;\n"
    "   vec2 distFromCenter = fract(texel) - 0.5;\n"
    "   vec2 f = (distFromCenter - clamp(distFromCenter, -region_range, region_range)) * scale_factor + 0.5;\n"
    "   frag_color = texture2D(texture, (floor(texel) + f) / tex_dims);\n"
    "}\n";

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vertex_shader, NULL);
    glCompileShader(vs);
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fragment_shader, NULL);
    glCompileShader(fs);

    shader_program = glCreateProgram();
    glAttachShader(shader_program, fs);
    glAttachShader(shader_program, vs);
    glLinkProgram(shader_program);

    glUseProgram(shader_program);
    glUniform1i(glGetUniformLocationARB(shader_program, "texture"), 0);
    glBindVertexArray(vao);
}

// callback that makes the GL area redraw on every frame
static gboolean tick_cb(GtkWidget *gl_area_w, GdkFrameClock *frame_clock, gpointer user_data)
{
    if (emu_thread)
    {
        gtk_widget_queue_draw(gl_area_w);
    }

    if (g_fullscreen)
    {
        // Hide cursor in fullscreen if no mouse movement in the last 2.5 seconds
        GdkWindow *window = gtk_widget_get_window(gl_area_w);
        if (g_last_mouse_movement + 2500000 < g_get_monotonic_time())
        {
            if (!g_blank_cursor)
            {
                g_blank_cursor = gdk_cursor_new_for_display(
                        gdk_window_get_display(window),
                        GDK_BLANK_CURSOR);
            }
            gdk_window_set_cursor(window, g_blank_cursor);
        }
        else
        {
            gdk_window_set_cursor(window, NULL);
        }
    }

    return G_SOURCE_CONTINUE;
}

static gboolean handle_key_press(GtkWidget *widget, GdkEventKey *event)
{
    gtk_window_activate_key(GTK_WINDOW(widget), event);
    //gtk_window_propagate_key_event(GTK_WINDOW(widget), event);

    handle_key_event(event->keyval, true);
    return TRUE;
}

static gboolean handle_key_release(GtkWidget *widget, GdkEventKey *event)
{
    handle_key_event(event->keyval, false);
    return TRUE;
}

static void on_window_state_change(GtkWidget *widget, GdkEvent *ev, gpointer user_data)
{
    if (ev->type != GDK_WINDOW_STATE)
        return;

    GdkEventWindowState *event = (GdkEventWindowState *) ev;
    GtkWidget *menu_bar = GTK_WIDGET(gtk_builder_get_object(builder, "menuBar"));
    if (event->changed_mask & GDK_WINDOW_STATE_FULLSCREEN)
    {
        // Hide the menu bar in fullscreen until the user moves the mouse
        // pointer to the top of the screen.
        if (event->new_window_state & GDK_WINDOW_STATE_FULLSCREEN)
        {
            g_fullscreen = true;
            gtk_widget_set_visible(menu_bar, FALSE);
        }
        else
        {
            g_fullscreen = false;
            gtk_widget_set_visible(menu_bar, TRUE);
            gdk_window_set_cursor(event->window, NULL);
        }
    }
}

// Responsible for showing/hiding the menu bar in fullscreen depending on cursor position.
static gboolean on_mouse_pointer_move(GtkWidget *widget, GdkEvent *ev, gpointer unused)
{
    GdkEventMotion *event = (GdkEventMotion *) ev;
    g_last_mouse_movement = g_get_monotonic_time();
    if (g_fullscreen)
    {
        // Show the menu bar in fullscreen only if the mouse pointer is in the top 10% of the screen.
        GtkWidget *menu_bar = GTK_WIDGET(gtk_builder_get_object(builder, "menuBar"));
        int height = gtk_widget_get_allocated_height(widget);
        gtk_widget_set_visible(menu_bar, event->y < height / 10.0);
    }
    return FALSE;
}

static void close_game()
{
    if (!emu_thread)
        return;

    g_mutex_lock(&g_frame_lock);
    retrocore_close_game();
    g_cond_signal(&g_ready_cond);
    g_mutex_unlock(&g_frame_lock);
    g_thread_join(emu_thread);
    emu_thread = NULL;
    free(rom_path);
    rom_path = NULL;

    // the GL area needs to redraw itself one last time, to clear everything
    GtkWidget *gl_area = GTK_WIDGET(gtk_builder_get_object(builder, "glArea"));
    gtk_widget_queue_draw(gl_area);
}

static void load_game(const char *path)
{
    if (emu_thread)
        close_game();

    retrocore_init("./mednafen_pce_libretro.so");
    retrocore_load_game(path);
    emu_thread = g_thread_new("emulator", retrocore_run_game, NULL);
    rom_path = strdup(path);
}

static void on_open_button_activate(GtkMenuItem *open_button, gpointer data)
{
    GtkBuilder *builder = (GtkBuilder*) data;
    GtkWindow *parent_window = GTK_WINDOW(gtk_builder_get_object(builder, "mainWindow"));
    gint res;

    GtkWidget *dialog = gtk_file_chooser_dialog_new("Select game",
                                         parent_window,
                                         GTK_FILE_CHOOSER_ACTION_OPEN,
                                         "_Cancel",
                                         GTK_RESPONSE_CANCEL,
                                         "_Open",
                                         GTK_RESPONSE_ACCEPT,
                                         NULL);

    // All supported games filter
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "All PC Engine/TurboGrafx-16 Games (*.pce, *.sgx, *.cue, *.chd)");
    gtk_file_filter_add_pattern(filter, "*.pce");
    gtk_file_filter_add_pattern(filter, "*.sgx");
    gtk_file_filter_add_pattern(filter, "*.cue");
    gtk_file_filter_add_pattern(filter, "*.chd");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

    // PC Engine/TurboGrafx-16 ROM filter
    filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "PC Engine/TurboGrafx-16/SuperGrafx ROMs (*.pce, *.sgx)");
    gtk_file_filter_add_pattern(filter, "*.pce");
    gtk_file_filter_add_pattern(filter, "*.sgx");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

    // CD image filter
    filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "PC Engine CD/TurboGrafx-CD Games (*.cue, *.chd)");
    gtk_file_filter_add_pattern(filter, "*.cue");
    gtk_file_filter_add_pattern(filter, "*.chd");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

    // All files filter
    filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "All Files");
    gtk_file_filter_add_pattern(filter, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

    res = gtk_dialog_run(GTK_DIALOG(dialog));
    if (res == GTK_RESPONSE_ACCEPT)
    {
        char *filename;
        GtkFileChooser *chooser = GTK_FILE_CHOOSER(dialog);
        filename = gtk_file_chooser_get_filename(chooser);

        printf("Open ROM: %s\n", filename);
        load_game(filename);

        g_free(filename);
    }

    gtk_widget_destroy(dialog);
}

static void on_close_button_activate(GtkMenuItem *button, gpointer data)
{
    close_game();
}

static void on_load_state_activate(GtkMenuItem *item, gpointer unused)
{
    if (!emu_thread)
        return;

    GtkWindow *parent_window = GTK_WINDOW(gtk_builder_get_object(builder, "mainWindow"));
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Select state",
                                         parent_window,
                                         GTK_FILE_CHOOSER_ACTION_OPEN,
                                         "_Cancel",
                                         GTK_RESPONSE_CANCEL,
                                         "_Open",
                                         GTK_RESPONSE_ACCEPT,
                                         NULL);

    gint res = gtk_dialog_run(GTK_DIALOG(dialog));
    if (res == GTK_RESPONSE_ACCEPT)
    {
        GtkFileChooser *chooser = GTK_FILE_CHOOSER(dialog);
        char *filename = gtk_file_chooser_get_filename(chooser);
        retrocore_load_state(filename);
        g_free(filename);
    }

    gtk_widget_destroy(dialog);
}

static void on_save_state_as_activate(GtkMenuItem *item, gpointer unused)
{
    if (!emu_thread)
        return;

    GtkWindow *parent_window = GTK_WINDOW(gtk_builder_get_object(builder, "mainWindow"));
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Select save location",
                                         parent_window,
                                         GTK_FILE_CHOOSER_ACTION_SAVE,
                                         "_Cancel",
                                         GTK_RESPONSE_CANCEL,
                                         "_Open",
                                         GTK_RESPONSE_ACCEPT,
                                         NULL);

    gint res = gtk_dialog_run(GTK_DIALOG(dialog));
    if (res == GTK_RESPONSE_ACCEPT)
    {
        GtkFileChooser *chooser = GTK_FILE_CHOOSER(dialog);
        char *filename = gtk_file_chooser_get_filename(chooser);
        retrocore_save_state(filename);
        g_free(filename);
    }

    gtk_widget_destroy(dialog);
}

static void on_state_slot_selected(GtkCheckMenuItem *selection, gpointer unused)
{
    if (gtk_check_menu_item_get_active(selection))
    {
        // Figure out which state slot was selected by looking at the label
        state_slot = gtk_menu_item_get_label(GTK_MENU_ITEM(selection))[0] - '0';
        //printf("State %u selected\n", state_slot);
    }
}

static void on_quick_load_activate(GtkMenuItem *item, gpointer unused)
{
    if (!emu_thread)
        return;

    char extension[] = {".state.0"};
    extension[7] = '0' + state_slot;
    char *save_path = string_replace_extension(g_current_game_path, extension);
    retrocore_load_state(save_path);
    free(save_path);
}

static void on_quick_save_activate(GtkMenuItem *item, gpointer unused)
{
    if (!emu_thread)
        return;

    char extension[] = {".state.0"};
    extension[7] = '0' + state_slot;
    char *save_path = string_replace_extension(g_current_game_path, extension);
    retrocore_save_state(save_path);
    free(save_path);
}

static void on_pause_button_activate(GtkMenuItem *button, gpointer data)
{
    if (!emu_thread)
        return;

    retrocore_toggle_pause();
}

static void on_reset_button_activate(GtkMenuItem *button, gpointer data)
{
    if (!emu_thread)
        return;

    // There is a retro_reset() function in libretro, but using it with the
    // beetle-pce core will trigger various assertion failures. There was no
    // "reset" button on any PC Engine or TurboGrafx-16, so resetting is the
    // same as just power cycling anyway.
    char *rom_path2 = strdup(rom_path);
    close_game();
    load_game(rom_path2);
    free(rom_path2);
}

static void on_video_size_selected(GtkCheckMenuItem *selection, gpointer data)
{
    if (gtk_check_menu_item_get_active(selection))
    {
        // Figure out which scale factor was selected by looking at the label.
        int video_scale = gtk_menu_item_get_label(GTK_MENU_ITEM(selection))[0] - '0';

        // Resize the rendering area to the selected size.
        GtkWidget *gl_area = (GtkWidget*) data;
        gtk_widget_set_size_request(gl_area, (int) 243 * video_scale * 1.2, 243 * video_scale);

        // Now resize the window to match.
        GtkWindow *window = GTK_WINDOW(gtk_builder_get_object(builder, "mainWindow"));
        GtkRequisition min, natural;
        gtk_widget_get_preferred_size(GTK_WIDGET(window), &min, &natural);
        gtk_window_resize(window, natural.width, natural.height);
    }
}

static void on_fullscreen_button_activate(GtkMenuItem *button, gpointer data)
{
    GtkWindow *window = GTK_WINDOW(gtk_builder_get_object(builder, "mainWindow"));
    if (g_fullscreen)
    {
        gtk_window_unfullscreen(window);
    }
    else
    {
        gtk_window_fullscreen(window);
    }
}

// By default, accelerators for menu items won't fire if the menu bar is
// hidden. Returning  TRUE from this function, connected to the
// "can-activate-accel" signal, overrides this behavior so that keyboard
// shortcuts work even when the menu bar is hidden in fullscreen.
gboolean can_activate_accel(GtkWidget *widget, guint signal_id, gpointer user_data)
{
    return TRUE;
}

void setup_menu_item(const char *name, GCallback callback, gpointer cb_user_data)
{
    GObject *obj = gtk_builder_get_object(builder, name);
    g_signal_connect(obj, "activate", callback, cb_user_data);
    g_signal_connect(obj, "can-activate-accel", G_CALLBACK(can_activate_accel), NULL);
}

int main(int argc, char **argv)
{
    GObject *window;
    GObject *box;
    GtkWidget *glArea;
    GError *error = NULL;

    gtk_init(&argc, &argv);

    // Construct a GtkBuilder instance and load our UI description
    builder = gtk_builder_new();
    if (gtk_builder_add_from_file(builder, "interface.glade", &error) == 0)
    {
        g_printerr("Error loading file: %s\n", error->message);
        g_clear_error(&error);
        return 1;
    }

    // Connect signal handlers to the constructed widgets
    window = gtk_builder_get_object(builder, "mainWindow");
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(window, "window-state-event", G_CALLBACK(on_window_state_change), NULL);
    g_signal_connect(window, "motion-notify-event", G_CALLBACK(on_mouse_pointer_move), NULL);

    setup_menu_item("openButton", G_CALLBACK(on_open_button_activate), builder);
    setup_menu_item("closeButton", G_CALLBACK(on_close_button_activate), builder);
    setup_menu_item("loadState", G_CALLBACK(on_load_state_activate), builder);
    setup_menu_item("saveStateAs", G_CALLBACK(on_save_state_as_activate), builder);
    setup_menu_item("exitButton", G_CALLBACK(gtk_main_quit), builder);
    setup_menu_item("pauseButton", G_CALLBACK(on_pause_button_activate), builder);
    setup_menu_item("resetButton", G_CALLBACK(on_reset_button_activate), builder);
    setup_menu_item("fullscreenButton", G_CALLBACK(on_fullscreen_button_activate), builder);

    // Create the GtkGlArea
    glArea = GTK_WIDGET(gtk_builder_get_object(builder, "glArea"));
    g_signal_connect(G_OBJECT(glArea), "realize", G_CALLBACK(on_realize_gl_area), NULL);
    g_signal_connect(G_OBJECT(glArea), "render", G_CALLBACK(render), NULL);

    // Make the GL area update at the monitor's refresh rate.
    gtk_widget_add_tick_callback(glArea, tick_cb, NULL, NULL);

    // Without this, the window won't get mouse movement events when the pointer is over the GL area.
    gtk_widget_add_events(glArea, GDK_POINTER_MOTION_MASK);

    // 6:5 aspect ratio with exact 2x scaling on the vertical axis
    gtk_widget_set_size_request(glArea, 584, 486);

    // Set up the menu to select the save state slot. There are 10 slots, numbered from 0 to 9.
    GtkMenuShell *state_menu = GTK_MENU_SHELL(gtk_builder_get_object(builder, "stateSelectMenu"));

    // Create the menu items for the state slots.
    GSList *state_group = NULL;
    for (int i = 0; i < 10; i++)
    {
        char text[2] = {"0"};
        text[0] = '0' + i;
        GtkWidget *state_entry = state_entry = gtk_radio_menu_item_new_with_label(state_group, text);
        gtk_menu_shell_append(state_menu, state_entry);
        g_signal_connect(G_OBJECT(state_entry), "toggled", G_CALLBACK(on_state_slot_selected), NULL);

        if (i == 0)
        {
            state_group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(state_entry));
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(state_entry), TRUE);
        }
    }

    // Set up the menu to select the video scale factor.
    GtkMenuShell *video_size_menu = GTK_MENU_SHELL(gtk_builder_get_object(builder, "videoSizeMenu"));
    GSList *video_size_group = NULL;
    for (int i = 1; i < 5; i++)
    {
        char text[3] = {"1x"};
        text[0] = '0' + i;
        GtkWidget *entry = gtk_radio_menu_item_new_with_label(video_size_group, text);
        gtk_menu_shell_append(video_size_menu, entry);
        g_signal_connect(G_OBJECT(entry), "toggled", G_CALLBACK(on_video_size_selected), glArea);

        if (i == 1)
            video_size_group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(entry));
        if (i == 2)
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(entry), TRUE);
    }

    // Connect the quick load/save state functionality.
    setup_menu_item("quickLoad", G_CALLBACK(on_quick_load_activate), NULL);
    setup_menu_item("quickSave", G_CALLBACK(on_quick_save_activate), NULL);

    gtk_widget_show_all(GTK_WIDGET(window));

    gtk_widget_add_events(GTK_WIDGET(window), GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK);
    g_signal_connect(G_OBJECT(window), "key_press_event", G_CALLBACK(handle_key_press), NULL);
    g_signal_connect(G_OBJECT(window), "key_release_event", G_CALLBACK(handle_key_release), NULL);

    g_mutex_init(&g_frame_lock);
    g_cond_init(&g_ready_cond);

    if (argc > 1)
    {
        load_game(argv[1]);
    }

    gtk_main();

    return 0;
}


