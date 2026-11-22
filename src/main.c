
#include "config.h"
#include "greetd.h"
#include "sessions.h"
#include "ui.h"
#include "users.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "xdg-shell-client-protocol.h"

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

#define RX_GREETER_VERSION "0.1.0"

struct Buffer {
    struct wl_buffer *wl_buffer;
    void *data;
    size_t size;
    cairo_surface_t *surface;
    bool busy;
    struct App *app;
};

struct App {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    uint32_t compositor_version;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct wl_seat *seat;
    struct wl_keyboard *keyboard;
    struct wl_pointer *pointer;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;

    struct Buffer *buffers[2];
    int width;
    int height;
    int pending_width;
    int pending_height;
    bool configured;
    bool running;
    int exit_code;
    bool needs_redraw;

    struct xkb_context *xkb_context;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;
    int repeat_key;
    int repeat_rate;
    int repeat_delay;
    int64_t repeat_next_ms;
    uint32_t pointer_enter_serial;
    double pointer_x;
    double pointer_y;

    Ui *ui;
};

static volatile sig_atomic_t signal_stop = 0;

static void
handle_signal(int signo)
{
    (void)signo;
    signal_stop = 1;
}

static void
log_error(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    fputs("rx-greeter: ", stderr);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
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

static void
buffer_release(void *data, struct wl_buffer *wl_buffer)
{
    (void)wl_buffer;
    struct Buffer *buffer = data;
    buffer->busy = false;
    buffer->app->needs_redraw = true;
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
destroy_buffers(struct App *app)
{
    for (size_t i = 0; i < 2; i++) {
        destroy_buffer(app->buffers[i]);
        app->buffers[i] = NULL;
    }
}

static struct Buffer *
create_buffer(struct App *app, int width, int height)
{
    if (!app->shm || width <= 0 || height <= 0) {
        return NULL;
    }
    size_t stride = (size_t)width * 4;
    size_t size = stride * (size_t)height;
    int fd = create_shm_file(size);
    if (fd < 0) {
        log_error("memfd_create failed: %s", strerror(errno));
        return NULL;
    }

    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        log_error("mmap failed: %s", strerror(errno));
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
    buffer->app = app;
    wl_buffer_add_listener(wl_buffer, &buffer_listener, buffer);
    return buffer;
}

static bool
create_buffers(struct App *app)
{
    destroy_buffers(app);
    app->buffers[0] = create_buffer(app, app->width, app->height);
    app->buffers[1] = create_buffer(app, app->width, app->height);
    if (!app->buffers[0] || !app->buffers[1]) {
        destroy_buffers(app);
        return false;
    }
    return true;
}

static struct Buffer *
pick_buffer(struct App *app)
{
    for (size_t i = 0; i < 2; i++) {
        if (app->buffers[i] && !app->buffers[i]->busy) {
            return app->buffers[i];
        }
    }
    return NULL;
}

static bool
render_frame(struct App *app)
{
    if (!app->configured || app->width <= 0 || app->height <= 0) {
        return false;
    }
    struct Buffer *buffer = pick_buffer(app);
    if (!buffer) {
        app->needs_redraw = true;
        return false;
    }

    cairo_t *cr = cairo_create(buffer->surface);
    if (!cr) {
        return false;
    }
    ui_render(app->ui, cr, app->width, app->height);
    cairo_destroy(cr);
    cairo_surface_flush(buffer->surface);

    buffer->busy = true;
    wl_surface_attach(app->surface, buffer->wl_buffer, 0, 0);
    if (app->compositor_version >= 4) {
        wl_surface_damage_buffer(app->surface, 0, 0, app->width, app->height);
    } else {
        wl_surface_damage(app->surface, 0, 0, app->width, app->height);
    }
    wl_surface_commit(app->surface);
    app->needs_redraw = false;
    return true;
}

static void
wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

static void
toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                   int32_t width, int32_t height, struct wl_array *states)
{
    (void)toplevel;
    (void)states;
    struct App *app = data;
    if (width > 0) {
        app->pending_width = width;
    }
    if (height > 0) {
        app->pending_height = height;
    }
}

static void
toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    (void)toplevel;
    struct App *app = data;
    app->running = false;
    app->exit_code = 0;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
};

static void
surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial)
{
    struct App *app = data;
    xdg_surface_ack_configure(xdg_surface, serial);

    int width = app->pending_width;
    int height = app->pending_height;
    if (width <= 0 || height <= 0) {
        width = app->width > 0 ? app->width : 1280;
        height = app->height > 0 ? app->height : 720;
    }
    if (width != app->width || height != app->height) {
        app->width = width;
        app->height = height;
        if (!create_buffers(app)) {
            log_error("failed to create shared memory buffers");
            app->running = false;
            app->exit_code = 1;
            return;
        }
    }
    app->configured = true;
    xdg_surface_set_window_geometry(xdg_surface, 0, 0, app->width, app->height);
    ui_resize(app->ui, app->width, app->height);
    ui_start(app->ui);
    app->needs_redraw = true;
}

static const struct xdg_surface_listener surface_listener = {
    .configure = surface_configure,
};

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
        log_error("failed to map xkb keymap: %s", strerror(errno));
        return;
    }
    struct xkb_keymap *keymap = xkb_keymap_new_from_string(
        app->xkb_context, map, XKB_KEYMAP_FORMAT_TEXT_V1,
        XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    if (!keymap) {
        log_error("failed to parse xkb keymap");
        return;
    }
    struct xkb_state *state = xkb_state_new(keymap);
    if (!state) {
        xkb_keymap_unref(keymap);
        log_error("failed to create xkb state");
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
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)surface;
    struct App *app = data;
    app->repeat_key = 0;
}

static bool
key_repeatable(xkb_keysym_t sym)
{
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter ||
        sym == XKB_KEY_Escape || sym == XKB_KEY_Tab ||
        sym == XKB_KEY_ISO_Left_Tab || sym == XKB_KEY_F10 ||
        sym == XKB_KEY_F11 || sym == XKB_KEY_F12) {
        return false;
    }
    return true;
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

    xkb_keysym_t sym = xkb_state_key_get_one_sym(app->xkb_state, keycode);
    ui_key(app->ui, sym, keycode, app->xkb_state, true, false);

    if (key_repeatable(sym)) {
        app->repeat_key = (int)keycode;
        int64_t now = now_ms();
        app->repeat_next_ms = now + app->repeat_delay;
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
    (void)surface;
    struct App *app = data;
    app->pointer_enter_serial = serial;
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
    ui_pointer_button(app->ui, app->pointer_x, app->pointer_y, button,
                      state == WL_POINTER_BUTTON_STATE_PRESSED);
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
        wl_keyboard_release(app->keyboard);
        app->keyboard = NULL;
        app->repeat_key = 0;
    }

    if (has_pointer && !app->pointer) {
        app->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(app->pointer, &pointer_listener, app);
    } else if (!has_pointer && app->pointer) {
        wl_pointer_release(app->pointer);
        app->pointer = NULL;
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
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        app->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(app->wm_base, &wm_base_listener, app);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        uint32_t bind_version = version < 5 ? version : 5;
        app->seat = wl_registry_bind(registry, name, &wl_seat_interface,
                                     bind_version);
        wl_seat_add_listener(app->seat, &seat_listener, app);
    }
}

static void
registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
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
    ui_key(app->ui, sym, keycode, app->xkb_state, true, true);
    int rate = app->repeat_rate > 0 ? app->repeat_rate : 25;
    app->repeat_next_ms = now + 1000 / rate;
}

static void
cleanup_app(struct App *app)
{
    if (!app) {
        return;
    }
    if (app->ui) {
        ui_destroy(app->ui);
        app->ui = NULL;
    }
    if (app->keyboard) {
        wl_keyboard_release(app->keyboard);
        app->keyboard = NULL;
    }
    if (app->pointer) {
        wl_pointer_release(app->pointer);
        app->pointer = NULL;
    }
    if (app->seat) {
        wl_seat_release(app->seat);
        app->seat = NULL;
    }
    if (app->xkb_state) {
        xkb_state_unref(app->xkb_state);
        app->xkb_state = NULL;
    }
    if (app->xkb_keymap) {
        xkb_keymap_unref(app->xkb_keymap);
        app->xkb_keymap = NULL;
    }
    if (app->xkb_context) {
        xkb_context_unref(app->xkb_context);
        app->xkb_context = NULL;
    }
    destroy_buffers(app);
    if (app->toplevel) {
        xdg_toplevel_destroy(app->toplevel);
    }
    if (app->xdg_surface) {
        xdg_surface_destroy(app->xdg_surface);
    }
    if (app->surface) {
        wl_surface_destroy(app->surface);
    }
    if (app->wm_base) {
        xdg_wm_base_destroy(app->wm_base);
    }
    if (app->shm) {
        wl_shm_destroy(app->shm);
    }
    if (app->compositor) {
        wl_compositor_destroy(app->compositor);
    }
    if (app->registry) {
        wl_registry_destroy(app->registry);
    }
    if (app->display) {
        wl_display_disconnect(app->display);
    }
}

static int
run_greeter(const Config *config, const char *debug_status)
{
    struct App app;
    memset(&app, 0, sizeof(app));
    app.running = true;
    app.repeat_rate = 25;
    app.repeat_delay = 500;

    app.display = wl_display_connect(NULL);
    if (!app.display) {
        log_error("cannot connect to a Wayland compositor");
        return 1;
    }
    app.registry = wl_display_get_registry(app.display);
    if (!app.registry) {
        cleanup_app(&app);
        return 1;
    }
    wl_registry_add_listener(app.registry, &registry_listener, &app);
    if (wl_display_roundtrip(app.display) < 0) {
        log_error("Wayland registry roundtrip failed");
        cleanup_app(&app);
        return 1;
    }
    if (!app.compositor || !app.shm || !app.wm_base) {
        log_error("compositor does not advertise wl_compositor, wl_shm and xdg_wm_base");
        cleanup_app(&app);
        return 1;
    }

    app.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!app.xkb_context) {
        log_error("cannot create xkb context");
        cleanup_app(&app);
        return 1;
    }

    app.ui = ui_create((Config *)config, debug_status);
    app.ui->width = 1280;
    app.ui->height = 720;

    app.surface = wl_compositor_create_surface(app.compositor);
    if (!app.surface) {
        log_error("cannot create Wayland surface");
        cleanup_app(&app);
        return 1;
    }
    app.xdg_surface = xdg_wm_base_get_xdg_surface(app.wm_base, app.surface);
    if (!app.xdg_surface) {
        log_error("cannot create xdg surface");
        cleanup_app(&app);
        return 1;
    }
    xdg_surface_add_listener(app.xdg_surface, &surface_listener, &app);
    app.toplevel = xdg_surface_get_toplevel(app.xdg_surface);
    if (!app.toplevel) {
        log_error("cannot create xdg toplevel");
        cleanup_app(&app);
        return 1;
    }
    xdg_toplevel_add_listener(app.toplevel, &toplevel_listener, &app);
    xdg_toplevel_set_title(app.toplevel, "rx-greeter");
    xdg_toplevel_set_app_id(app.toplevel, "rx-greeter");
    xdg_toplevel_set_fullscreen(app.toplevel, NULL);
    wl_surface_commit(app.surface);

    app.running = true;
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
            log_error("poll failed: %s", strerror(errno));
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
            if (render_frame(&app)) {
                ui_clear_dirty(app.ui);
            }
        }
    }

    int exit_code = app.exit_code;
    cleanup_app(&app);
    return exit_code;
}

static void
print_usage(const char *program)
{
    printf("usage: %s [options]\n"
           "\n"
           "Options:\n"
           "  --theme PATH             read this theme file\n"
           "  --background PATH        override the wallpaper path\n"
           "  --default-user USER      default username (default: ranxom)\n"
           "  --default-session NAME   preferred session (default: sway)\n"
           "  --power-helper PATH      privileged power helper path\n"
           "  --power-dry-run          never perform power actions\n"
           "  --debug-status PATH      write a JSON state file (testing)\n"
           "  --print-config           print effective theme and exit\n"
           "  --list-users             list selectable users and exit\n"
           "  --list-sessions          list Wayland sessions and exit\n"
           "  --version                print version and exit\n"
           "  --help                   show this help\n"
           "\n"
           "Environment: GREETD_SOCK, RX_GREETER_THEME, RX_GREETER_BACKGROUND,\n"
           "             RX_GREETER_POWER, RX_GREETER_SESSION_DIRS\n",
           program);
}

int
main(int argc, char **argv)
{
    Config config;
    config_init(&config);
    const char *explicit_theme = NULL;
    const char *debug_status = NULL;
    const char *cli_background = NULL;
    const char *cli_default_user = NULL;
    const char *cli_default_session = NULL;
    const char *cli_power_helper = NULL;
    bool cli_power_dry_run = false;
    bool print_config = false;
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
        } else if (strcmp(arg, "--theme") == 0 && i + 1 < argc) {
            explicit_theme = argv[++i];
        } else if (strcmp(arg, "--background") == 0 && i + 1 < argc) {
            cli_background = argv[++i];
        } else if (strcmp(arg, "--default-user") == 0 && i + 1 < argc) {
            cli_default_user = argv[++i];
        } else if (strcmp(arg, "--default-session") == 0 && i + 1 < argc) {
            cli_default_session = argv[++i];
        } else if (strcmp(arg, "--power-helper") == 0 && i + 1 < argc) {
            cli_power_helper = argv[++i];
        } else if (strcmp(arg, "--power-dry-run") == 0) {
            cli_power_dry_run = true;
        } else if (strcmp(arg, "--debug-status") == 0 && i + 1 < argc) {
            debug_status = argv[++i];
        } else if (strcmp(arg, "--print-config") == 0) {
            print_config = true;
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

    if (explicit_theme) {
        if (!path_is_readable(explicit_theme)) {
            log_error("theme file is not readable: %s", explicit_theme);
            config_free(&config);
            return 1;
        }
        if (!config_load_file(&config, explicit_theme)) {
            log_error("failed to load theme: %s", explicit_theme);
            config_free(&config);
            return 1;
        }
    } else {
        char *found = config_find_theme_path(NULL);
        if (found) {
            config_load_file(&config, found);
            free(found);
        }
    }

    /* Command-line values take precedence over the theme file. */
    if (cli_background) {
        free(config.background_image);
        config.background_image = xstrdup(cli_background);
    }
    if (cli_default_user) {
        free(config.default_user);
        config.default_user = xstrdup(cli_default_user);
    }
    if (cli_default_session) {
        free(config.default_session);
        config.default_session = xstrdup(cli_default_session);
    }
    if (cli_power_helper) {
        free(config.power_helper);
        config.power_helper = xstrdup(cli_power_helper);
    }
    if (cli_power_dry_run) {
        config.power_dry_run = true;
    }

    if (print_config) {
        config_print(&config);
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
        sessions_load(&sessions);
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
