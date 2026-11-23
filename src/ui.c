
#include "ui.h"

#include "util.h"

#include <errno.h>
#include <fontconfig/fontconfig.h>
#include <json-c/json.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void layout(Ui *ui, int64_t now);

static const char *const power_actions[] = { "poweroff", "reboot", "suspend" };
static const char *const power_labels[] = { "poweroff", "reboot", "suspend" };

static void
mark_dirty(Ui *ui)
{
    ui->dirty = true;
}

static void
mark_debug(Ui *ui)
{
    ui->debug_dirty = true;
    ui->dirty = true;
}

static double
clamp01(double value)
{
    if (value < 0.0) {
        return 0.0;
    }
    if (value > 1.0) {
        return 1.0;
    }
    return value;
}

static double
ease_out_cubic(double t)
{
    t = clamp01(t);
    double inv = 1.0 - t;
    return 1.0 - inv * inv * inv;
}

static void
set_rgba(cairo_t *cr, Rgba color, double alpha)
{
    cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a * alpha);
}

static void
rounded_rect(cairo_t *cr, double x, double y, double w, double h, double radius)
{
    if (radius <= 0.0) {
        cairo_rectangle(cr, x, y, w, h);
        return;
    }
    double r = fmin(radius, fmin(w, h) / 2.0);
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2.0, 0.0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0.0, M_PI / 2.0);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2.0, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3.0 * M_PI / 2.0);
    cairo_close_path(cr);
}

static void
select_font(cairo_t *cr, const char *family, double size)
{
    cairo_select_font_face(cr, family && *family ? family : "sans",
                           CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, size);
}

static void
text_extents(cairo_t *cr, const char *text, cairo_text_extents_t *extents)
{
    cairo_text_extents(cr, text ? text : "", extents);
}

/* Center text on the font's em box, not on each string's ink extents. This
 * keeps labels with descenders (reboot/poweroff/suspend) on one baseline. */
static double
text_center_baseline(cairo_t *cr, double cy)
{
    cairo_font_extents_t extents;
    cairo_font_extents(cr, &extents);
    return cy + (extents.ascent - extents.descent) / 2.0;
}

static void
draw_text_center(cairo_t *cr, double cx, double cy, const char *text,
                 const char *family, double size, Rgba color, double alpha)
{
    select_font(cr, family, size);
    cairo_text_extents_t ext;
    text_extents(cr, text, &ext);
    set_rgba(cr, color, alpha);
    cairo_move_to(cr, cx - ext.width / 2.0 - ext.x_bearing,
                  text_center_baseline(cr, cy));
    cairo_show_text(cr, text ? text : "");
}

static void
draw_text_left(cairo_t *cr, double x, double cy, const char *text,
               const char *family, double size, Rgba color, double alpha)
{
    select_font(cr, family, size);
    cairo_text_extents_t ext;
    text_extents(cr, text, &ext);
    set_rgba(cr, color, alpha);
    cairo_move_to(cr, x - ext.x_bearing, cy - ext.height / 2.0 - ext.y_bearing);
    cairo_show_text(cr, text ? text : "");
}

static void
draw_text_right(cairo_t *cr, double right, double cy, const char *text,
                const char *family, double size, Rgba color, double alpha)
{
    select_font(cr, family, size);
    cairo_text_extents_t ext;
    text_extents(cr, text, &ext);
    set_rgba(cr, color, alpha);
    cairo_move_to(cr, right - ext.width - ext.x_bearing,
                  cy - ext.height / 2.0 - ext.y_bearing);
    cairo_show_text(cr, text ? text : "");
}

static double
text_width(cairo_t *cr, const char *text, const char *family, double size)
{
    select_font(cr, family, size);
    cairo_text_extents_t ext;
    text_extents(cr, text, &ext);
    return ext.width + fabs(ext.x_bearing);
}

static void
draw_text_elided(cairo_t *cr, double cx, double cy, const char *text,
                 double max_width, const char *family, double size,
                 Rgba color, double alpha)
{
    if (!text) {
        return;
    }
    select_font(cr, family, size);
    cairo_text_extents_t ext;
    text_extents(cr, text, &ext);
    if (ext.width <= max_width) {
        set_rgba(cr, color, alpha);
        cairo_move_to(cr, cx - ext.width / 2.0 - ext.x_bearing,
                      text_center_baseline(cr, cy));
        cairo_show_text(cr, text);
        return;
    }

    const char *ellipsis = "...";
    cairo_text_extents_t ell_ext;
    text_extents(cr, ellipsis, &ell_ext);
    if (ell_ext.width > max_width) {
        return;
    }

    size_t len = strlen(text);
    size_t best = 0;
    for (size_t i = 0; i < len;) {
        size_t next = utf8_next_codepoint(text + i, len - i);
        char *prefix = xstrndup(text, i + next);
        cairo_text_extents_t prefix_ext;
        text_extents(cr, prefix, &prefix_ext);
        free(prefix);
        if (prefix_ext.width + ell_ext.width <= max_width) {
            best = i + next;
        } else {
            break;
        }
        i += next;
    }
    if (best == 0) {
        return;
    }
    char *shown = xmalloc(best + 4);
    memcpy(shown, text, best);
    memcpy(shown + best, ellipsis, 4);
    text_extents(cr, shown, &ext);
    set_rgba(cr, color, alpha);
    cairo_move_to(cr, cx - ext.width / 2.0 - ext.x_bearing,
                  text_center_baseline(cr, cy));
    cairo_show_text(cr, shown);
    free(shown);
}

static bool
family_available(const char *family)
{
    if (!family || !*family) {
        return false;
    }
    if (str_ieq(family, "sans") || str_ieq(family, "serif") ||
        str_ieq(family, "monospace")) {
        return true;
    }
    FcPattern *pattern = FcNameParse((const FcChar8 *)family);
    if (!pattern) {
        return false;
    }
    FcConfigSubstitute(NULL, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result = FcResultNoMatch;
    FcPattern *match = FcFontMatch(NULL, pattern, &result);
    bool available = false;
    if (match) {
        FcChar8 *matched_family = NULL;
        if (FcPatternGetString(match, FC_FAMILY, 0, &matched_family) == FcResultMatch) {
            available = str_ieq((const char *)matched_family, family);
        }
        FcPatternDestroy(match);
    }
    FcPatternDestroy(pattern);
    return available;
}

static char *
resolve_font_family(const char *requested, const char *fallback)
{
    if (requested && family_available(requested)) {
        return xstrdup(requested);
    }
    if (fallback && family_available(fallback)) {
        if (requested && *requested) {
            rx_log(LOG_WARNING, "font '%s' is unavailable; using '%s'", requested, fallback);
        }
        return xstrdup(fallback);
    }
    if (requested && *requested) {
        rx_log(LOG_WARNING, "font '%s' is unavailable; using fontconfig default", requested);
    }
    return xstrdup("sans");
}

static bool
color_visible(Rgba color)
{
    return color.a > 0.001;
}

static void
load_background(Ui *ui)
{
    Config *config = ui->config;
    const char *env = getenv("RX_GREETER_BACKGROUND");
    const char *candidates[8];
    size_t count = 0;
    if (env && *env) {
        candidates[count++] = env;
    }
    if (config->background_image && *config->background_image) {
        candidates[count++] = config->background_image;
    }
    const char *home = getenv("HOME");
    char home_background[4096];
    if (home && *home) {
        snprintf(home_background, sizeof(home_background),
                 "%s/.cache/swaylock-bg.png", home);
        candidates[count++] = home_background;
    }
    candidates[count++] = "/var/lib/rx-greeter/background.png";
    candidates[count++] = "/etc/rx-greeter/background.png";
    candidates[count++] = "/usr/local/share/rx-greeter/background.png";
    candidates[count++] = "/usr/share/rx-greeter/background.png";

    for (size_t i = 0; i < count; i++) {
        const char *path = candidates[i];
        if (!path_is_readable(path)) {
            continue;
        }
        cairo_surface_t *surface = cairo_image_surface_create_from_png(path);
        if (!surface) {
            continue;
        }
        if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(surface);
            continue;
        }
        ui->background = surface;
        free(ui->background_path);
        ui->background_path = xstrdup(path);
        return;
    }

    const char *wanted = config->background_image && *config->background_image
                             ? config->background_image
                             : "(none)";
    rx_log(LOG_WARNING, "background image '%s' is not readable; using background color", wanted);
}

static bool
user_selector_available(const Ui *ui)
{
    return ui->config->show_user_selector && ui->users.count > 0;
}

static bool
session_selector_available(const Ui *ui)
{
    return ui->config->show_session_selector && ui->sessions.count > 0;
}

static bool
power_selector_available(const Ui *ui)
{
    return ui->config->show_power_buttons;
}

static const char *
selected_user(const Ui *ui)
{
    if (ui->users.count > 0 && ui->user_index >= 0 &&
        (size_t)ui->user_index < ui->users.count) {
        return ui->users.items[ui->user_index].name;
    }
    return ui->config->default_user ? ui->config->default_user : "";
}

static const Session *
selected_session(const Ui *ui)
{
    if (ui->sessions.count > 0 && ui->session_index >= 0 &&
        (size_t)ui->session_index < ui->sessions.count) {
        return &ui->sessions.items[ui->session_index];
    }
    return NULL;
}

const char *
ui_selected_user(const Ui *ui)
{
    return selected_user(ui);
}

const Session *
ui_selected_session(const Ui *ui)
{
    return selected_session(ui);
}

static double
animation_value(double from, double to, int64_t started, int duration, int64_t now)
{
    if (duration <= 0) {
        return to;
    }
    double t = (double)(now - started) / (double)duration;
    return from + (to - from) * ease_out_cubic(t);
}

static double
layout_user_height(const Ui *ui, int64_t now)
{
    return animation_value(ui->user_height_from, ui->user_height_to,
                           ui->user_height_started_ms, ui->config->animation_ms,
                           now);
}

static double
layout_below_height(const Ui *ui, int64_t now)
{
    return animation_value(ui->below_height_from, ui->below_height_to,
                           ui->below_height_started_ms, ui->config->animation_ms,
                           now);
}

static double
selector_start_center(const Ui *ui, bool below)
{
    double edge = below ? ui->field_rect.y + ui->field_rect.h
                        : ui->field_rect.y;
    double direction = below ? 1.0 : -1.0;
    return edge + direction *
        (ui->config->selector_preview_margin +
         ui->config->selector_preview_font_size / 2.0);
}

static double
selector_end_center(const Ui *ui, bool below)
{
    double edge = below ? ui->field_rect.y + ui->field_rect.h
                        : ui->field_rect.y;
    double direction = below ? 1.0 : -1.0;
    return edge + direction *
        (ui->config->selector_spacing +
         ui->config->selector_height / 2.0);
}

static double
selector_row_y(const Ui *ui, double height, bool below)
{
    double start = selector_start_center(ui, below);
    double end = selector_end_center(ui, below);
    double t = 0.0;
    if (ui->config->selector_height > 0.0) {
        t = clamp01(height / ui->config->selector_height);
    }
    double center = start + (end - start) * t;
    return center - height / 2.0;
}

static void
set_focus(Ui *ui, UiFocus focus)
{
    if (focus == UI_FOCUS_USER && !user_selector_available(ui)) {
        focus = UI_FOCUS_PASSWORD;
    } else if (focus == UI_FOCUS_SESSION && !session_selector_available(ui)) {
        focus = UI_FOCUS_PASSWORD;
    } else if (focus == UI_FOCUS_POWER && !power_selector_available(ui)) {
        focus = UI_FOCUS_PASSWORD;
    }
    if (ui->focus == focus) {
        return;
    }

    int64_t now = now_ms();
    ui->user_height = layout_user_height(ui, now);
    ui->below_height = layout_below_height(ui, now);
    ui->focus = focus;
    if (focus == UI_FOCUS_SESSION || focus == UI_FOCUS_POWER) {
        ui->below_focus = focus;
    }
    ui->user_height_from = ui->user_height;
    ui->user_height_to = focus == UI_FOCUS_USER ? ui->config->selector_height : 0.0;
    ui->user_height_started_ms = now;
    ui->below_height_from = ui->below_height;
    ui->below_height_to =
        (focus == UI_FOCUS_SESSION || focus == UI_FOCUS_POWER)
            ? ui->config->selector_height
            : 0.0;
    ui->below_height_started_ms = now;
    mark_debug(ui);
}

static int
next_available_focus(const Ui *ui, int direction)
{
    int order[4] = { UI_FOCUS_PASSWORD, UI_FOCUS_USER, UI_FOCUS_SESSION,
                     UI_FOCUS_POWER };
    int current = (int)ui->focus;
    for (int step = 1; step <= 4; step++) {
        int idx = (current + direction * step) % 4;
        if (idx < 0) {
            idx += 4;
        }
        int candidate = order[idx];
        if (candidate == UI_FOCUS_USER && !user_selector_available(ui)) {
            continue;
        }
        if (candidate == UI_FOCUS_SESSION && !session_selector_available(ui)) {
            continue;
        }
        if (candidate == UI_FOCUS_POWER && !power_selector_available(ui)) {
            continue;
        }
        return candidate;
    }
    return UI_FOCUS_PASSWORD;
}

static void
show_message(Ui *ui, UiMessageKind kind, const char *message)
{
    snprintf(ui->message, sizeof(ui->message), "%s", message ? message : "");
    ui->message_kind = kind;
    ui->debug_dirty = true;
    mark_dirty(ui);
}

static void
clear_error_message(Ui *ui)
{
    if (ui->message_kind == UI_MESSAGE_ERROR) {
        ui->message[0] = '\0';
        ui->message_kind = UI_MESSAGE_NONE;
        ui->debug_dirty = true;
    }
}

static void
show_status_timed(Ui *ui, UiMessageKind kind, const char *message, int ms)
{
    show_message(ui, kind, message);
    ui->power_status_until_ms = now_ms() + ms;
    snprintf(ui->power_status, sizeof(ui->power_status), "%s",
             message ? message : "");
    mark_debug(ui);
}

static void
trigger_login_error(Ui *ui, const char *message)
{
    int64_t now = now_ms();
    ui->phase = UI_PHASE_IDLE;
    ui->initial_password_ready = false;
    ui->auth_input_secret = false;
    ui->start_sent = false;
    ui->prompt[0] = '\0';
    if (ui->config->clear_password_on_error) {
        password_clear(&ui->password);
    }
    ui->error_active = true;
    ui->error_started_ms = now;
    ui->error_until_ms = now + ui->config->error_hold_ms;
    set_focus(ui, UI_FOCUS_PASSWORD);
    if (message && *message) {
        show_message(ui, UI_MESSAGE_ERROR, message);
    } else {
        show_message(ui, UI_MESSAGE_NONE, "");
    }
    mark_debug(ui);
}

static void
auth_error(Ui *ui, const char *message)
{
    rx_log(LOG_WARNING, "authentication failed: %s",
           message && *message ? message : "unknown error");
    /* greetd keeps the failed session in the configuring state until it is
     * explicitly cancelled. Closing the socket alone leaves an orphan, and the
     * next create_session fails with "a session is already being configured". */
    gd_cancel_and_close(&ui->greetd);
    trigger_login_error(ui, NULL);
}

static void
cancel_auth(Ui *ui, bool show_cancelled)
{
    if (ui->phase == UI_PHASE_STARTING || ui->phase == UI_PHASE_FINISHED) {
        return;
    }
    gd_cancel_and_close(&ui->greetd);
    ui->phase = UI_PHASE_IDLE;
    ui->initial_password_ready = false;
    ui->auth_input_secret = false;
    ui->start_sent = false;
    password_clear(&ui->password);
    ui->prompt[0] = '\0';
    ui->error_active = false;
    set_focus(ui, UI_FOCUS_PASSWORD);
    if (show_cancelled) {
        show_message(ui, UI_MESSAGE_INFO, "Login cancelled");
    }
    mark_debug(ui);
}

static void
send_auth_response(Ui *ui)
{
    char error[256];
    bool ok = gd_send_post(&ui->greetd, ui->password.data, error, sizeof(error));
    password_clear(&ui->password);
    ui->initial_password_ready = false;
    ui->auth_input_secret = false;
    if (!ok) {
        auth_error(ui, error);
        return;
    }
    ui->phase = UI_PHASE_WAITING;
    ui->auth_started_ms = now_ms();
    mark_debug(ui);
}

static void
start_session_if_ready(Ui *ui)
{
    if (ui->start_sent) {
        ui->quit = true;
        ui->exit_code = 0;
        ui->phase = UI_PHASE_FINISHED;
        gd_close(&ui->greetd);
        mark_debug(ui);
        return;
    }

    const Session *session = selected_session(ui);
    if (!session) {
        auth_error(ui, "No Wayland session selected");
        return;
    }

    size_t env_count = 0;
    char **env = session_build_env(session, &env_count);
    char error[256];
    bool ok = gd_send_start(&ui->greetd, session->argv, session->argc,
                            env, env_count, error, sizeof(error));
    session_free_env(env, env_count);
    if (!ok) {
        auth_error(ui, error);
        return;
    }
    rx_log(LOG_INFO, "starting session '%s' as '%s'", session->name,
           selected_user(ui));
    ui->start_sent = true;
    ui->phase = UI_PHASE_STARTING;
    ui->prompt[0] = '\0';
    password_clear(&ui->password);
    show_message(ui, UI_MESSAGE_INFO, "Starting session...");
    mark_debug(ui);
}

static void
begin_login(Ui *ui)
{
    if (ui->phase != UI_PHASE_IDLE) {
        return;
    }
    const Session *session = selected_session(ui);
    if (!session) {
        trigger_login_error(ui, "No Wayland session found");
        return;
    }

    const char *user = selected_user(ui);
    if (!user || !*user) {
        trigger_login_error(ui, "No user selected");
        return;
    }
    if (password_is_empty(&ui->password) && !ui->config->allow_empty_password) {
        show_message(ui, UI_MESSAGE_INFO, "Enter a password");
        mark_dirty(ui);
        return;
    }

    const char *sock = getenv("GREETD_SOCK");
    char error[256];
    if (!gd_connect(&ui->greetd, sock, error, sizeof(error))) {
        trigger_login_error(ui, error);
        return;
    }
    if (!gd_send_create(&ui->greetd, user, error, sizeof(error))) {
        gd_close(&ui->greetd);
        trigger_login_error(ui, error);
        return;
    }

    ui->initial_password_ready = !password_is_empty(&ui->password);
    ui->phase = UI_PHASE_CONNECTING;
    ui->auth_started_ms = now_ms();
    ui->error_active = false;
    ui->message[0] = '\0';
    ui->message_kind = UI_MESSAGE_NONE;
    ui->prompt[0] = '\0';
    set_focus(ui, UI_FOCUS_PASSWORD);
    mark_debug(ui);

    ui->phase = UI_PHASE_WAITING;
}

static void
handle_auth_message(Ui *ui, const GdEvent *event)
{
    snprintf(ui->last_auth_type, sizeof(ui->last_auth_type), "%s",
             event->auth_message_type);
    snprintf(ui->prompt, sizeof(ui->prompt), "%s", event->auth_message);

    if (strcmp(event->auth_message_type, "secret") == 0) {
        /* The password field already communicates what is being asked; do not
         * echo PAM's raw "Password:" prompt above it. */
        ui->prompt[0] = '\0';
        if (ui->initial_password_ready) {
            ui->phase = UI_PHASE_WAITING;
            send_auth_response(ui);
        } else {
            ui->phase = UI_PHASE_INPUT;
            ui->auth_input_secret = true;
            password_clear(&ui->password);
            ui->auth_started_ms = now_ms();
            show_message(ui, UI_MESSAGE_NONE, "");
        }
    } else if (strcmp(event->auth_message_type, "visible") == 0) {
        ui->phase = UI_PHASE_INPUT;
        ui->auth_input_secret = false;
        password_clear(&ui->password);
        ui->initial_password_ready = false;
        ui->auth_started_ms = now_ms();
        show_message(ui, UI_MESSAGE_NONE, "");
    } else if (strcmp(event->auth_message_type, "info") == 0) {
        show_message(ui, UI_MESSAGE_INFO, event->auth_message);
        ui->phase = UI_PHASE_WAITING;
        ui->auth_started_ms = now_ms();
        char error[256];
        if (!gd_send_post(&ui->greetd, NULL, error, sizeof(error))) {
            auth_error(ui, error);
        }
    } else if (strcmp(event->auth_message_type, "error") == 0) {
        rx_log(LOG_WARNING, "authentication message: %s",
               event->auth_message[0] ? event->auth_message : "unknown error");
        ui->prompt[0] = '\0';
        show_message(ui, UI_MESSAGE_NONE, "");
        ui->phase = UI_PHASE_WAITING;
        ui->auth_started_ms = now_ms();
        char error[256];
        if (!gd_send_post(&ui->greetd, NULL, error, sizeof(error))) {
            auth_error(ui, error);
        }
    } else {
        auth_error(ui, "Unknown authentication message");
    }
    mark_debug(ui);
}

static void
handle_greetd_event(Ui *ui, const GdEvent *event)
{
    switch (event->type) {
    case GD_EVENT_NONE:
        break;
    case GD_EVENT_SUCCESS:
        start_session_if_ready(ui);
        break;
    case GD_EVENT_AUTH_MESSAGE:
        handle_auth_message(ui, event);
        break;
    case GD_EVENT_ERROR:
        if (strcmp(event->error_type, "auth_error") == 0) {
            auth_error(ui, event->description);
        } else {
            char message[1200];
            if (event->description[0]) {
                snprintf(message, sizeof(message), "greetd: %s", event->description);
            } else {
                snprintf(message, sizeof(message), "greetd error");
            }
            auth_error(ui, message);
        }
        break;
    }
}

void
ui_greetd_readable(Ui *ui)
{
    if (ui->greetd.fd < 0) {
        return;
    }
    for (;;) {
        GdEvent event;
        int rc = gd_read(&ui->greetd, &event);
        if (rc == 0) {
            return;
        }
        if (rc < 0) {
            handle_greetd_event(ui, &event);
            return;
        }
        handle_greetd_event(ui, &event);
        if (ui->greetd.fd < 0 || ui->quit) {
            return;
        }
    }
}

static void
cycle_user(Ui *ui, int direction)
{
    if (!user_selector_available(ui)) {
        return;
    }
    int count = (int)ui->users.count;
    if (ui->user_index < 0) {
        ui->user_index = direction > 0 ? 0 : count - 1;
    } else {
        ui->user_index = (ui->user_index + direction + count) % count;
    }
    mark_debug(ui);
}

static void
cycle_session(Ui *ui, int direction)
{
    if (!session_selector_available(ui)) {
        return;
    }
    int count = (int)ui->sessions.count;
    ui->session_index = (ui->session_index + direction + count) % count;
    mark_debug(ui);
}

static void
cycle_power(Ui *ui, int direction)
{
    ui->power_index = (ui->power_index + direction + 3) % 3;
    mark_debug(ui);
}

static char *
resolve_power_helper(Ui *ui)
{
    const char *env_helper = getenv("RX_GREETER_POWER");
    if (env_helper && *env_helper) {
        return path_is_executable(env_helper) ? xstrdup(env_helper) : NULL;
    }
    if (ui->config->power_helper && *ui->config->power_helper) {
        return path_is_executable(ui->config->power_helper)
                   ? xstrdup(ui->config->power_helper)
                   : NULL;
    }
    char *sibling = find_sibling_executable(NULL, "rx-greeter-power");
    if (sibling) {
        return sibling;
    }
    static const char *const candidates[] = {
        "/usr/local/libexec/rx-greeter-power",
        "/usr/libexec/rx-greeter-power",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (path_is_executable(candidates[i])) {
            return xstrdup(candidates[i]);
        }
    }
    return NULL;
}

static void
spawn_power_action(Ui *ui, int action_index)
{
    if (ui->power_pid > 0) {
        return;
    }
    if (action_index < 0 || action_index >= 3) {
        return;
    }
    const char *action = power_actions[action_index];
    char *helper = resolve_power_helper(ui);
    if (!helper) {
        show_status_timed(ui, UI_MESSAGE_ERROR, "Power helper not found", 4000);
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        show_status_timed(ui, UI_MESSAGE_ERROR, "Cannot start power helper", 4000);
        free(helper);
        return;
    }
    if (pid == 0) {
        signal(SIGPIPE, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        if (ui->config->power_dry_run) {
            setenv("RX_GREETER_POWER_DRY_RUN", "1", 1);
        }
        execl(helper, helper, action, (char *)NULL);
        _exit(127);
    }

    ui->power_pid = pid;
    snprintf(ui->power_action, sizeof(ui->power_action), "%s", action);
    if (ui->config->power_dry_run) {
        char status[128];
        snprintf(status, sizeof(status), "dry-run: would %s", action);
        show_status_timed(ui, UI_MESSAGE_INFO, status, 4000);
    } else {
        char status[128];
        snprintf(status, sizeof(status), "Requesting %s...", action);
        show_status_timed(ui, UI_MESSAGE_INFO, status, 4000);
    }
    free(helper);
    mark_debug(ui);
}

static void
handle_key_idle(Ui *ui, xkb_keysym_t sym, const char *text, bool ctrl,
                bool shift)
{
    switch (sym) {
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        if (ui->focus == UI_FOCUS_PASSWORD) {
            begin_login(ui);
        } else if (ui->focus == UI_FOCUS_POWER) {
            spawn_power_action(ui, ui->power_index);
        } else {
            set_focus(ui, UI_FOCUS_PASSWORD);
        }
        return;
    case XKB_KEY_Escape:
        if (ui->focus != UI_FOCUS_PASSWORD) {
            set_focus(ui, UI_FOCUS_PASSWORD);
        } else if (!password_is_empty(&ui->password)) {
            password_clear(&ui->password);
            clear_error_message(ui);
            mark_debug(ui);
        }
        return;
    case XKB_KEY_Tab:
    case XKB_KEY_ISO_Left_Tab:
        set_focus(ui, (UiFocus)next_available_focus(ui, shift ? -1 : 1));
        return;
    case XKB_KEY_Up:
        if (ui->focus == UI_FOCUS_PASSWORD) {
            set_focus(ui, UI_FOCUS_USER);
        } else if (ui->focus == UI_FOCUS_SESSION) {
            set_focus(ui, UI_FOCUS_PASSWORD);
        } else if (ui->focus == UI_FOCUS_POWER) {
            set_focus(ui, UI_FOCUS_SESSION);
        }
        return;
    case XKB_KEY_Down:
        if (ui->focus == UI_FOCUS_PASSWORD) {
            set_focus(ui, UI_FOCUS_SESSION);
        } else if (ui->focus == UI_FOCUS_USER) {
            set_focus(ui, UI_FOCUS_PASSWORD);
        } else if (ui->focus == UI_FOCUS_SESSION) {
            set_focus(ui, UI_FOCUS_POWER);
        }
        return;
    case XKB_KEY_Left:
        if (ui->focus == UI_FOCUS_USER) {
            cycle_user(ui, -1);
        } else if (ui->focus == UI_FOCUS_SESSION) {
            cycle_session(ui, -1);
        } else if (ui->focus == UI_FOCUS_POWER) {
            cycle_power(ui, -1);
        }
        return;
    case XKB_KEY_Right:
        if (ui->focus == UI_FOCUS_USER) {
            cycle_user(ui, 1);
        } else if (ui->focus == UI_FOCUS_SESSION) {
            cycle_session(ui, 1);
        } else if (ui->focus == UI_FOCUS_POWER) {
            cycle_power(ui, 1);
        }
        return;
    case XKB_KEY_BackSpace:
        if (ui->focus == UI_FOCUS_PASSWORD) {
            password_backspace(&ui->password);
            clear_error_message(ui);
            mark_debug(ui);
        }
        return;
    default:
        break;
    }

    if (ctrl && (sym == XKB_KEY_u || sym == XKB_KEY_U)) {
        password_clear(&ui->password);
        set_focus(ui, UI_FOCUS_PASSWORD);
        clear_error_message(ui);
        mark_debug(ui);
        return;
    }

    if (!text || !*text) {
        return;
    }
    unsigned char first = (unsigned char)text[0];
    if (first < 0x20 || first == 0x7f) {
        return;
    }
    if (ui->focus != UI_FOCUS_PASSWORD) {
        set_focus(ui, UI_FOCUS_PASSWORD);
    }
    if (!password_append(&ui->password, text, strlen(text))) {
        show_message(ui, UI_MESSAGE_INFO, "Password is too long");
        return;
    }
    ui->error_active = false;
    clear_error_message(ui);
    mark_debug(ui);
}

static void
handle_key_input(Ui *ui, xkb_keysym_t sym, const char *text, bool ctrl)
{
    switch (sym) {
    case XKB_KEY_Escape:
        cancel_auth(ui, true);
        return;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        if (ui->auth_input_secret && password_is_empty(&ui->password) &&
            !ui->config->allow_empty_password) {
            show_message(ui, UI_MESSAGE_INFO, "Enter a password");
            return;
        }
        send_auth_response(ui);
        return;
    case XKB_KEY_BackSpace:
        password_backspace(&ui->password);
        clear_error_message(ui);
        mark_debug(ui);
        return;
    default:
        break;
    }
    if (ctrl && (sym == XKB_KEY_u || sym == XKB_KEY_U)) {
        password_clear(&ui->password);
        clear_error_message(ui);
        mark_debug(ui);
        return;
    }
    if (!text || !*text) {
        return;
    }
    unsigned char first = (unsigned char)text[0];
    if (first < 0x20 || first == 0x7f) {
        return;
    }
    if (!password_append(&ui->password, text, strlen(text))) {
        show_message(ui, UI_MESSAGE_INFO, "Password is too long");
        return;
    }
    clear_error_message(ui);
    mark_debug(ui);
}

void
ui_set_output_info(Ui *ui, int count, const char *primary,
                   int scale_120, int width, int height)
{
    ui->output_count = count;
    snprintf(ui->primary_output, sizeof(ui->primary_output), "%s",
             primary ? primary : "");
    ui->output_scale_120 = scale_120;
    ui->output_width = width;
    ui->output_height = height;
    ui->debug_dirty = true;
}

void
ui_set_caps_lock(Ui *ui, bool caps_lock)
{
    if (ui->caps_lock != caps_lock) {
        ui->caps_lock = caps_lock;
        mark_debug(ui);
    }
}

void
ui_key(Ui *ui, xkb_keysym_t sym, const char *text, bool ctrl, bool shift)
{
    if (ui->quit || ui->phase == UI_PHASE_FINISHED) {
        return;
    }

    if (sym == XKB_KEY_F10) {
        spawn_power_action(ui, 2);
        return;
    }
    if (sym == XKB_KEY_F11) {
        spawn_power_action(ui, 0);
        return;
    }
    if (sym == XKB_KEY_F12) {
        spawn_power_action(ui, 1);
        return;
    }

    if (ui->phase == UI_PHASE_STARTING) {
        return;
    }
    if (ui->phase == UI_PHASE_CONNECTING || ui->phase == UI_PHASE_WAITING) {
        if (sym == XKB_KEY_Escape) {
            cancel_auth(ui, true);
        }
        return;
    }
    if (ui->phase == UI_PHASE_INPUT) {
        handle_key_input(ui, sym, text, ctrl);
        mark_dirty(ui);
        return;
    }

    handle_key_idle(ui, sym, text, ctrl, shift);
    mark_dirty(ui);
}

static bool
point_in_rect(double x, double y, UiRect rect)
{
    return rect.w > 0.5 && rect.h > 0.5 && x >= rect.x && x <= rect.x + rect.w &&
           y >= rect.y && y <= rect.y + rect.h;
}

void
ui_pointer_enter(Ui *ui, double x, double y)
{
    ui->pointer_inside = true;
    ui->pointer_x = x;
    ui->pointer_y = y;
    ui->debug_dirty = true;
}

void
ui_pointer_motion(Ui *ui, double x, double y)
{
    ui->pointer_inside = true;
    ui->pointer_x = x;
    ui->pointer_y = y;
    ui->debug_dirty = true;
}

void
ui_pointer_leave(Ui *ui)
{
    ui->pointer_inside = false;
    ui->debug_dirty = true;
}

void
ui_pointer_button(Ui *ui, double x, double y, int width, int height,
                  uint32_t button, bool pressed)
{
    if (!pressed || button != BTN_LEFT || ui->quit) {
        return;
    }
    if (width > 0 && height > 0) {
        ui->width = width;
        ui->height = height;
    }
    layout(ui, now_ms());
    ui->pointer_button_count++;
    ui->pointer_x = x;
    ui->pointer_y = y;
    mark_debug(ui);

    if (ui->phase != UI_PHASE_IDLE) {
        if (point_in_rect(x, y, ui->field_rect)) {
            mark_debug(ui);
        }
        return;
    }

    if (ui->focus == UI_FOCUS_USER &&
        point_in_rect(x, y, ui->user_row_rect)) {
        if (point_in_rect(x, y, ui->user_left_rect)) {
            cycle_user(ui, -1);
        } else if (point_in_rect(x, y, ui->user_right_rect)) {
            cycle_user(ui, 1);
        } else {
            set_focus(ui, UI_FOCUS_PASSWORD);
        }
        return;
    }

    if ((ui->focus == UI_FOCUS_SESSION || ui->focus == UI_FOCUS_POWER) &&
        point_in_rect(x, y, ui->below_row_rect)) {
        if (point_in_rect(x, y, ui->below_left_rect)) {
            if (ui->focus == UI_FOCUS_SESSION) {
                cycle_session(ui, -1);
            } else {
                cycle_power(ui, -1);
            }
        } else if (point_in_rect(x, y, ui->below_right_rect)) {
            if (ui->focus == UI_FOCUS_SESSION) {
                cycle_session(ui, 1);
            } else {
                cycle_power(ui, 1);
            }
        } else if (ui->focus == UI_FOCUS_POWER) {
            spawn_power_action(ui, ui->power_index);
        } else {
            set_focus(ui, UI_FOCUS_PASSWORD);
        }
        return;
    }

    if (point_in_rect(x, y, ui->field_rect)) {
        set_focus(ui, UI_FOCUS_PASSWORD);
        return;
    }
    if (point_in_rect(x, y, ui->user_preview_rect)) {
        set_focus(ui, UI_FOCUS_USER);
        return;
    }
    if (point_in_rect(x, y, ui->session_preview_rect)) {
        set_focus(ui, UI_FOCUS_SESSION);
        return;
    }

    if (ui->focus != UI_FOCUS_PASSWORD) {
        set_focus(ui, UI_FOCUS_PASSWORD);
    }
}

static double
ui_alpha(const Ui *ui, int64_t now)
{
    double fade = 1.0;
    if (ui->config->fade_ms > 0) {
        double t = (double)(now - ui->fade_started_ms) /
                   (double)ui->config->fade_ms;
        fade = ease_out_cubic(t);
    }
    return fade * ui->config->element_opacity;
}

static double
shake_offset(const Ui *ui, int64_t now)
{
    if (!ui->error_active || !ui->config->login_error_shake) {
        return 0.0;
    }
    int64_t elapsed = now - ui->error_started_ms;
    if (elapsed < 0 || elapsed >= 250) {
        return 0.0;
    }
    static const double points[] = { 0.0, -10.0, 10.0, -10.0, 10.0, 0.0 };
    int segment = (int)(elapsed / 50);
    if (segment >= 5) {
        return 0.0;
    }
    double t = (double)(elapsed % 50) / 50.0;
    return points[segment] + (points[segment + 1] - points[segment]) * t;
}

static void
layout(Ui *ui, int64_t now)
{
    Config *config = ui->config;
    if (ui->width <= 0 || ui->height <= 0) {
        return;
    }
    const double margin = 20.0;
    double field_w = fmin(config->box_width,
                          fmax(1.0, (double)ui->width - 2.0 * margin));
    double field_h = fmin(config->box_height,
                          fmax(1.0, (double)ui->height - 2.0 * margin));
    double cx = ui->width / 2.0 + config->box_x;
    double cy = ui->height / 2.0 + config->box_y;
    double min_cx = margin + field_w / 2.0;
    double max_cx = ui->width - margin - field_w / 2.0;
    double min_cy = margin + field_h / 2.0;
    double max_cy = ui->height - margin - field_h / 2.0;
    if (max_cx < min_cx) {
        max_cx = min_cx;
    }
    if (max_cy < min_cy) {
        max_cy = min_cy;
    }
    cx = fmin(fmax(cx, min_cx), max_cx);
    cy = fmin(fmax(cy, min_cy), max_cy);
    ui->field_rect = (UiRect){
        cx - field_w / 2.0,
        cy - field_h / 2.0,
        field_w,
        field_h,
    };

    ui->user_height = layout_user_height(ui, now);
    ui->below_height = layout_below_height(ui, now);

    ui->user_row_rect = (UiRect){
        ui->field_rect.x,
        selector_row_y(ui, ui->user_height, false),
        ui->field_rect.w,
        ui->user_height,
    };
    ui->below_row_rect = (UiRect){
        ui->field_rect.x,
        selector_row_y(ui, ui->below_height, true),
        ui->field_rect.w,
        ui->below_height,
    };

    double arrow = config->selector_arrow_width;
    ui->user_left_rect = (UiRect){ ui->user_row_rect.x, ui->user_row_rect.y, arrow,
                                   ui->user_row_rect.h };
    ui->user_right_rect = (UiRect){ ui->user_row_rect.x + ui->user_row_rect.w - arrow,
                                    ui->user_row_rect.y, arrow, ui->user_row_rect.h };
    ui->user_center_rect = (UiRect){ ui->user_row_rect.x + arrow, ui->user_row_rect.y,
                                     ui->user_row_rect.w - 2 * arrow,
                                     ui->user_row_rect.h };
    ui->below_left_rect = (UiRect){ ui->below_row_rect.x, ui->below_row_rect.y, arrow,
                                    ui->below_row_rect.h };
    ui->below_right_rect = (UiRect){ ui->below_row_rect.x + ui->below_row_rect.w - arrow,
                                     ui->below_row_rect.y, arrow,
                                     ui->below_row_rect.h };
    ui->below_center_rect = (UiRect){ ui->below_row_rect.x + arrow,
                                      ui->below_row_rect.y,
                                      ui->below_row_rect.w - 2 * arrow,
                                      ui->below_row_rect.h };

    ui->user_preview_rect = (UiRect){ 0, 0, 0, 0 };
    ui->session_preview_rect = (UiRect){ 0, 0, 0, 0 };
}

static void
draw_background(Ui *ui, cairo_t *cr)
{
    cairo_set_source_rgba(cr, ui->config->background_color.r,
                          ui->config->background_color.g,
                          ui->config->background_color.b, 1.0);
    cairo_paint(cr);

    if (ui->background) {
        int iw = cairo_image_surface_get_width(ui->background);
        int ih = cairo_image_surface_get_height(ui->background);
        if (iw > 0 && ih > 0) {
            double scale = fmax((double)ui->width / iw, (double)ui->height / ih);
            cairo_save(cr);
            cairo_translate(cr, (ui->width - iw * scale) / 2.0,
                            (ui->height - ih * scale) / 2.0);
            cairo_scale(cr, scale, scale);
            cairo_set_source_surface(cr, ui->background, 0, 0);
            cairo_pattern_t *pattern = cairo_get_source(cr);
            if (pattern) {
                cairo_pattern_set_filter(pattern, CAIRO_FILTER_BILINEAR);
            }
            cairo_paint(cr);
            cairo_restore(cr);
        }
    }

    cairo_set_source_rgba(cr, 0, 0, 0, ui->config->background_overlay);
    cairo_paint(cr);
}

static void
draw_clock(Ui *ui, cairo_t *cr, double alpha)
{
    if (!ui->config->show_clock) {
        return;
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char time_text[256] = { 0 };
    char date_text[256] = { 0 };
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
    strftime(time_text, sizeof(time_text), ui->config->clock_format, &tm);
    if (ui->config->show_date) {
        strftime(date_text, sizeof(date_text), ui->config->date_format, &tm);
    }
#pragma GCC diagnostic pop

    const char *family = ui->resolved_clock_font;
    double cx = ui->width / 2.0 + ui->config->clock_x;
    double cy = ui->height / 2.0 + ui->config->clock_y;

    if (ui->config->show_date && date_text[0]) {
        double time_size = ui->config->clock_font_size;
        double date_size = ui->config->date_font_size;
        double total = time_size + 8.0 + date_size;
        double top = cy - total / 2.0;
        draw_text_elided(cr, cx, top + time_size / 2.0, time_text,
                         (double)ui->width - 40.0, family, time_size,
                         ui->config->clock_color, alpha);
        Rgba date_color = ui->config->clock_color;
        date_color.a *= 0.7;
        draw_text_elided(cr, cx, top + time_size + 8.0 + date_size / 2.0,
                         date_text, (double)ui->width - 40.0, family,
                         date_size, date_color, alpha);
    } else {
        draw_text_elided(cr, cx, cy, time_text, (double)ui->width - 40.0,
                         family, ui->config->clock_font_size,
                         ui->config->clock_color, alpha);
    }
}

static void
draw_help_tips(Ui *ui, cairo_t *cr, double alpha)
{
    if (!ui->config->show_help_tips) {
        return;
    }
    const char *lines[] = { "F10 - suspend", "F11 - poweroff", "F12 - reboot" };
    double size = ui->config->help_tips_font_size;
    double line_height = size * 1.35;
    double y = 20.0 + size / 2.0;
    for (size_t i = 0; i < 3; i++) {
        draw_text_left(cr, 20.0, y, lines[i], ui->resolved_font, size,
                       ui->config->help_tips_color, alpha * 0.9);
        y += line_height;
    }
}

static void
draw_caps_lock(Ui *ui, cairo_t *cr, double alpha)
{
    if (!ui->config->show_caps_lock_indicator || !ui->caps_lock) {
        return;
    }
    double size = ui->config->caps_lock_font_size;
    double width = text_width(cr, "CAPS LOCK", ui->resolved_font, size);
    double cy = ui->field_rect.y + ui->field_rect.h / 2.0;
    if (ui->field_rect.x - 15.0 - width >= 10.0) {
        draw_text_right(cr, ui->field_rect.x - 15.0, cy, "CAPS LOCK",
                        ui->resolved_font, size,
                        ui->config->caps_lock_color, alpha);
    } else if (ui->field_rect.x + ui->field_rect.w + 15.0 + width <=
               ui->width - 10.0) {
        draw_text_left(cr, ui->field_rect.x + ui->field_rect.w + 15.0, cy,
                       "CAPS LOCK", ui->resolved_font, size,
                       ui->config->caps_lock_color, alpha);
    } else {
        draw_text_center(cr, ui->field_rect.x + ui->field_rect.w / 2.0,
                         ui->field_rect.y - 14.0, "CAPS LOCK",
                         ui->resolved_font, size,
                         ui->config->caps_lock_color, alpha);
    }
}

static void
draw_dots(Ui *ui, cairo_t *cr, double alpha, double cx, double cy)
{
    size_t count = password_codepoints(&ui->password);
    if (count == 0) {
        return;
    }
    if (count > 128) {
        count = 128;
    }
    double spacing = ui->config->dot_spacing;
    if (count > 1) {
        double fit = (ui->field_rect.w - 12.0) / (double)(count - 1);
        if (fit < spacing) {
            spacing = fmax(2.0, fit);
        }
    }
    double radius = fmin(ui->config->dot_radius, spacing * 0.27);
    radius = fmax(1.3, radius);
    double x0 = cx - (double)(count - 1) * spacing / 2.0;
    set_rgba(cr, ui->config->text_color, alpha);
    for (size_t i = 0; i < count; i++) {
        cairo_arc(cr, x0 + (double)i * spacing, cy, radius, 0.0, 2.0 * M_PI);
        cairo_fill(cr);
    }
}

static void
draw_password_text(Ui *ui, cairo_t *cr, double alpha)
{
    if (password_is_empty(&ui->password)) {
        return;
    }
    double margin = 20.0;
    double left = ui->field_rect.x + margin;
    double right = ui->field_rect.x + ui->field_rect.w - margin;
    double width = right - left;
    if (width <= 4.0) {
        return;
    }

    select_font(cr, ui->resolved_font, ui->config->font_size);
    cairo_text_extents_t ext;
    text_extents(cr, ui->password.data, &ext);
    double cy = ui->field_rect.y + ui->field_rect.h / 2.0;
    double x = ui->field_rect.x + ui->field_rect.w / 2.0 - ext.width / 2.0 -
               ext.x_bearing;
    if (ext.width > width) {
        x = right - ext.width - ext.x_bearing;
    }

    cairo_save(cr);
    cairo_rectangle(cr, left, ui->field_rect.y + 3.0, width,
                    ui->field_rect.h - 6.0);
    cairo_clip(cr);
    set_rgba(cr, ui->config->text_color, alpha);
    cairo_move_to(cr, x, cy - ext.height / 2.0 - ext.y_bearing);
    cairo_show_text(cr, ui->password.data);
    cairo_restore(cr);
}

static void
draw_field(Ui *ui, cairo_t *cr, double alpha, int64_t now)
{
    double shake = shake_offset(ui, now);
    cairo_save(cr);
    cairo_translate(cr, shake, 0.0);

    Rgba border;
    double border_width;
    if (ui->error_active) {
        border = ui->config->error_color;
        border_width = ui->config->login_error_border_width;
    } else if (ui->phase != UI_PHASE_IDLE || ui->focus == UI_FOCUS_PASSWORD) {
        border = ui->config->primary_color;
        border_width = 2.0;
    } else {
        border = ui->config->outline_color;
        border_width = 1.0;
        border.a *= 0.45;
    }

    set_rgba(cr, ui->config->surface_color,
             ui->config->surface_opacity * alpha);
    rounded_rect(cr, ui->field_rect.x, ui->field_rect.y, ui->field_rect.w,
                 ui->field_rect.h, ui->config->border_radius);
    cairo_fill(cr);

    if (border_width > 0.01 && color_visible(border)) {
        double inset = border_width / 2.0;
        cairo_set_line_width(cr, border_width);
        set_rgba(cr, border, alpha);
        rounded_rect(cr, ui->field_rect.x + inset, ui->field_rect.y + inset,
                     ui->field_rect.w - border_width,
                     ui->field_rect.h - border_width,
                     fmax(0.0, ui->config->border_radius - inset));
        cairo_stroke(cr);
    }

    if (ui->phase == UI_PHASE_INPUT && !ui->auth_input_secret) {
        draw_password_text(ui, cr, alpha);
    } else {
        draw_dots(ui, cr, alpha,
                  ui->field_rect.x + ui->field_rect.w / 2.0,
                  ui->field_rect.y + ui->field_rect.h / 2.0);
    }
    cairo_restore(cr);
}

static void
draw_prompt_and_status(Ui *ui, cairo_t *cr, double alpha)
{
    if (ui->prompt[0] &&
        (ui->phase == UI_PHASE_CONNECTING || ui->phase == UI_PHASE_WAITING ||
         ui->phase == UI_PHASE_INPUT)) {
        draw_text_elided(cr, ui->field_rect.x + ui->field_rect.w / 2.0,
                         ui->field_rect.y - 14.0, ui->prompt,
                         (double)ui->width - 40.0, ui->resolved_font,
                         ui->config->font_size * 0.85, ui->config->text_color,
                         alpha * 0.75);
    }

    if (ui->message[0]) {
        Rgba color = ui->message_kind == UI_MESSAGE_ERROR
                         ? ui->config->error_color
                         : ui->config->outline_color;
        if (ui->message_kind == UI_MESSAGE_INFO) {
            color = ui->config->text_color;
            color.a *= 0.7;
        }
        double y = ui->field_rect.y + ui->field_rect.h +
                   ui->config->element_spacing + ui->below_height + 10.0;
        if (y > ui->height - 12.0) {
            y = ui->field_rect.y - 14.0;
        }
        draw_text_elided(cr, ui->field_rect.x + ui->field_rect.w / 2.0, y,
                         ui->message, (double)ui->width - 40.0,
                         ui->resolved_font,
                         ui->config->selector_preview_font_size, color, alpha);
    }
}

static void
draw_selector_row(Ui *ui, cairo_t *cr, double alpha, UiRect row,
                  const char *left_text, const char *center_text,
                  const char *right_text)
{
    if (row.h <= 0.5) {
        return;
    }
    /* Do not clip vertically to the animated row height: that slices the text
     * into a thin line near the end of the collapse and reads as a flicker.
     * The row has no background, and the center text is already elided. */
    double cy = row.y + row.h / 2.0;
    double arrow = ui->config->selector_arrow_width;
    double arrow_alpha =
        alpha * clamp01(row.h / fmax(1.0, ui->config->selector_arrow_font_size));
    draw_text_center(cr, row.x + arrow / 2.0, cy, left_text, ui->resolved_font,
                     ui->config->selector_arrow_font_size,
                     ui->config->text_color, arrow_alpha);
    draw_text_center(cr, row.x + row.w - arrow / 2.0, cy, right_text,
                     ui->resolved_font,
                     ui->config->selector_arrow_font_size,
                     ui->config->text_color, arrow_alpha);
    draw_text_elided(cr, row.x + row.w / 2.0, cy, center_text,
                     row.w - 2.0 * arrow - 20.0, ui->resolved_font,
                     ui->config->selector_font_size, ui->config->text_color,
                     alpha);
}

static bool
previews_enabled(const Ui *ui)
{
    return ui->config->show_selector_preview &&
           (ui->phase == UI_PHASE_IDLE || ui->phase == UI_PHASE_CONNECTING ||
            ui->phase == UI_PHASE_WAITING || ui->phase == UI_PHASE_INPUT) &&
           !ui->message[0];
}

static void
draw_previews(Ui *ui, cairo_t *cr, double alpha, int64_t now)
{
    ui->user_preview_rect = (UiRect){ 0 };
    ui->session_preview_rect = (UiRect){ 0 };
    if (!previews_enabled(ui)) {
        return;
    }
    (void)now;

    const char *user = selected_user(ui);
    if (user && *user && ui->focus != UI_FOCUS_USER &&
        ui->user_height <= 0.5 && !ui->prompt[0] &&
        user_selector_available(ui)) {
        double width = text_width(cr, user, ui->resolved_font,
                                  ui->config->selector_preview_font_size);
        double cy = ui->field_rect.y - ui->config->selector_preview_margin -
                    ui->config->selector_preview_font_size / 2.0;
        draw_text_center(cr, ui->field_rect.x + ui->field_rect.w / 2.0, cy, user,
                         ui->resolved_font,
                         ui->config->selector_preview_font_size,
                         ui->config->selector_preview_color, alpha);
        ui->user_preview_rect = (UiRect){
            ui->field_rect.x + ui->field_rect.w / 2.0 - width / 2.0 - 4.0,
            cy - ui->config->selector_preview_font_size,
            width + 8.0,
            ui->config->selector_preview_font_size * 2.0,
        };
    }

    const Session *session = selected_session(ui);
    if (session && ui->focus != UI_FOCUS_SESSION &&
        ui->focus != UI_FOCUS_POWER && ui->below_height <= 0.5 &&
        session_selector_available(ui)) {
        double width = text_width(cr, session->name, ui->resolved_font,
                                  ui->config->selector_preview_font_size);
        double cy = ui->field_rect.y + ui->field_rect.h +
                    ui->config->selector_preview_margin +
                    ui->config->selector_preview_font_size / 2.0;
        draw_text_center(cr, ui->field_rect.x + ui->field_rect.w / 2.0, cy,
                         session->name, ui->resolved_font,
                         ui->config->selector_preview_font_size,
                         ui->config->selector_preview_color, alpha);
        ui->session_preview_rect = (UiRect){
            ui->field_rect.x + ui->field_rect.w / 2.0 - width / 2.0 - 4.0,
            cy - ui->config->selector_preview_font_size,
            width + 8.0,
            ui->config->selector_preview_font_size * 2.0,
        };
    }
}

void
ui_render(Ui *ui, cairo_t *cr, int width, int height)
{
    ui->width = width;
    ui->height = height;
    int64_t now = now_ms();
    layout(ui, now);
    double alpha = ui_alpha(ui, now);
    ui->user_row_drawn = false;
    ui->below_row_drawn = false;

    draw_background(ui, cr);
    draw_clock(ui, cr, alpha);
    draw_help_tips(ui, cr, alpha);
    draw_caps_lock(ui, cr, alpha);
    draw_previews(ui, cr, alpha, now);

    if (ui->user_row_rect.h > 0.5) {
        draw_selector_row(ui, cr, alpha, ui->user_row_rect, "<",
                          selected_user(ui), ">");
        ui->user_row_drawn = true;
    }
    if (ui->below_row_rect.h > 0.5) {
        if (ui->below_focus == UI_FOCUS_POWER) {
            draw_selector_row(ui, cr, alpha, ui->below_row_rect, "<",
                              power_labels[ui->power_index], ">");
        } else {
            const Session *session = selected_session(ui);
            draw_selector_row(ui, cr, alpha, ui->below_row_rect, "<",
                              session ? session->name : "no sessions", ">");
        }
        ui->below_row_drawn = true;
    }

    draw_field(ui, cr, alpha, now);
    draw_prompt_and_status(ui, cr, alpha);
}

static const char *
focus_name(UiFocus focus)
{
    switch (focus) {
    case UI_FOCUS_PASSWORD:
        return "password";
    case UI_FOCUS_USER:
        return "user";
    case UI_FOCUS_SESSION:
        return "session";
    case UI_FOCUS_POWER:
        return "power";
    }
    return "password";
}

static const char *
phase_name(UiPhase phase)
{
    switch (phase) {
    case UI_PHASE_IDLE:
        return "idle";
    case UI_PHASE_CONNECTING:
        return "connecting";
    case UI_PHASE_WAITING:
        return "waiting";
    case UI_PHASE_INPUT:
        return "input";
    case UI_PHASE_STARTING:
        return "starting";
    case UI_PHASE_FINISHED:
        return "finished";
    }
    return "idle";
}

static struct json_object *
rect_json(UiRect rect)
{
    struct json_object *object = json_object_new_object();
    json_object_object_add(object, "x", json_object_new_double(rect.x));
    json_object_object_add(object, "y", json_object_new_double(rect.y));
    json_object_object_add(object, "w", json_object_new_double(rect.w));
    json_object_object_add(object, "h", json_object_new_double(rect.h));
    return object;
}

static void
debug_write(Ui *ui)
{
    if (!ui->debug_status_path) {
        return;
    }
    struct json_object *object = json_object_new_object();
    json_object_object_add(object, "width", json_object_new_int(ui->width));
    json_object_object_add(object, "height", json_object_new_int(ui->height));
    json_object_object_add(object, "focus",
                           json_object_new_string(focus_name(ui->focus)));
    json_object_object_add(object, "phase",
                           json_object_new_string(phase_name(ui->phase)));
    json_object_object_add(object, "user",
                           json_object_new_string(selected_user(ui)));
    const Session *session = selected_session(ui);
    json_object_object_add(object, "session",
                           json_object_new_string(session ? session->name : ""));
    struct json_object *exec_array = json_object_new_array();
    if (session) {
        for (size_t i = 0; i < session->argc; i++) {
            json_object_array_add(exec_array,
                                  json_object_new_string(session->argv[i]));
        }
    }
    json_object_object_add(object, "session_exec", exec_array);
    json_object_object_add(object, "sessions",
                           json_object_new_int((int)ui->sessions.count));
    json_object_object_add(object, "users", json_object_new_int((int)ui->users.count));
    json_object_object_add(object, "user_index", json_object_new_int(ui->user_index));
    json_object_object_add(object, "session_index",
                           json_object_new_int(ui->session_index));
    json_object_object_add(object, "power_index",
                           json_object_new_int(ui->power_index));
    json_object_object_add(object, "caps_lock",
                           json_object_new_boolean(ui->caps_lock));
    json_object_object_add(object, "password_len",
                           json_object_new_int((int)ui->password.len));
    json_object_object_add(object, "message", json_object_new_string(ui->message));
    json_object_object_add(object, "prompt", json_object_new_string(ui->prompt));
    json_object_object_add(object, "previews_visible",
                           json_object_new_boolean(previews_enabled(ui)));
    json_object_object_add(object, "last_auth_type",
                           json_object_new_string(ui->last_auth_type));
    json_object_object_add(object, "error_active",
                           json_object_new_boolean(ui->error_active));
    json_object_object_add(object, "power_status",
                           json_object_new_string(ui->power_status));
    json_object_object_add(object, "power_pending",
                           json_object_new_boolean(ui->power_pid > 0));
    json_object_object_add(object, "pointer_inside",
                           json_object_new_boolean(ui->pointer_inside));
    json_object_object_add(object, "pointer_x",
                           json_object_new_double(ui->pointer_x));
    json_object_object_add(object, "pointer_y",
                           json_object_new_double(ui->pointer_y));
    json_object_object_add(object, "pointer_buttons",
                           json_object_new_int64((int64_t)ui->pointer_button_count));
    json_object_object_add(object, "background_loaded",
                           json_object_new_boolean(ui->background != NULL));
    json_object_object_add(object, "background_path",
                           json_object_new_string(ui->background_path
                                                      ? ui->background_path
                                                      : ""));
    json_object_object_add(object, "theme_path",
                           json_object_new_string(ui->config->theme_path
                                                      ? ui->config->theme_path
                                                      : ""));
    json_object_object_add(object, "config_path",
                           json_object_new_string(ui->config->config_path
                                                      ? ui->config->config_path
                                                      : ""));
    json_object_object_add(object, "output_name",
                           json_object_new_string(ui->config->output_name
                                                      ? ui->config->output_name
                                                      : ""));
    json_object_object_add(object, "outputs",
                           json_object_new_int(ui->output_count));
    json_object_object_add(object, "primary_output",
                           json_object_new_string(ui->primary_output));
    json_object_object_add(object, "output_scale_120",
                           json_object_new_int(ui->output_scale_120));
    json_object_object_add(object, "output_width",
                           json_object_new_int(ui->output_width));
    json_object_object_add(object, "output_height",
                           json_object_new_int(ui->output_height));
    json_object_object_add(object, "font",
                           json_object_new_string(ui->resolved_font
                                                      ? ui->resolved_font
                                                      : ""));
    json_object_object_add(object, "power_dry_run",
                           json_object_new_boolean(ui->config->power_dry_run));
    json_object_object_add(object, "field", rect_json(ui->field_rect));
    json_object_object_add(object, "user_row", rect_json(ui->user_row_rect));
    json_object_object_add(object, "below_row", rect_json(ui->below_row_rect));
    json_object_object_add(object, "user_left", rect_json(ui->user_left_rect));
    json_object_object_add(object, "user_right", rect_json(ui->user_right_rect));
    json_object_object_add(object, "below_left", rect_json(ui->below_left_rect));
    json_object_object_add(object, "below_right", rect_json(ui->below_right_rect));
    json_object_object_add(object, "user_preview",
                           rect_json(ui->user_preview_rect));
    json_object_object_add(object, "session_preview",
                           rect_json(ui->session_preview_rect));
    json_object_object_add(object, "user_row_drawn",
                           json_object_new_boolean(ui->user_row_drawn));
    json_object_object_add(object, "below_row_drawn",
                           json_object_new_boolean(ui->below_row_drawn));

    char temp_path[PATH_MAX];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp.%ld", ui->debug_status_path,
             (long)getpid());
    FILE *file = fopen(temp_path, "w");
    if (file) {
        (void)fchmod(fileno(file), 0600);
        const char *text = json_object_to_json_string_ext(
            object, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE);
        fputs(text, file);
        fputc('\n', file);
        fclose(file);
        if (rename(temp_path, ui->debug_status_path) != 0) {
            unlink(temp_path);
        }
    }
    json_object_put(object);
    ui->debug_dirty = false;
}

Ui *
ui_create(Config *config, const char *debug_status_path)
{
    Ui *ui = xcalloc(1, sizeof(*ui));
    ui->config = config;
    ui->focus = UI_FOCUS_PASSWORD;
    ui->below_focus = UI_FOCUS_SESSION;
    ui->phase = UI_PHASE_IDLE;
    ui->user_index = -1;
    ui->session_index = -1;
    ui->power_index = 0;
    ui->exit_code = 0;
    password_init(&ui->password);
    gd_init(&ui->greetd);

    users_load(&ui->users);
    for (size_t i = 0; i < ui->users.count; i++) {
        if (config->default_user && strcmp(ui->users.items[i].name,
                                           config->default_user) == 0) {
            ui->user_index = (int)i;
            break;
        }
    }
    if (ui->user_index < 0 && ui->users.count > 0) {
        if (config->default_user && *config->default_user) {
            rx_log(LOG_WARNING,
                   "configured default_user is not in the selectable user list: %s",
                   config->default_user);
        } else {
            ui->user_index = 0;
        }
    }

    sessions_load(&ui->sessions, config->session_dirs);
    ui->session_index = sessions_default_index(&ui->sessions,
                                               config->default_session);
    if (config->default_session && *config->default_session &&
        ui->sessions.count > 0) {
        bool matched = false;
        for (size_t i = 0; i < ui->sessions.count; i++) {
            if (str_ieq(ui->sessions.items[i].id, config->default_session) ||
                str_ieq(ui->sessions.items[i].name, config->default_session)) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            rx_log(LOG_WARNING,
                   "configured default_session was not found: %s; using %s",
                   config->default_session,
                   ui->sessions.items[ui->session_index].name);
        }
    }

    ui->resolved_font = resolve_font_family(config->font_family,
                                            config->fallback_font_family);
    if (config->clock_font_family && *config->clock_font_family) {
        ui->resolved_clock_font = resolve_font_family(config->clock_font_family,
                                                      NULL);
    } else {
        ui->resolved_clock_font = xstrdup(ui->resolved_font);
    }

    load_background(ui);
    if (debug_status_path && *debug_status_path) {
        ui->debug_status_path = xstrdup(debug_status_path);
    }
    if (ui->sessions.count == 0) {
        show_message(ui, UI_MESSAGE_ERROR,
                     "No Wayland sessions found in the session directories");
    } else if (ui->users.count == 0 &&
               (!config->default_user || !*config->default_user)) {
        show_message(ui, UI_MESSAGE_ERROR, "No local users found");
    }
    ui->dirty = true;
    ui->debug_dirty = true;
    return ui;
}

void
ui_destroy(Ui *ui)
{
    if (!ui) {
        return;
    }
    gd_close(&ui->greetd);
    password_clear(&ui->password);
    users_free(&ui->users);
    sessions_free(&ui->sessions);
    if (ui->background) {
        cairo_surface_destroy(ui->background);
    }
    free(ui->background_path);
    free(ui->resolved_font);
    free(ui->resolved_clock_font);
    free(ui->debug_status_path);
    if (ui->power_pid > 0) {
        kill(ui->power_pid, SIGTERM);
        waitpid(ui->power_pid, NULL, WNOHANG);
    }
    free(ui);
}

void
ui_resize(Ui *ui, int width, int height)
{
    ui->width = width;
    ui->height = height;
    mark_debug(ui);
}

void
ui_start(Ui *ui)
{
    if (ui->started) {
        return;
    }
    ui->started = true;
    ui->fade_started_ms = now_ms();
    ui->user_height_started_ms = ui->fade_started_ms;
    ui->below_height_started_ms = ui->fade_started_ms;
    mark_debug(ui);
}

bool
ui_dirty(const Ui *ui)
{
    return ui->dirty;
}

void
ui_clear_dirty(Ui *ui)
{
    ui->dirty = false;
}

bool
ui_quit(const Ui *ui)
{
    return ui->quit;
}

int
ui_exit_code(const Ui *ui)
{
    return ui->exit_code;
}

int
ui_greetd_fd(const Ui *ui)
{
    return gd_fd(&ui->greetd);
}

bool
ui_show_cursor(const Ui *ui)
{
    return ui->config->show_cursor;
}

static bool
animating(const Ui *ui, int64_t now)
{
    if (ui->phase == UI_PHASE_FINISHED) {
        return false;
    }
    if (ui->started && ui->config->fade_ms > 0 &&
        now < ui->fade_started_ms + ui->config->fade_ms) {
        return true;
    }
    if (fabs(layout_user_height(ui, now) - ui->user_height_to) > 0.1 ||
        fabs(layout_below_height(ui, now) - ui->below_height_to) > 0.1) {
        return true;
    }
    if (ui->error_active && ui->config->login_error_shake &&
        now < ui->error_started_ms + 250) {
        return true;
    }
    return false;
}

int
ui_poll_timeout_ms(const Ui *ui)
{
    int64_t now = now_ms();
    if (animating(ui, now)) {
        return 16;
    }
    return 250;
}

void
ui_tick(Ui *ui)
{
    int64_t now = now_ms();
    bool changed = false;

    if (ui->started) {
        double user = layout_user_height(ui, now);
        double below = layout_below_height(ui, now);
        if (fabs(user - ui->user_height) > 0.01 ||
            fabs(below - ui->below_height) > 0.01) {
            ui->user_height = user;
            ui->below_height = below;
            changed = true;
            ui->debug_dirty = true;
        }
    }

    if (ui->started && ui->config->fade_ms > 0 &&
        now < ui->fade_started_ms + ui->config->fade_ms + 34) {
        changed = true;
    }
    if (ui->error_active && ui->config->login_error_shake &&
        now < ui->error_started_ms + 250 + 34) {
        changed = true;
    }

    if (ui->error_active && now >= ui->error_until_ms) {
        ui->error_active = false;
        changed = true;
        ui->debug_dirty = true;
    }

    if (ui->power_status_until_ms > 0 && now >= ui->power_status_until_ms) {
        ui->power_status_until_ms = 0;
        if (strcmp(ui->message, ui->power_status) == 0) {
            ui->message[0] = '\0';
            ui->message_kind = UI_MESSAGE_NONE;
        }
        ui->power_status[0] = '\0';
        changed = true;
        ui->debug_dirty = true;
    }

    if (ui->power_pid > 0) {
        int status = 0;
        pid_t result = waitpid(ui->power_pid, &status, WNOHANG);
        if (result == ui->power_pid) {
            ui->power_pid = 0;
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                if (ui->config->power_dry_run) {
                    char text[128];
                    snprintf(text, sizeof(text), "dry-run: would %s",
                             ui->power_action);
                    show_status_timed(ui, UI_MESSAGE_INFO, text, 4000);
                } else {
                    show_status_timed(ui, UI_MESSAGE_INFO,
                                      "Power action sent", 4000);
                }
            } else {
                show_status_timed(ui, UI_MESSAGE_ERROR,
                                  "Power action failed", 5000);
            }
            changed = true;
            ui->debug_dirty = true;
        }
    }

    if (ui->config->show_clock) {
        time_t current = time(NULL);
        if (current != ui->last_clock) {
            ui->last_clock = current;
            changed = true;
        }
    }

    if ((ui->phase == UI_PHASE_CONNECTING || ui->phase == UI_PHASE_WAITING ||
         ui->phase == UI_PHASE_INPUT) &&
        now - ui->auth_started_ms > ui->config->auth_timeout_ms) {
        cancel_auth(ui, false);
        trigger_login_error(ui, "Authentication timed out");
        changed = true;
    }

    if (changed) {
        mark_dirty(ui);
    }
    if (ui->debug_dirty) {
        layout(ui, now);
        debug_write(ui);
    }
}
