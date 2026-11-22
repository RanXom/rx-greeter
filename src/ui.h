#ifndef RX_GREETER_UI_H
#define RX_GREETER_UI_H

#include "config.h"
#include "greetd.h"
#include "password.h"
#include "sessions.h"
#include "users.h"

#include <cairo/cairo.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>
#include <xkbcommon/xkbcommon.h>

typedef struct {
    double x;
    double y;
    double w;
    double h;
} UiRect;

typedef enum {
    UI_FOCUS_PASSWORD = 0,
    UI_FOCUS_USER,
    UI_FOCUS_SESSION,
    UI_FOCUS_POWER,
} UiFocus;

typedef enum {
    UI_PHASE_IDLE = 0,
    UI_PHASE_CONNECTING,
    UI_PHASE_WAITING,
    UI_PHASE_INPUT,
    UI_PHASE_STARTING,
    UI_PHASE_FINISHED,
} UiPhase;

typedef enum {
    UI_MESSAGE_NONE = 0,
    UI_MESSAGE_INFO,
    UI_MESSAGE_ERROR,
} UiMessageKind;

typedef struct {
    Config *config;
    UserList users;
    SessionList sessions;

    PasswordBuffer password;
    GdConn greetd;
    UiFocus focus;
    UiFocus below_focus;
    UiPhase phase;
    int user_index;
    int session_index;
    int power_index;

    bool auth_input_secret;
    bool initial_password_ready;
    bool start_sent;
    char prompt[1024];
    char message[1024];
    UiMessageKind message_kind;
    char last_auth_type[32];
    int64_t auth_started_ms;

    bool error_active;
    int64_t error_started_ms;
    int64_t error_until_ms;

    bool caps_lock;
    bool quit;
    int exit_code;

    int width;
    int height;
    bool started;

    cairo_surface_t *background;
    char *background_path;
    char *resolved_font;
    char *resolved_clock_font;

    int64_t fade_started_ms;
    double user_height;
    double user_height_from;
    double user_height_to;
    int64_t user_height_started_ms;
    double below_height;
    double below_height_from;
    double below_height_to;
    int64_t below_height_started_ms;

    UiRect field_rect;
    UiRect user_row_rect;
    UiRect below_row_rect;
    UiRect user_left_rect;
    UiRect user_center_rect;
    UiRect user_right_rect;
    UiRect below_left_rect;
    UiRect below_center_rect;
    UiRect below_right_rect;
    UiRect user_preview_rect;
    UiRect session_preview_rect;
    bool user_row_drawn;
    bool below_row_drawn;

    bool pointer_inside;
    double pointer_x;
    double pointer_y;
    uint64_t pointer_button_count;

    pid_t power_pid;
    char power_action[32];
    char power_status[256];
    int64_t power_status_until_ms;

    char *debug_status_path;
    bool debug_dirty;
    bool dirty;
    time_t last_clock;

    int output_count;
    char primary_output[64];
    int output_scale_120;
    int output_width;
    int output_height;
} Ui;

Ui *ui_create(Config *config, const char *debug_status_path);
void ui_destroy(Ui *ui);

void ui_resize(Ui *ui, int width, int height);
void ui_start(Ui *ui);
void ui_tick(Ui *ui);
bool ui_dirty(const Ui *ui);
void ui_clear_dirty(Ui *ui);
bool ui_quit(const Ui *ui);
int ui_exit_code(const Ui *ui);

int ui_greetd_fd(const Ui *ui);
void ui_greetd_readable(Ui *ui);

void ui_key(Ui *ui, xkb_keysym_t sym, const char *text, bool ctrl, bool shift);
void ui_set_caps_lock(Ui *ui, bool caps_lock);
void ui_pointer_enter(Ui *ui, double x, double y);
void ui_pointer_motion(Ui *ui, double x, double y);
void ui_pointer_leave(Ui *ui);
void ui_pointer_button(Ui *ui, double x, double y, int width, int height,
                       uint32_t button, bool pressed);

void ui_set_output_info(Ui *ui, int count, const char *primary,
                        int scale_120, int width, int height);

bool ui_show_cursor(const Ui *ui);
int ui_poll_timeout_ms(const Ui *ui);
void ui_render(Ui *ui, cairo_t *cr, int width, int height);

const char *ui_selected_user(const Ui *ui);
const Session *ui_selected_session(const Ui *ui);

#endif
