/*
 * Small test input injector for nested wlroots sessions.
 *
 * It uses the wlroots virtual-keyboard and virtual-pointer protocols. This
 * is a test utility only; rx-greeter never links it.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

struct Action {
    enum {
        ACTION_KEY,
        ACTION_TEXT,
        ACTION_CLICK,
        ACTION_SLEEP,
        ACTION_CAPS,
    } type;
    char *text;
    int x;
    int y;
    int ms;
    bool caps_on;
};

struct InputApp {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_seat *seat;
    struct zwp_virtual_keyboard_manager_v1 *keyboard_manager;
    struct zwp_virtual_keyboard_v1 *keyboard;
    struct zwlr_virtual_pointer_manager_v1 *pointer_manager;
    struct zwlr_virtual_pointer_v1 *pointer;
    uint32_t pointer_version;
    struct xkb_context *xkb_context;
    struct xkb_keymap *keymap;
    struct xkb_state *xkb_state;
    int width;
    int height;
    struct Action *actions;
    size_t action_count;
    size_t action_capacity;
};

static void
die(const char *message)
{
    fprintf(stderr, "wl-input: %s\n", message);
    exit(1);
}

static uint32_t
now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void
add_action(struct InputApp *app, struct Action action)
{
    if (app->action_count == app->action_capacity) {
        app->action_capacity = app->action_capacity ? app->action_capacity * 2 : 8;
        app->actions = realloc(app->actions,
                               app->action_capacity * sizeof(*app->actions));
        if (!app->actions) {
            die("out of memory");
        }
    }
    app->actions[app->action_count++] = action;
}

static void
registry_global(void *data, struct wl_registry *registry, uint32_t name,
                const char *interface, uint32_t version)
{
    struct InputApp *app = data;
    if (strcmp(interface, wl_seat_interface.name) == 0 && !app->seat) {
        app->seat = wl_registry_bind(registry, name, &wl_seat_interface,
                                     version < 5 ? version : 5);
    } else if (strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
        app->keyboard_manager = wl_registry_bind(
            registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
    } else if (strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
        uint32_t bind_version = version < 2 ? version : 2;
        app->pointer_version = bind_version;
        app->pointer_manager = wl_registry_bind(
            registry, name, &zwlr_virtual_pointer_manager_v1_interface,
            bind_version);
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

static int
create_memfd(const char *name, const void *data, size_t size)
{
    int fd = memfd_create(name, MFD_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    if (write(fd, data, size) != (ssize_t)size) {
        close(fd);
        return -1;
    }
    lseek(fd, 0, SEEK_SET);
    return fd;
}

static bool
setup_keyboard(struct InputApp *app)
{
    app->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!app->xkb_context) {
        return false;
    }
    const char *layout = getenv("XKB_DEFAULT_LAYOUT");
    if (!layout || !*layout) {
        layout = "us";
    }
    struct xkb_rule_names names = {
        .rules = NULL,
        .model = "pc105",
        .layout = layout,
        .variant = NULL,
        .options = NULL,
    };
    app->keymap = xkb_keymap_new_from_names(app->xkb_context, &names,
                                            XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!app->keymap) {
        return false;
    }
    app->xkb_state = xkb_state_new(app->keymap);
    if (!app->xkb_state) {
        return false;
    }

    if (!app->keyboard_manager || !app->seat) {
        return false;
    }
    app->keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
        app->keyboard_manager, app->seat);
    if (!app->keyboard) {
        return false;
    }

    char *keymap_text = xkb_keymap_get_as_string(app->keymap,
                                                 XKB_KEYMAP_FORMAT_TEXT_V1);
    if (!keymap_text) {
        return false;
    }
    size_t size = strlen(keymap_text) + 1;
    int fd = create_memfd("wl-input-keymap", keymap_text, size);
    free(keymap_text);
    if (fd < 0) {
        return false;
    }
    zwp_virtual_keyboard_v1_keymap(app->keyboard,
                                   WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd,
                                   (uint32_t)size);
    close(fd);
    /* Flush and process setup immediately; some compositors only update seat
     * capabilities after the keymap request has been handled. */
    wl_display_roundtrip(app->display);
    return true;
}

static bool
setup_pointer(struct InputApp *app)
{
    if (!app->pointer_manager || !app->seat) {
        return false;
    }
    app->pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(
        app->pointer_manager, app->seat);
    wl_display_roundtrip(app->display);
    return app->pointer != NULL;
}

static bool
find_keycode(struct InputApp *app, xkb_keysym_t keysym, xkb_keycode_t *keycode,
             bool *needs_shift)
{
    for (xkb_keycode_t key = 8; key < 256; key++) {
        for (xkb_level_index_t level = 0; level < 2; level++) {
            const xkb_keysym_t *syms = NULL;
            int count = xkb_keymap_key_get_syms_by_level(app->keymap, key, 0,
                                                         level, &syms);
            for (int i = 0; i < count; i++) {
                if (syms[i] == keysym) {
                    *keycode = key;
                    *needs_shift = level == 1;
                    return true;
                }
            }
        }
    }
    return false;
}

static void
emit_modifiers(struct InputApp *app)
{
    xkb_mod_mask_t depressed = xkb_state_serialize_mods(
        app->xkb_state, XKB_STATE_MODS_DEPRESSED);
    xkb_mod_mask_t latched = xkb_state_serialize_mods(
        app->xkb_state, XKB_STATE_MODS_LATCHED);
    xkb_mod_mask_t locked = xkb_state_serialize_mods(
        app->xkb_state, XKB_STATE_MODS_LOCKED);
    xkb_layout_index_t group = xkb_state_serialize_layout(
        app->xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);
    zwp_virtual_keyboard_v1_modifiers(app->keyboard, depressed, latched,
                                      locked, group);
}

static void
emit_key(struct InputApp *app, xkb_keycode_t keycode, bool pressed)
{
    /* wlroots virtual keyboards pass update_state=false, so the client must
     * maintain xkb state and send the modifier masks itself. */
    xkb_state_update_key(app->xkb_state, keycode,
                         pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
    /* The protocol wants the Linux evdev code, while xkb uses evdev + 8. */
    zwp_virtual_keyboard_v1_key(app->keyboard, now_ms(),
                                (uint32_t)(keycode - 8), pressed ? 1 : 0);
    emit_modifiers(app);
}

static void
press_keysym(struct InputApp *app, xkb_keysym_t keysym)
{
    xkb_keycode_t keycode = 0;
    bool shift = false;
    if (!find_keycode(app, keysym, &keycode, &shift)) {
        fprintf(stderr, "wl-input: no keycode for keysym 0x%x\n", keysym);
        return;
    }

    xkb_keycode_t shift_code = 0;
    bool shift_shift = false;
    if (shift && !find_keycode(app, XKB_KEY_Shift_L, &shift_code, &shift_shift)) {
        fprintf(stderr, "wl-input: no Shift key\n");
        return;
    }
    if (shift) {
        emit_key(app, shift_code, true);
    }
    emit_key(app, keycode, true);
    emit_key(app, keycode, false);
    if (shift) {
        emit_key(app, shift_code, false);
    }
}

static uint32_t
decode_utf8(const char **text)
{
    const unsigned char *s = (const unsigned char *)*text;
    if (!s[0]) {
        return 0;
    }
    uint32_t cp = 0;
    size_t len = 0;
    if (s[0] < 0x80) {
        cp = s[0];
        len = 1;
    } else if ((s[0] & 0xe0) == 0xc0 && s[1]) {
        cp = ((uint32_t)(s[0] & 0x1f) << 6) | (s[1] & 0x3f);
        len = 2;
    } else if ((s[0] & 0xf0) == 0xe0 && s[1] && s[2]) {
        cp = ((uint32_t)(s[0] & 0x0f) << 12) |
             ((uint32_t)(s[1] & 0x3f) << 6) | (s[2] & 0x3f);
        len = 3;
    } else if ((s[0] & 0xf8) == 0xf0 && s[1] && s[2] && s[3]) {
        cp = ((uint32_t)(s[0] & 0x07) << 18) |
             ((uint32_t)(s[1] & 0x3f) << 12) |
             ((uint32_t)(s[2] & 0x3f) << 6) | (s[3] & 0x3f);
        len = 4;
    } else {
        cp = '?';
        len = 1;
    }
    *text += len;
    return cp;
}

static void
type_text(struct InputApp *app, const char *text)
{
    while (*text) {
        uint32_t cp = decode_utf8(&text);
        if (cp == '\n' || cp == '\r') {
            press_keysym(app, XKB_KEY_Return);
            continue;
        }
        xkb_keysym_t keysym = xkb_utf32_to_keysym(cp);
        if (keysym == XKB_KEY_NoSymbol) {
            fprintf(stderr, "wl-input: cannot type U+%04x\n", cp);
            continue;
        }
        press_keysym(app, keysym);
    }
}

static void
set_caps_lock(struct InputApp *app, bool enabled)
{
    xkb_mod_index_t caps_index =
        xkb_keymap_mod_get_index(app->keymap, XKB_MOD_NAME_CAPS);
    xkb_mod_mask_t caps =
        caps_index == XKB_MOD_INVALID ? 0 : ((xkb_mod_mask_t)1u << caps_index);
    xkb_mod_mask_t locked = enabled ? caps : 0;
    xkb_state_update_mask(app->xkb_state, 0, 0, locked, 0, 0, 0);
    emit_modifiers(app);
}

static void
click(struct InputApp *app, int x, int y)
{
    if (!app->pointer) {
        fprintf(stderr, "wl-input: no virtual pointer\n");
        return;
    }
    uint32_t time = now_ms();
    zwlr_virtual_pointer_v1_motion_absolute(
        app->pointer, time, (uint32_t)x, (uint32_t)y,
        (uint32_t)app->width, (uint32_t)app->height);
    if (app->pointer_version >= 2) {
        zwlr_virtual_pointer_v1_frame(app->pointer);
    }
    wl_display_roundtrip(app->display);
    usleep(100000);

    zwlr_virtual_pointer_v1_button(app->pointer, time, BTN_LEFT,
                                   WL_POINTER_BUTTON_STATE_PRESSED);
    zwlr_virtual_pointer_v1_button(app->pointer, time, BTN_LEFT,
                                   WL_POINTER_BUTTON_STATE_RELEASED);
    if (app->pointer_version >= 2) {
        zwlr_virtual_pointer_v1_frame(app->pointer);
    }
}

static void
add_key_action(struct InputApp *app, const char *name)
{
    add_action(app, (struct Action){ .type = ACTION_KEY, .text = strdup(name) });
}

static bool
execute_stdin_command(struct InputApp *app, char *line)
{
    char *arg = strchr(line, ' ');
    if (arg) {
        *arg++ = '\0';
        while (*arg == ' ') {
            arg++;
        }
    } else {
        arg = line + strlen(line);
    }

    if (strcmp(line, "quit") == 0) {
        return false;
    } else if (strcmp(line, "key") == 0) {
        xkb_keysym_t keysym = xkb_keysym_from_name(arg,
                                                   XKB_KEYSYM_CASE_INSENSITIVE);
        if (keysym == XKB_KEY_NoSymbol) {
            if (strcmp(arg, "Enter") == 0) {
                keysym = XKB_KEY_Return;
            } else if (strcmp(arg, "CapsLock") == 0) {
                keysym = XKB_KEY_Caps_Lock;
            } else if (strlen(arg) == 1) {
                keysym = xkb_utf32_to_keysym((unsigned char)arg[0]);
            }
        }
        if (keysym == XKB_KEY_NoSymbol) {
            fprintf(stderr, "wl-input: unknown key '%s'\n", arg);
        } else {
            press_keysym(app, keysym);
        }
    } else if (strcmp(line, "text") == 0) {
        type_text(app, arg);
    } else if (strcmp(line, "click") == 0) {
        int x = 0;
        int y = 0;
        if (sscanf(arg, "%d %d", &x, &y) != 2) {
            fprintf(stderr, "wl-input: click expects X Y\n");
        } else {
            click(app, x, y);
        }
    } else if (strcmp(line, "motion") == 0) {
        int x = 0;
        int y = 0;
        if (sscanf(arg, "%d %d", &x, &y) == 2 && app->pointer) {
            zwlr_virtual_pointer_v1_motion_absolute(
                app->pointer, now_ms(), (uint32_t)x, (uint32_t)y,
                (uint32_t)app->width, (uint32_t)app->height);
            if (app->pointer_version >= 2) {
                zwlr_virtual_pointer_v1_frame(app->pointer);
            }
        }
    } else if (strcmp(line, "sleep") == 0) {
        usleep((useconds_t)atoi(arg) * 1000);
    } else if (strcmp(line, "caps-lock") == 0) {
        if (strcmp(arg, "on") == 0) {
            set_caps_lock(app, true);
        } else if (strcmp(arg, "off") == 0) {
            set_caps_lock(app, false);
        } else {
            fprintf(stderr, "wl-input: caps-lock expects on or off\n");
        }
    } else if (*line) {
        fprintf(stderr, "wl-input: unknown command '%s'\n", line);
    }
    return true;
}

static void
usage(const char *program)
{
    fprintf(stderr,
            "usage: %s [--display NAME] [--size WxH] [--stdin] "
            "[--text TEXT] [--key KEY] [--click X Y] [--sleep MS] "
            "[--caps-lock on|off]\n",
            program);
}

int
main(int argc, char **argv)
{
    struct InputApp app;
    memset(&app, 0, sizeof(app));
    app.width = 1280;
    app.height = 720;
    const char *display_name = NULL;
    bool stdin_mode = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--display") == 0 && i + 1 < argc) {
            display_name = argv[++i];
        } else if (strcmp(argv[i], "--stdin") == 0) {
            stdin_mode = true;
        } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &app.width, &app.height) != 2 ||
                app.width <= 0 || app.height <= 0) {
                die("invalid --size");
            }
        } else if (strcmp(argv[i], "--text") == 0 && i + 1 < argc) {
            add_action(&app, (struct Action){
                .type = ACTION_TEXT,
                .text = strdup(argv[++i]),
            });
        } else if (strcmp(argv[i], "--key") == 0 && i + 1 < argc) {
            add_key_action(&app, argv[++i]);
        } else if (strcmp(argv[i], "--click") == 0 && i + 2 < argc) {
            add_action(&app, (struct Action){
                .type = ACTION_CLICK,
                .x = atoi(argv[++i]),
                .y = atoi(argv[++i]),
            });
        } else if (strcmp(argv[i], "--sleep") == 0 && i + 1 < argc) {
            add_action(&app, (struct Action){
                .type = ACTION_SLEEP,
                .ms = atoi(argv[++i]),
            });
        } else if (strcmp(argv[i], "--caps-lock") == 0 && i + 1 < argc) {
            const char *value = argv[++i];
            if (strcmp(value, "on") != 0 && strcmp(value, "off") != 0) {
                die("--caps-lock expects on or off");
            }
            add_action(&app, (struct Action){
                .type = ACTION_CAPS,
                .caps_on = strcmp(value, "on") == 0,
            });
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    app.display = wl_display_connect(display_name);
    if (!app.display) {
        die("cannot connect to Wayland display");
    }
    app.registry = wl_display_get_registry(app.display);
    wl_registry_add_listener(app.registry, &registry_listener, &app);
    if (wl_display_roundtrip(app.display) < 0) {
        die("registry roundtrip failed");
    }
    if (!app.seat) {
        die("compositor did not advertise wl_seat");
    }
    bool keyboard_ok = setup_keyboard(&app);
    if (!keyboard_ok) {
        bool needs_keys = stdin_mode;
        for (size_t i = 0; i < app.action_count; i++) {
            if (app.actions[i].type == ACTION_KEY ||
                app.actions[i].type == ACTION_TEXT) {
                needs_keys = true;
            }
        }
        if (needs_keys) {
            die("virtual keyboard is not available");
        }
    }
    if (app.pointer_manager) {
        setup_pointer(&app);
    }

    if (stdin_mode) {
        char *line = NULL;
        size_t capacity = 0;
        while (getline(&line, &capacity, stdin) >= 0) {
            size_t len = strlen(line);
            while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
                line[--len] = '\0';
            }
            if (!execute_stdin_command(&app, line)) {
                break;
            }
            wl_display_roundtrip(app.display);
            fputs("done\n", stdout);
            fflush(stdout);
        }
        free(line);
        wl_display_disconnect(app.display);
        for (size_t i = 0; i < app.action_count; i++) {
            free(app.actions[i].text);
        }
        free(app.actions);
        return 0;
    }

    for (size_t i = 0; i < app.action_count; i++) {
        struct Action *action = &app.actions[i];
        if (action->type == ACTION_KEY) {
            xkb_keysym_t keysym = xkb_keysym_from_name(action->text,
                                                       XKB_KEYSYM_CASE_INSENSITIVE);
            if (keysym == XKB_KEY_NoSymbol) {
                if (strcmp(action->text, "Enter") == 0) {
                    keysym = XKB_KEY_Return;
                } else if (strcmp(action->text, "CapsLock") == 0) {
                    keysym = XKB_KEY_Caps_Lock;
                } else if (strlen(action->text) == 1) {
                    keysym = xkb_utf32_to_keysym((unsigned char)action->text[0]);
                }
            }
            if (keysym == XKB_KEY_NoSymbol) {
                fprintf(stderr, "wl-input: unknown key '%s'\n", action->text);
            } else {
                press_keysym(&app, keysym);
            }
        } else if (action->type == ACTION_TEXT) {
            type_text(&app, action->text);
        } else if (action->type == ACTION_CLICK) {
            click(&app, action->x, action->y);
        } else if (action->type == ACTION_SLEEP) {
            wl_display_flush(app.display);
            usleep((useconds_t)action->ms * 1000);
        } else if (action->type == ACTION_CAPS) {
            set_caps_lock(&app, action->caps_on);
        }
        wl_display_flush(app.display);
    }

    if (wl_display_roundtrip(app.display) < 0) {
        die("final roundtrip failed");
    }
    wl_display_flush(app.display);
    wl_display_disconnect(app.display);
    for (size_t i = 0; i < app.action_count; i++) {
        free(app.actions[i].text);
    }
    free(app.actions);
    return 0;
}
