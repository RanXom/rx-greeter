#include "config.h"
#include "greetd.h"
#include "sessions.h"
#include "ui.h"
#include "users.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <math.h>
#include <pwd.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>

#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

#ifndef RX_GREETER_VERSION
#define RX_GREETER_VERSION "1.0.1"
#endif

struct App;
struct Output;

struct Buffer {
    struct wl_buffer *wl_buffer;
    void *data;
    size_t size;
    cairo_surface_t *surface;
    int width;
    int height;
    bool busy;
    struct Output *output;
};

struct Output {
    struct App *app;
    struct wl_output *wl_output;
    uint32_t wl_output_version;
    uint32_t global_name;
    char name[64];
    int wl_scale;
    int scale_120;
    bool fractional_scale_set;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wl_surface *surface;
    struct wp_fractional_scale_v1 *fractional_scale;
    struct wp_viewport *viewport;
    int width;
    int height;
    bool configured;
    bool primary;
    struct Buffer *buffers[2];
    struct wl_list link;
};

struct App {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    uint32_t compositor_version;
    struct wl_shm *shm;
    struct zwlr_layer_shell_v1 *layer_shell;
    uint32_t layer_shell_version;
    struct wp_fractional_scale_manager_v1 *fractional_manager;
    struct wp_viewporter *viewporter;
    struct wl_seat *seat;
    uint32_t seat_version;
    struct wl_keyboard *keyboard;
    struct wl_pointer *pointer;

    struct wl_list outputs;
    size_t output_count;
    struct Output *primary_output;
    bool setup_done;
    bool cleaning;

    bool running;
    int exit_code;
    bool needs_redraw;
    const Config *config;

    struct xkb_context *xkb_context;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;
    struct xkb_compose_table *compose_table;
    struct xkb_compose_state *compose_state;
    int repeat_key;
    int repeat_rate;
    int repeat_delay;
    int64_t repeat_next_ms;

    struct Output *pointer_output;
    double pointer_x;
    double pointer_y;

    Ui *ui;
};

static const struct wl_seat_listener seat_listener;

static volatile sig_atomic_t signal_stop = 0;

static void
handle_signal(int signo)
{
    (void)signo;
    signal_stop = 1;
}

static int
create_shm_file(size_t size)
{
    int fd = memfd_create("rx-greeter-shm", MFD_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static double
output_scale_factor(const struct Output *output)
{
    if (output->viewport && output->fractional_scale_set &&
        output->scale_120 > 0) {
        return fmax(1.0, (double)output->scale_120 / 120.0);
    }
    if (output->wl_scale > 0) {
        return (double)output->wl_scale;
    }
    return 1.0;
}

static int
output_physical_width(const struct Output *output)
{
    double scale = fmax(1.0, output_scale_factor(output));
    return (int)ceil((double)output->width * scale);
}

static int
output_physical_height(const struct Output *output)
{
    double scale = fmax(1.0, output_scale_factor(output));
    return (int)ceil((double)output->height * scale);
}

static void
buffer_release(void *data, struct wl_buffer *wl_buffer)
{
    (void)wl_buffer;
    struct Buffer *buffer = data;
    buffer->busy = false;
    /* A released buffer is not itself a reason to render again. The main loop
     * renders when the UI is dirty or a configure/scale change is pending;
     * scheduling a frame here causes a permanent redraw loop. */
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static void
destroy_buffer(struct Buffer *buffer)
{
    if (!buffer) {
        return;
    }
    if (buffer->wl_buffer) {
        wl_buffer_destroy(buffer->wl_buffer);
    }
    if (buffer->surface) {
        cairo_surface_destroy(buffer->surface);
    }
    if (buffer->data && buffer->size) {
        munmap(buffer->data, buffer->size);
    }
    free(buffer);
}

static void
destroy_output_buffers(struct Output *output)
{
    for (size_t i = 0; i < 2; i++) {
        destroy_buffer(output->buffers[i]);
        output->buffers[i] = NULL;
    }
}

static struct Buffer *
create_buffer(struct Output *output, int width, int height)
{
    struct App *app = output->app;
    if (!app->shm || width <= 0 || height <= 0) {
        return NULL;
    }
    size_t stride = (size_t)width * 4;
    size_t size = stride * (size_t)height;
    int fd = create_shm_file(size);
    if (fd < 0) {
        rx_log(LOG_ERR, "memfd_create failed: %s", strerror(errno));
        return NULL;
    }

    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        rx_log(LOG_ERR, "mmap failed: %s", strerror(errno));
        close(fd);
        return NULL;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(app->shm, fd, (int32_t)size);
    struct wl_buffer *wl_buffer = NULL;
    if (pool) {
        wl_buffer = wl_shm_pool_create_buffer(pool, 0, width, height,
                                              (int32_t)stride,
                                              WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
    }
    close(fd);
    if (!wl_buffer) {
        munmap(data, size);
        return NULL;
    }

    cairo_surface_t *surface = cairo_image_surface_create_for_data(
        data, CAIRO_FORMAT_ARGB32, width, height, (int)stride);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        wl_buffer_destroy(wl_buffer);
        munmap(data, size);
        return NULL;
    }

    struct Buffer *buffer = xcalloc(1, sizeof(*buffer));
    buffer->wl_buffer = wl_buffer;
    buffer->data = data;
    buffer->size = size;
    buffer->surface = surface;
    buffer->width = width;
    buffer->height = height;
    buffer->output = output;
    wl_buffer_add_listener(wl_buffer, &buffer_listener, buffer);
    return buffer;
}

static bool
output_buffers_match(const struct Output *output)
{
    int width = output_physical_width(output);
    int height = output_physical_height(output);
    return output->buffers[0] && output->buffers[1] &&
           output->buffers[0]->width == width &&
           output->buffers[0]->height == height &&
           output->buffers[1]->width == width &&
           output->buffers[1]->height == height;
}

static bool
create_output_buffers(struct Output *output)
{
    destroy_output_buffers(output);
    int width = output_physical_width(output);
    int height = output_physical_height(output);
    output->buffers[0] = create_buffer(output, width, height);
    output->buffers[1] = create_buffer(output, width, height);
    if (!output->buffers[0] || !output->buffers[1]) {
        destroy_output_buffers(output);
        return false;
    }
    return true;
}

static struct Buffer *
pick_buffer(struct Output *output)
{
    for (size_t i = 0; i < 2; i++) {
        if (output->buffers[i] && !output->buffers[i]->busy) {
            return output->buffers[i];
        }
    }
    return NULL;
}

static bool
render_output(struct Output *output)
{
    if (!output->configured || !output->surface ||
        output->width <= 0 || output->height <= 0) {
        return false;
    }
    if (!output_buffers_match(output) && !create_output_buffers(output)) {
        return false;
    }
    struct Buffer *buffer = pick_buffer(output);
    if (!buffer) {
        return false;
    }

    double scale = output_scale_factor(output);
    cairo_t *cr = cairo_create(buffer->surface);
    if (!cr) {
        return false;
    }
    cairo_scale(cr, scale, scale);
    ui_render(output->app->ui, cr, output->width, output->height);
    cairo_destroy(cr);
    cairo_surface_flush(buffer->surface);

    if (output->viewport) {
        wp_viewport_set_destination(output->viewport, output->width,
                                    output->height);
        wl_surface_set_buffer_scale(output->surface, 1);
    } else {
        wl_surface_set_buffer_scale(output->surface, (int)scale);
    }
    buffer->busy = true;
    wl_surface_attach(output->surface, buffer->wl_buffer, 0, 0);
    if (output->app->compositor_version >= 4) {
        wl_surface_damage_buffer(output->surface, 0, 0, buffer->width,
                                 buffer->height);
    } else {
        wl_surface_damage(output->surface, 0, 0, output->width,
                          output->height);
    }
    wl_surface_commit(output->surface);
    return true;
}

static bool
render_all_outputs(struct App *app)
{
    bool any = false;
    bool all = true;
    size_t configured = 0;
    struct Output *output;
    wl_list_for_each(output, &app->outputs, link) {
        if (!output->configured) {
            continue;
        }
        configured++;
        if (render_output(output)) {
            any = true;
        } else {
            all = false;
        }
    }
    return configured > 0 && any && all;
}

static void
select_primary(struct App *app)
{
    const char *want = getenv("RX_GREETER_OUTPUT");
    if (!want || !*want) {
        want = app->config->output_name;
    }

    struct Output *selected = NULL;
    struct Output *first = NULL;
    struct Output *output;
    wl_list_for_each(output, &app->outputs, link) {
        if (!first) {
            first = output;
        }
        if (want && *want && strcmp(output->name, want) == 0) {
            selected = output;
            break;
        }
    }
    if (!selected) {
        selected = first;
    }
    if (selected == app->primary_output) {
        if (app->ui) {
            ui_set_output_info(app->ui, (int)app->output_count,
                               selected ? selected->name : "",
                               selected ? (selected->fractional_scale_set
                                               ? selected->scale_120
                                               : selected->wl_scale * 120)
                                        : 120,
                               selected ? selected->width : 0,
                               selected ? selected->height : 0);
        }
        return;
    }
    app->primary_output = selected;
    wl_list_for_each(output, &app->outputs, link) {
        output->primary = output == selected;
        if (output->layer_surface) {
            if (output->primary) {
                zwlr_layer_surface_v1_set_keyboard_interactivity(
                    output->layer_surface,
                    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
            } else if (app->layer_shell_version >= 4) {
                zwlr_layer_surface_v1_set_keyboard_interactivity(
                    output->layer_surface,
                    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND);
            } else {
                zwlr_layer_surface_v1_set_keyboard_interactivity(
                    output->layer_surface,
                    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
            }
            wl_surface_commit(output->surface);
        }
    }
    if (app->ui) {
        ui_set_output_info(app->ui, (int)app->output_count,
                           selected ? selected->name : "",
                           selected ? (selected->fractional_scale_set
                                           ? selected->scale_120
                                           : selected->wl_scale * 120)
                                    : 120,
                           selected ? selected->width : 0,
                           selected ? selected->height : 0);
    }
    app->needs_redraw = true;
}

static void output_create_layer(struct Output *output);
static void output_destroy(struct Output *output);

static void
layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                        uint32_t serial, uint32_t width, uint32_t height)
{
    struct Output *output = data;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    if (width == 0 || height == 0) {
        return;
    }
    output->width = (int)width;
    output->height = (int)height;
    output->configured = true;
    if (!output_buffers_match(output) && !create_output_buffers(output)) {
        rx_log(LOG_ERR, "failed to create buffers for output %s", output->name);
        return;
    }
    ui_resize(output->app->ui, output->width, output->height);
    ui_start(output->app->ui);
    if (output->primary) {
        ui_set_output_info(output->app->ui, (int)output->app->output_count,
                           output->name,
                           output->fractional_scale_set
                               ? output->scale_120
                               : output->wl_scale * 120,
                           output->width, output->height);
    }
    output->app->needs_redraw = true;
}

static void
layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
    (void)surface;
    output_destroy(data);
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

static void
fractional_scale_preferred(void *data,
                           struct wp_fractional_scale_v1 *fractional_scale,
                           uint32_t scale)
{
    (void)fractional_scale;
    struct Output *output = data;
    if ((int)scale == output->scale_120 && output->fractional_scale_set) {
        return;
    }
    output->scale_120 = (int)scale;
    output->fractional_scale_set = true;
    destroy_output_buffers(output);
    if (output->configured) {
        if (!create_output_buffers(output)) {
            rx_log(LOG_ERR, "failed to recreate buffers for output %s",
                   output->name);
        }
        if (output->primary) {
            ui_set_output_info(output->app->ui,
                               (int)output->app->output_count, output->name,
                               output->scale_120, output->width,
                               output->height);
        }
    }
    output->app->needs_redraw = true;
}

static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
    .preferred_scale = fractional_scale_preferred,
};

static void
output_geometry(void *data, struct wl_output *wl_output, int32_t x, int32_t y,
                int32_t physical_width, int32_t physical_height,
                int32_t subpixel, const char *make, const char *model,
                int32_t transform)
{
    (void)data;
    (void)wl_output;
    (void)x;
    (void)y;
    (void)physical_width;
    (void)physical_height;
    (void)subpixel;
    (void)make;
    (void)model;
    (void)transform;
}

static void
output_mode(void *data, struct wl_output *wl_output, uint32_t flags,
            int32_t width, int32_t height, int32_t refresh)
{
    (void)data;
    (void)wl_output;
    (void)flags;
    (void)width;
    (void)height;
    (void)refresh;
}

static void
output_done(void *data, struct wl_output *wl_output)
{
    (void)wl_output;
    struct Output *output = data;
    if (!output->app->setup_done) {
        return;
    }
    select_primary(output->app);
    if (!output->layer_surface) {
        output_create_layer(output);
    }
}

static void
output_scale(void *data, struct wl_output *wl_output, int32_t factor)
{
    (void)wl_output;
    struct Output *output = data;
    if (factor <= 0) {
        factor = 1;
    }
    if (output->wl_scale == factor) {
        return;
    }
    output->wl_scale = factor;
    if (!output->fractional_scale_set) {
        destroy_output_buffers(output);
        output->app->needs_redraw = true;
        if (output->primary && output->app->ui) {
            ui_set_output_info(output->app->ui,
                               (int)output->app->output_count, output->name,
                               factor * 120, output->width, output->height);
        }
    }
}

static void
output_name(void *data, struct wl_output *wl_output, const char *name)
{
    (void)wl_output;
    struct Output *output = data;
    snprintf(output->name, sizeof(output->name), "%s", name ? name : "");
    if (output->app->setup_done && output->layer_surface) {
        select_primary(output->app);
    }
}

static void
output_description(void *data, struct wl_output *wl_output,
                   const char *description)
{
    (void)data;
    (void)wl_output;
    (void)description;
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_name,
    .description = output_description,
};

static struct Output *
output_for_surface(struct App *app, struct wl_surface *surface)
{
    struct Output *output;
    wl_list_for_each(output, &app->outputs, link) {
        if (output->surface == surface) {
            return output;
        }
    }
    return NULL;
}

static void
output_create_layer(struct Output *output)
{
    struct App *app = output->app;
    if (!app->layer_shell || output->layer_surface) {
        return;
    }

    output->surface = wl_compositor_create_surface(app->compositor);
    if (!output->surface) {
        return;
    }
    output->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        app->layer_shell, output->surface, output->wl_output,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "rx-greeter");
    if (!output->layer_surface) {
        wl_surface_destroy(output->surface);
        output->surface = NULL;
        return;
    }
    zwlr_layer_surface_v1_add_listener(output->layer_surface,
                                       &layer_surface_listener, output);
    if (app->viewporter) {
        output->viewport = wp_viewporter_get_viewport(app->viewporter,
                                                      output->surface);
    }
    if (app->fractional_manager && output->viewport) {
        output->fractional_scale =
            wp_fractional_scale_manager_v1_get_fractional_scale(
                app->fractional_manager, output->surface);
        if (output->fractional_scale) {
            wp_fractional_scale_v1_add_listener(
                output->fractional_scale, &fractional_scale_listener, output);
        }
    }

    zwlr_layer_surface_v1_set_size(output->layer_surface, 0, 0);
    zwlr_layer_surface_v1_set_anchor(
        output->layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(output->layer_surface, -1);
    zwlr_layer_surface_v1_set_margin(output->layer_surface, 0, 0, 0, 0);
    if (output->primary) {
        zwlr_layer_surface_v1_set_keyboard_interactivity(
            output->layer_surface,
            ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
    } else if (app->layer_shell_version >= 4) {
        zwlr_layer_surface_v1_set_keyboard_interactivity(
            output->layer_surface,
            ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND);
    } else {
        zwlr_layer_surface_v1_set_keyboard_interactivity(
            output->layer_surface,
            ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    }
    wl_surface_commit(output->surface);
}

static void
output_destroy(struct Output *output)
{
    struct App *app = output->app;
    bool was_primary = output == app->primary_output;
    destroy_output_buffers(output);
    if (output->fractional_scale) {
        wp_fractional_scale_v1_destroy(output->fractional_scale);
    }
    if (output->viewport) {
        wp_viewport_destroy(output->viewport);
    }
    if (output->layer_surface) {
        zwlr_layer_surface_v1_destroy(output->layer_surface);
    }
    if (output->surface) {
        wl_surface_destroy(output->surface);
    }
    if (output->wl_output) {
        if (output->wl_output_version >= 3) {
            wl_output_release(output->wl_output);
        } else {
            wl_output_destroy(output->wl_output);
        }
    }
    wl_list_remove(&output->link);
    app->output_count--;
    if (app->pointer_output == output) {
        app->pointer_output = NULL;
    }
    if (was_primary) {
        app->primary_output = NULL;
    }
    free(output);
    if (app->setup_done && !app->cleaning) {
        select_primary(app);
    }
}

static void
setup_outputs(struct App *app)
{
    app->setup_done = true;
    select_primary(app);
    struct Output *output;
    wl_list_for_each(output, &app->outputs, link) {
        output_create_layer(output);
    }
}

static void
registry_global(void *data, struct wl_registry *registry, uint32_t name,
                const char *interface, uint32_t version)
{
    struct App *app = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        uint32_t bind_version = version < 4 ? version : 4;
        app->compositor_version = bind_version;
        app->compositor = wl_registry_bind(registry, name,
                                           &wl_compositor_interface,
                                           bind_version);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        app->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        uint32_t bind_version = version < 5 ? version : 5;
        app->seat_version = bind_version;
        app->seat = wl_registry_bind(registry, name, &wl_seat_interface,
                                     bind_version);
        wl_seat_add_listener(app->seat, &seat_listener, app);
    } else if (strcmp(interface,
                      zwlr_layer_shell_v1_interface.name) == 0) {
        uint32_t bind_version = version < 4 ? version : 4;
        app->layer_shell_version = bind_version;
        app->layer_shell = wl_registry_bind(registry, name,
                                            &zwlr_layer_shell_v1_interface,
                                            bind_version);
    } else if (strcmp(interface,
                      wp_fractional_scale_manager_v1_interface.name) == 0) {
        app->fractional_manager = wl_registry_bind(
            registry, name, &wp_fractional_scale_manager_v1_interface, 1);
    } else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
        app->viewporter = wl_registry_bind(registry, name,
                                           &wp_viewporter_interface, 1);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        uint32_t bind_version = version < 4 ? version : 4;
        struct Output *output = xcalloc(1, sizeof(*output));
        output->app = app;
        output->global_name = name;
        output->wl_output_version = bind_version;
        output->wl_scale = 1;
        output->scale_120 = 120;
        output->wl_output = wl_registry_bind(registry, name,
                                             &wl_output_interface,
                                             bind_version);
        wl_output_add_listener(output->wl_output, &output_listener, output);
        wl_list_insert(app->outputs.prev, &output->link);
        app->output_count++;
    }
}

static void
registry_global_remove(void *data, struct wl_registry *registry,
                       uint32_t name)
{
    (void)registry;
    struct App *app = data;
    struct Output *output;
    wl_list_for_each(output, &app->outputs, link) {
        if (output->global_name == name) {
            output_destroy(output);
            return;
        }
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static bool
mod_active(struct App *app, const char *name, enum xkb_state_component component)
{
    return app->xkb_state &&
           xkb_state_mod_name_is_active(app->xkb_state, name, component) > 0;
}

static bool
key_repeatable(xkb_keysym_t sym)
{
    switch (sym) {
    case XKB_KEY_BackSpace:
    case XKB_KEY_Delete:
    case XKB_KEY_Left:
    case XKB_KEY_Right:
    case XKB_KEY_Up:
    case XKB_KEY_Down:
        return true;
    default:
        break;
    }
    return xkb_keysym_to_utf32(sym) != 0;
}

static void
dispatch_key(struct App *app, xkb_keycode_t keycode, bool repeat)
{
    if (!app->xkb_state) {
        return;
    }
    xkb_keysym_t sym = xkb_state_key_get_one_sym(app->xkb_state, keycode);
    char text[64] = { 0 };
    bool composing = false;
    bool composed = false;

    if (!repeat && app->compose_state) {
        (void)xkb_compose_state_feed(app->compose_state, sym);
        enum xkb_compose_status status =
            xkb_compose_state_get_status(app->compose_state);
        if (status == XKB_COMPOSE_COMPOSING) {
            composing = true;
            if (sym == XKB_KEY_BackSpace) {
                xkb_compose_state_reset(app->compose_state);
                return;
            }
        } else if (status == XKB_COMPOSE_COMPOSED) {
            xkb_keysym_t composed_sym =
                xkb_compose_state_get_one_sym(app->compose_state);
            xkb_keysym_to_utf8(composed_sym, text, sizeof(text));
            xkb_compose_state_reset(app->compose_state);
            composed = true;
        } else if (status == XKB_COMPOSE_CANCELLED) {
            xkb_compose_state_reset(app->compose_state);
        }
    }

    if (!composing && !composed) {
        xkb_state_key_get_utf8(app->xkb_state, keycode, text, sizeof(text));
    }
    if (app->compose_state &&
        (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter ||
         sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab ||
         sym == XKB_KEY_Escape)) {
        xkb_compose_state_reset(app->compose_state);
    }

    bool ctrl = mod_active(app, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE);
    bool shift = mod_active(app, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE);
    ui_key(app->ui, sym, text, ctrl, shift);
}

static void
keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format,
                int32_t fd, uint32_t size)
{
    (void)keyboard;
    struct App *app = data;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }
    void *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        rx_log(LOG_ERR, "failed to map xkb keymap: %s", strerror(errno));
        return;
    }
    struct xkb_keymap *keymap = xkb_keymap_new_from_string(
        app->xkb_context, map, XKB_KEYMAP_FORMAT_TEXT_V1,
        XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    if (!keymap) {
        rx_log(LOG_ERR, "failed to parse xkb keymap");
        return;
    }
    struct xkb_state *state = xkb_state_new(keymap);
    if (!state) {
        xkb_keymap_unref(keymap);
        rx_log(LOG_ERR, "failed to create xkb state");
        return;
    }
    if (app->xkb_state) {
        xkb_state_unref(app->xkb_state);
    }
    if (app->xkb_keymap) {
        xkb_keymap_unref(app->xkb_keymap);
    }
    app->xkb_keymap = keymap;
    app->xkb_state = state;
}

static void
keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
               struct wl_surface *surface, struct wl_array *keys)
{
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)surface;
    (void)keys;
}

static void
keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial,
               struct wl_surface *surface)
{
    (void)keyboard;
    (void)serial;
    (void)surface;
    struct App *app = data;
    app->repeat_key = 0;
    if (app->compose_state) {
        xkb_compose_state_reset(app->compose_state);
    }
}

static void
keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial,
             uint32_t time, uint32_t key, uint32_t state)
{
    (void)keyboard;
    (void)serial;
    (void)time;
    struct App *app = data;
    xkb_keycode_t keycode = key + 8;

    if (state != WL_KEYBOARD_KEY_STATE_PRESSED) {
        if (app->repeat_key == (int)keycode) {
            app->repeat_key = 0;
        }
        return;
    }
    if (!app->xkb_state) {
        return;
    }

    dispatch_key(app, keycode, false);
    xkb_keysym_t sym = xkb_state_key_get_one_sym(app->xkb_state, keycode);
    if (key_repeatable(sym)) {
        app->repeat_key = (int)keycode;
        app->repeat_next_ms = now_ms() + app->repeat_delay;
    } else {
        app->repeat_key = 0;
    }
}

static void
keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                   uint32_t mods_depressed, uint32_t mods_latched,
                   uint32_t mods_locked, uint32_t group)
{
    (void)keyboard;
    (void)serial;
    struct App *app = data;
    if (app->xkb_state) {
        xkb_state_update_mask(app->xkb_state, mods_depressed, mods_latched,
                              mods_locked, 0, 0, group);
        bool caps = xkb_state_mod_name_is_active(
                        app->xkb_state, XKB_MOD_NAME_CAPS,
                        XKB_STATE_MODS_LOCKED) > 0;
        ui_set_caps_lock(app->ui, caps);
    }
}

static void
keyboard_repeat_info(void *data, struct wl_keyboard *keyboard,
                     int32_t rate, int32_t delay)
{
    (void)keyboard;
    struct App *app = data;
    app->repeat_rate = rate > 0 ? rate : 25;
    app->repeat_delay = delay >= 0 ? delay : 500;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

static void
pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
              struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy)
{
    struct App *app = data;
    app->pointer_output = output_for_surface(app, surface);
    app->pointer_x = wl_fixed_to_double(sx);
    app->pointer_y = wl_fixed_to_double(sy);
    ui_pointer_enter(app->ui, app->pointer_x, app->pointer_y);
    if (!ui_show_cursor(app->ui)) {
        wl_pointer_set_cursor(pointer, serial, NULL, 0, 0);
    }
}

static void
pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
              struct wl_surface *surface)
{
    (void)pointer;
    (void)serial;
    (void)surface;
    struct App *app = data;
    app->pointer_output = NULL;
    ui_pointer_leave(app->ui);
}

static void
pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
               wl_fixed_t sx, wl_fixed_t sy)
{
    (void)pointer;
    (void)time;
    struct App *app = data;
    app->pointer_x = wl_fixed_to_double(sx);
    app->pointer_y = wl_fixed_to_double(sy);
    ui_pointer_motion(app->ui, app->pointer_x, app->pointer_y);
}

static void
pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
               uint32_t time, uint32_t button, uint32_t state)
{
    (void)pointer;
    (void)serial;
    (void)time;
    struct App *app = data;
    struct Output *output = app->pointer_output;
    int width = output ? output->width : 0;
    int height = output ? output->height : 0;
    ui_pointer_button(app->ui, app->pointer_x, app->pointer_y, width, height,
                      button, state == WL_POINTER_BUTTON_STATE_PRESSED);
}

static void
pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
             uint32_t axis, wl_fixed_t value)
{
    (void)data;
    (void)pointer;
    (void)time;
    (void)axis;
    (void)value;
}

static void
pointer_frame(void *data, struct wl_pointer *pointer)
{
    (void)data;
    (void)pointer;
}

static void
pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t source)
{
    (void)data;
    (void)pointer;
    (void)source;
}

static void
pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time,
                  uint32_t axis)
{
    (void)data;
    (void)pointer;
    (void)time;
    (void)axis;
}

static void
pointer_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis,
                      int32_t discrete)
{
    (void)data;
    (void)pointer;
    (void)axis;
    (void)discrete;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
};

static void
seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities)
{
    struct App *app = data;
    bool has_keyboard = capabilities & WL_SEAT_CAPABILITY_KEYBOARD;
    bool has_pointer = capabilities & WL_SEAT_CAPABILITY_POINTER;

    if (has_keyboard && !app->keyboard) {
        app->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(app->keyboard, &keyboard_listener, app);
    } else if (!has_keyboard && app->keyboard) {
        if (app->seat_version >= 3) {
            wl_keyboard_release(app->keyboard);
        } else {
            wl_keyboard_destroy(app->keyboard);
        }
        app->keyboard = NULL;
        app->repeat_key = 0;
    }

    if (has_pointer && !app->pointer) {
        app->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(app->pointer, &pointer_listener, app);
    } else if (!has_pointer && app->pointer) {
        if (app->seat_version >= 3) {
            wl_pointer_release(app->pointer);
        } else {
            wl_pointer_destroy(app->pointer);
        }
        app->pointer = NULL;
        app->pointer_output = NULL;
    }
}

static void
seat_name(void *data, struct wl_seat *seat, const char *name)
{
    (void)data;
    (void)seat;
    (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

static void
repeat_tick(struct App *app)
{
    if (app->repeat_key <= 0 || !app->xkb_state) {
        return;
    }
    int64_t now = now_ms();
    if (now < app->repeat_next_ms) {
        return;
    }
    xkb_keycode_t keycode = (xkb_keycode_t)app->repeat_key;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(app->xkb_state, keycode);
    if (!key_repeatable(sym)) {
        app->repeat_key = 0;
        return;
    }
    dispatch_key(app, keycode, true);
    int rate = app->repeat_rate > 0 ? app->repeat_rate : 25;
    app->repeat_next_ms = now + 1000 / rate;
}

static void
cleanup_app(struct App *app)
{
    if (!app) {
        return;
    }
    app->cleaning = true;
    app->setup_done = false;
    while (!wl_list_empty(&app->outputs)) {
        struct Output *output = wl_container_of(app->outputs.next,
                                                output, link);
        output_destroy(output);
    }
    if (app->ui) {
        ui_destroy(app->ui);
        app->ui = NULL;
    }
    if (app->keyboard) {
        if (app->seat_version >= 3) {
            wl_keyboard_release(app->keyboard);
        } else {
            wl_keyboard_destroy(app->keyboard);
        }
        app->keyboard = NULL;
    }
    if (app->pointer) {
        if (app->seat_version >= 3) {
            wl_pointer_release(app->pointer);
        } else {
            wl_pointer_destroy(app->pointer);
        }
        app->pointer = NULL;
    }
    if (app->seat) {
        if (app->seat_version >= 5) {
            wl_seat_release(app->seat);
        } else {
            wl_seat_destroy(app->seat);
        }
        app->seat = NULL;
    }
    if (app->layer_shell) {
        if (app->layer_shell_version >= 3) {
            zwlr_layer_shell_v1_destroy(app->layer_shell);
        } else {
            wl_proxy_destroy((struct wl_proxy *)app->layer_shell);
        }
        app->layer_shell = NULL;
    }
    if (app->fractional_manager) {
        wp_fractional_scale_manager_v1_destroy(app->fractional_manager);
        app->fractional_manager = NULL;
    }
    if (app->viewporter) {
        wp_viewporter_destroy(app->viewporter);
        app->viewporter = NULL;
    }
    if (app->xkb_state) {
        xkb_state_unref(app->xkb_state);
        app->xkb_state = NULL;
    }
    if (app->xkb_keymap) {
        xkb_keymap_unref(app->xkb_keymap);
        app->xkb_keymap = NULL;
    }
    if (app->compose_state) {
        xkb_compose_state_unref(app->compose_state);
        app->compose_state = NULL;
    }
    if (app->compose_table) {
        xkb_compose_table_unref(app->compose_table);
        app->compose_table = NULL;
    }
    if (app->xkb_context) {
        xkb_context_unref(app->xkb_context);
        app->xkb_context = NULL;
    }
    if (app->shm) {
        wl_shm_destroy(app->shm);
        app->shm = NULL;
    }
    if (app->compositor) {
        wl_compositor_destroy(app->compositor);
        app->compositor = NULL;
    }
    if (app->registry) {
        wl_registry_destroy(app->registry);
        app->registry = NULL;
    }
    if (app->display) {
        wl_display_disconnect(app->display);
        app->display = NULL;
    }
}

static int
run_greeter(const Config *config, const char *debug_status)
{
    struct App app;
    memset(&app, 0, sizeof(app));
    wl_list_init(&app.outputs);
    app.running = true;
    app.repeat_rate = 25;
    app.repeat_delay = 500;
    app.config = config;
    app.pointer_x = -1;
    app.pointer_y = -1;

    app.display = wl_display_connect(NULL);
    if (!app.display) {
        rx_log(LOG_ERR, "cannot connect to a Wayland compositor");
        return 1;
    }
    app.registry = wl_display_get_registry(app.display);
    if (!app.registry) {
        cleanup_app(&app);
        return 1;
    }
    wl_registry_add_listener(app.registry, &registry_listener, &app);
    if (wl_display_roundtrip(app.display) < 0) {
        rx_log(LOG_ERR, "Wayland registry roundtrip failed");
        cleanup_app(&app);
        return 1;
    }
    if (!app.compositor || !app.shm) {
        rx_log(LOG_ERR, "compositor does not advertise wl_compositor and wl_shm");
        cleanup_app(&app);
        return 1;
    }
    if (!app.layer_shell) {
        rx_log(LOG_ERR,
               "compositor does not advertise wlr-layer-shell; rx-greeter "
               "requires an output-aware layer-shell compositor (Sway)");
        cleanup_app(&app);
        return 1;
    }
    if (!app.fractional_manager || !app.viewporter) {
        rx_log(LOG_WARNING,
               "fractional scale protocols unavailable; using integer scale");
    }
    if (app.output_count == 0) {
        rx_log(LOG_ERR, "compositor did not advertise any outputs");
        cleanup_app(&app);
        return 1;
    }

    app.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!app.xkb_context) {
        rx_log(LOG_ERR, "cannot create xkb context");
        cleanup_app(&app);
        return 1;
    }
    const char *locale_name = setlocale(LC_CTYPE, NULL);
    if (!locale_name || !*locale_name) {
        locale_name = "C";
    }
    app.compose_table = xkb_compose_table_new_from_locale(
        app.xkb_context, locale_name, XKB_COMPOSE_COMPILE_NO_FLAGS);
    if (!app.compose_table && strcmp(locale_name, "C") != 0) {
        app.compose_table = xkb_compose_table_new_from_locale(
            app.xkb_context, "C", XKB_COMPOSE_COMPILE_NO_FLAGS);
    }
    if (app.compose_table) {
        app.compose_state = xkb_compose_state_new(
            app.compose_table, XKB_COMPOSE_STATE_NO_FLAGS);
    }

    app.ui = ui_create((Config *)config, debug_status);
    setup_outputs(&app);

    while (app.running && !signal_stop) {
        while (wl_display_prepare_read(app.display) != 0) {
            if (wl_display_dispatch_pending(app.display) < 0) {
                app.running = false;
                break;
            }
        }
        if (!app.running) {
            wl_display_cancel_read(app.display);
            break;
        }
        wl_display_flush(app.display);

        struct pollfd fds[2];
        int fd_count = 0;
        fds[fd_count].fd = wl_display_get_fd(app.display);
        fds[fd_count].events = POLLIN;
        fds[fd_count].revents = 0;
        fd_count++;
        int greetd_fd = ui_greetd_fd(app.ui);
        if (greetd_fd >= 0) {
            fds[fd_count].fd = greetd_fd;
            fds[fd_count].events = POLLIN;
            fds[fd_count].revents = 0;
            fd_count++;
        }

        int timeout = ui_poll_timeout_ms(app.ui);
        if (app.repeat_key > 0) {
            int64_t now = now_ms();
            int64_t repeat_in = app.repeat_next_ms - now;
            if (repeat_in < 0) {
                repeat_in = 0;
            }
            if (repeat_in < timeout) {
                timeout = (int)repeat_in;
            }
        }

        int rc = poll(fds, (nfds_t)fd_count, timeout);
        if (rc < 0) {
            if (errno == EINTR) {
                wl_display_cancel_read(app.display);
                continue;
            }
            rx_log(LOG_ERR, "poll failed: %s", strerror(errno));
            wl_display_cancel_read(app.display);
            app.running = false;
            app.exit_code = 1;
            break;
        }

        if (fds[0].revents & POLLIN) {
            if (wl_display_read_events(app.display) < 0) {
                app.running = false;
                app.exit_code = 1;
                break;
            }
        } else {
            wl_display_cancel_read(app.display);
            if (fds[0].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                app.running = false;
                app.exit_code = 1;
                break;
            }
        }

        if (wl_display_dispatch_pending(app.display) < 0) {
            app.running = false;
            app.exit_code = 1;
            break;
        }

        if (fd_count > 1 &&
            (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            ui_greetd_readable(app.ui);
        }

        repeat_tick(&app);
        ui_tick(app.ui);

        if (ui_quit(app.ui)) {
            app.running = false;
            app.exit_code = ui_exit_code(app.ui);
            break;
        }

        if (ui_dirty(app.ui) || app.needs_redraw) {
            if (render_all_outputs(&app)) {
                ui_clear_dirty(app.ui);
                app.needs_redraw = false;
            }
        }
    }

    int exit_code = app.exit_code;
    cleanup_app(&app);
    return exit_code;
}

static bool
preferred_session_exists(const SessionList *list, const char *preferred)
{
    if (!preferred || !*preferred) {
        return true;
    }
    for (size_t i = 0; i < list->count; i++) {
        const Session *session = &list->items[i];
        if (str_ieq(session->id, preferred) ||
            str_ieq(session->name, preferred)) {
            return true;
        }
    }
    return false;
}

static bool
power_helper_available(const Config *config)
{
    if (config->power_helper && *config->power_helper) {
        return path_is_executable(config->power_helper);
    }
    char *sibling = find_sibling_executable(NULL, "rx-greeter-power");
    bool found = sibling != NULL;
    free(sibling);
    if (found) {
        return true;
    }
    static const char *const candidates[] = {
        "/usr/local/libexec/rx-greeter-power",
        "/usr/libexec/rx-greeter-power",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (path_is_executable(candidates[i])) {
            return true;
        }
    }
    return false;
}

static int
check_configuration(const Config *config)
{
    int errors = 0;

    if (config->background_image && *config->background_image &&
        !path_is_readable(config->background_image)) {
        rx_log(LOG_ERR, "configured background is not readable: %s",
               config->background_image);
        errors++;
    }
    if (!power_helper_available(config)) {
        rx_log(LOG_ERR, "no executable power helper was found");
        errors++;
    }

    UserList users;
    users_load(&users);
    if (config->default_user && *config->default_user) {
        if (!getpwnam(config->default_user)) {
            rx_log(LOG_ERR, "configured default_user does not exist: %s",
                   config->default_user);
            errors++;
        }
        if (users_find(&users, config->default_user) < 0) {
            rx_log(LOG_WARNING,
                   "configured default_user is not in the selectable user list: %s",
                   config->default_user);
        }
    } else if (users.count == 0) {
        rx_log(LOG_ERR, "no selectable human users were found");
        errors++;
    }
    users_free(&users);

    SessionList sessions;
    sessions_load(&sessions, config->session_dirs);
    if (sessions.count == 0) {
        rx_log(LOG_ERR, "no Wayland sessions were found");
        errors++;
    } else if (config->default_session && *config->default_session &&
               !preferred_session_exists(&sessions, config->default_session)) {
        rx_log(LOG_WARNING,
               "configured default_session was not found: %s",
               config->default_session);
    }
    sessions_free(&sessions);

    if (errors == 0) {
        printf("configuration OK\n");
        return 0;
    }
    fprintf(stderr, "configuration has %d error(s)\n", errors);
    return 1;
}

static void
print_usage(const char *program)
{
    printf("usage: %s [options]\n"
           "\n"
           "Options:\n"
           "  --config PATH             read host configuration from PATH\n"
           "  --theme PATH              read visual theme from PATH\n"
           "  --background PATH         override the wallpaper path\n"
           "  --default-user USER       default username (default: first human user)\n"
           "  --default-session NAME    preferred session (default: first available)\n"
           "  --output NAME             primary output for keyboard focus\n"
           "  --power-helper PATH       privileged power helper path\n"
           "  --power-dry-run           never perform power actions\n"
           "  --debug-status PATH       write a JSON state file (testing)\n"
           "  --print-config            print effective configuration and exit\n"
           "  --check-config            validate configuration and exit\n"
           "  --list-users              list selectable users and exit\n"
           "  --list-sessions           list Wayland sessions and exit\n"
           "  --version                 print version and exit\n"
           "  --help                    show this help\n"
           "\n"
           "Environment: GREETD_SOCK, RX_GREETER_CONFIG, RX_GREETER_THEME,\n"
           "             RX_GREETER_BACKGROUND, RX_GREETER_OUTPUT,\n"
           "             RX_GREETER_POWER, RX_GREETER_SESSION_DIRS\n",
           program);
}

int
main(int argc, char **argv)
{
    Config config;
    config_init(&config);
    const char *explicit_config = NULL;
    const char *explicit_theme = NULL;
    const char *debug_status = NULL;
    const char *cli_background = NULL;
    const char *cli_default_user = NULL;
    const char *cli_default_session = NULL;
    const char *cli_output = NULL;
    const char *cli_power_helper = NULL;
    bool cli_power_dry_run = false;
    bool print_config = false;
    bool check_config = false;
    bool list_users = false;
    bool list_sessions = false;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage(argv[0]);
            config_free(&config);
            return 0;
        } else if (strcmp(arg, "--version") == 0) {
            printf("rx-greeter %s\n", RX_GREETER_VERSION);
            config_free(&config);
            return 0;
        } else if (strcmp(arg, "--config") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "rx-greeter: --config requires a path\n");
                config_free(&config);
                return 2;
            }
            explicit_config = argv[++i];
        } else if (strcmp(arg, "--theme") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "rx-greeter: --theme requires a path\n");
                config_free(&config);
                return 2;
            }
            explicit_theme = argv[++i];
        } else if (strcmp(arg, "--background") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "rx-greeter: --background requires a path\n");
                config_free(&config);
                return 2;
            }
            cli_background = argv[++i];
        } else if (strcmp(arg, "--default-user") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "rx-greeter: --default-user requires a user\n");
                config_free(&config);
                return 2;
            }
            cli_default_user = argv[++i];
        } else if (strcmp(arg, "--default-session") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "rx-greeter: --default-session requires a name\n");
                config_free(&config);
                return 2;
            }
            cli_default_session = argv[++i];
        } else if (strcmp(arg, "--output") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "rx-greeter: --output requires a name\n");
                config_free(&config);
                return 2;
            }
            cli_output = argv[++i];
        } else if (strcmp(arg, "--power-helper") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "rx-greeter: --power-helper requires a path\n");
                config_free(&config);
                return 2;
            }
            cli_power_helper = argv[++i];
        } else if (strcmp(arg, "--power-dry-run") == 0) {
            cli_power_dry_run = true;
        } else if (strcmp(arg, "--debug-status") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "rx-greeter: --debug-status requires a path\n");
                config_free(&config);
                return 2;
            }
            debug_status = argv[++i];
        } else if (strcmp(arg, "--print-config") == 0) {
            print_config = true;
        } else if (strcmp(arg, "--check-config") == 0) {
            check_config = true;
        } else if (strcmp(arg, "--list-users") == 0) {
            list_users = true;
        } else if (strcmp(arg, "--list-sessions") == 0) {
            list_sessions = true;
        } else {
            fprintf(stderr, "rx-greeter: unknown option '%s'\n", arg);
            print_usage(argv[0]);
            config_free(&config);
            return 2;
        }
    }

    setlocale(LC_ALL, "");
    rx_log_init();

    const char *config_env = getenv("RX_GREETER_CONFIG");
    char *config_path = NULL;
    if (explicit_config) {
        if (!path_is_readable(explicit_config)) {
            rx_log(LOG_ERR, "config file is not readable: %s", explicit_config);
            config_free(&config);
            return 1;
        }
        config_path = xstrdup(explicit_config);
    } else if (config_env && *config_env) {
        if (!path_is_readable(config_env)) {
            rx_log(LOG_ERR, "config file is not readable: %s", config_env);
            config_free(&config);
            return 1;
        }
        config_path = xstrdup(config_env);
    } else {
        config_path = config_find_config_path(NULL);
    }

    Config host;
    config_init(&host);
    bool host_loaded = false;
    if (config_path) {
        if (!config_load_file(&host, config_path, false)) {
            rx_log(LOG_ERR, "failed to load host config: %s", config_path);
            free(config_path);
            config_free(&host);
            config_free(&config);
            return 1;
        }
        host_loaded = true;
    }

    const char *theme_env = getenv("RX_GREETER_THEME");
    char *theme_path = NULL;
    if (explicit_theme) {
        if (!path_is_readable(explicit_theme)) {
            rx_log(LOG_ERR, "theme file is not readable: %s", explicit_theme);
            free(config_path);
            config_free(&host);
            config_free(&config);
            return 1;
        }
        theme_path = xstrdup(explicit_theme);
    } else if (theme_env && *theme_env) {
        if (!path_is_readable(theme_env)) {
            rx_log(LOG_ERR, "theme file is not readable: %s", theme_env);
            free(config_path);
            config_free(&host);
            config_free(&config);
            return 1;
        }
        theme_path = xstrdup(theme_env);
    } else if (host.theme_path && *host.theme_path) {
        if (!path_is_readable(host.theme_path)) {
            rx_log(LOG_ERR, "configured theme_path is not readable: %s",
                   host.theme_path);
            free(config_path);
            config_free(&host);
            config_free(&config);
            return 1;
        }
        theme_path = xstrdup(host.theme_path);
    } else {
        theme_path = config_find_theme_path(NULL);
    }

    if (theme_path) {
        if (!config_load_file(&config, theme_path, true)) {
            rx_log(LOG_ERR, "failed to load theme: %s", theme_path);
            free(theme_path);
            free(config_path);
            config_free(&host);
            config_free(&config);
            return 1;
        }
    } else {
        rx_log(LOG_WARNING, "no theme file found; using built-in defaults");
    }

    if (host_loaded) {
        if (!config_load_file(&config, config_path, true)) {
            rx_log(LOG_ERR, "failed to reload host config: %s", config_path);
            free(theme_path);
            free(config_path);
            config_free(&host);
            config_free(&config);
            return 1;
        }
        config.config_path = xstrdup(config_path);
    }
    if (theme_path) {
        free(config.theme_path);
        config.theme_path = xstrdup(theme_path);
    }

    if (cli_background) {
        free(config.background_image);
        config.background_image = expand_path(cli_background);
    }
    if (cli_default_user) {
        free(config.default_user);
        config.default_user = xstrdup(cli_default_user);
    }
    if (cli_default_session) {
        free(config.default_session);
        config.default_session = xstrdup(cli_default_session);
    }
    if (cli_output) {
        free(config.output_name);
        config.output_name = xstrdup(cli_output);
    }
    if (cli_power_helper) {
        free(config.power_helper);
        config.power_helper = expand_path(cli_power_helper);
    }
    if (cli_power_dry_run) {
        config.power_dry_run = true;
    }

    free(theme_path);
    free(config_path);
    config_free(&host);

    if (print_config) {
        config_print(&config);
    }
    if (check_config) {
        int rc = check_configuration(&config);
        config_free(&config);
        return rc;
    }
    if (print_config) {
        config_free(&config);
        return 0;
    }
    if (list_users) {
        UserList users;
        users_load(&users);
        for (size_t i = 0; i < users.count; i++) {
            printf("%s\n", users.items[i].name);
        }
        users_free(&users);
        config_free(&config);
        return 0;
    }
    if (list_sessions) {
        SessionList sessions;
        sessions_load(&sessions, config.session_dirs);
        for (size_t i = 0; i < sessions.count; i++) {
            printf("%s\t%s\t%s\n", sessions.items[i].id,
                   sessions.items[i].name, sessions.items[i].exec);
        }
        sessions_free(&sessions);
        config_free(&config);
        return 0;
    }

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sa.sa_handler = handle_signal;
    sigaction(SIGHUP, &sa, NULL);

    int rc = run_greeter(&config, debug_status);
    config_free(&config);
    return rc;
}
