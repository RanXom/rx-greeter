#ifndef RX_GREETER_CONFIG_H
#define RX_GREETER_CONFIG_H

#include <stdbool.h>

typedef struct {
    double r;
    double g;
    double b;
    double a;
} Rgba;

typedef struct {
    /* Background and palette. These names intentionally match waylock. */
    char *background_image;
    Rgba background_color;
    Rgba surface_color;
    double surface_opacity;
    Rgba text_color;
    Rgba primary_color;
    Rgba error_color;
    Rgba outline_color;
    double background_overlay;

    /* Password field geometry, matching waylock where possible. */
    double box_width;
    double box_height;
    double border_radius;
    double box_x;
    double box_y;
    double dot_spacing;
    double dot_radius;

    /* Typography. */
    char *font_family;
    char *fallback_font_family;
    double font_size;

    /* Motion and layout. */
    int fade_ms;
    int animation_ms;
    double element_opacity;
    double element_spacing;
    bool show_cursor;

    /* Clock and date. */
    bool show_clock;
    bool show_date;
    char *clock_format;
    char *date_format;
    char *clock_font_family;
    double clock_font_size;
    double date_font_size;
    Rgba clock_color;
    double clock_x;
    double clock_y;

    /* Help and state indicators. */
    bool show_help_tips;
    double help_tips_font_size;
    Rgba help_tips_color;
    bool show_caps_lock_indicator;
    double caps_lock_font_size;
    Rgba caps_lock_color;

    /* Selectors. */
    bool show_user_selector;
    bool show_session_selector;
    bool show_power_buttons;
    bool show_selector_preview;
    double selector_preview_font_size;
    Rgba selector_preview_color;
    double selector_preview_margin;
    double selector_height;
    double selector_spacing;
    double selector_font_size;
    double selector_arrow_font_size;
    double selector_arrow_width;
    double selector_radius;

    /* Login behaviour. */
    char *default_user;
    char *default_session;
    bool allow_empty_password;
    bool clear_password_on_error;
    bool login_error_shake;
    double login_error_border_width;
    int error_hold_ms;
    int auth_timeout_ms;

    /* Privileged actions. */
    char *power_helper;
    bool power_dry_run;

    /* Resolved at runtime, not a theme key. */
    char *theme_path;
} Config;

void config_init(Config *config);
void config_free(Config *config);

bool config_apply_line(Config *config, const char *line);
bool config_load_file(Config *config, const char *path);
char *config_find_theme_path(const char *explicit_path);

bool color_parse(const char *text, Rgba *out);
void config_print(const Config *config);

#endif
