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
#include "retrocore.h"

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>

static GLuint shader_program = 0;
static bool texture_inited = 0;
static GThread *emu_thread = NULL;
GMutex g_frame_lock = {0};
GCond g_ready_cond = {0};
int64_t last_frame_count = -1;
static unsigned int state_slot = 0;

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
        glPixelStorei(GL_UNPACK_ROW_LENGTH, frame->pitch / 2);
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
    gtk_widget_queue_draw(gl_area_w);
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
}

static void load_game(const char *path)
{
    if (emu_thread)
        close_game();

    retrocore_init("./mednafen_pce_libretro.so");
    retrocore_load_game(path);
    emu_thread = g_thread_new("emulator", retrocore_run_game, NULL);
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
    retrocore_load_state(state_slot);
}

static void on_quick_save_activate(GtkMenuItem *item, gpointer unused)
{
    retrocore_save_state(state_slot);
}

int main(int argc, char **argv)
{
    GtkBuilder *builder;
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

    g_signal_connect(gtk_builder_get_object(builder, "openButton"), "activate", G_CALLBACK(on_open_button_activate), builder);
    g_signal_connect(gtk_builder_get_object(builder, "closeButton"), "activate", G_CALLBACK(on_close_button_activate), builder);
    g_signal_connect(gtk_builder_get_object(builder, "exitButton"), "activate", G_CALLBACK(gtk_main_quit), builder);

    // Create the GtkGlArea
    glArea = gtk_gl_area_new();
    box = gtk_builder_get_object(builder, "mainBox");
    gtk_box_pack_end(GTK_BOX(box), glArea, TRUE, TRUE, 0);
    g_signal_connect(glArea, "realize", G_CALLBACK(on_realize_gl_area), NULL);
    g_signal_connect(glArea, "render", G_CALLBACK(render), NULL);
    gtk_widget_add_tick_callback(glArea, tick_cb, NULL, NULL);

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

    // Connect the quick load/save state functionality.
    g_signal_connect(gtk_builder_get_object(builder, "quickLoad"), "activate",
                     G_CALLBACK(on_quick_load_activate), NULL);
    g_signal_connect(gtk_builder_get_object(builder, "quickSave"), "activate",
                     G_CALLBACK(on_quick_save_activate), NULL);

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


