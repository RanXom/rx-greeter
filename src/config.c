#include "config.h"

#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

static Rgba
rgba(double r, double g, double b, double a)
{
    return (Rgba){ .r = r, .g = g, .b = b, .a = a };
}

static Rgba
rgba_hex(unsigned int rgb, double a)
{
    return rgba(((rgb >> 16) & 0xff) / 255.0,
                ((rgb >> 8) & 0xff) / 255.0,
                (rgb & 0xff) / 255.0,
                a);
}

void
config_init(Config *config)
{
    memset(config, 0, sizeof(*config));

    config->background_image = xstrdup("");

    /* The live waylock theme is the source of truth for these defaults. */
    config->background_color = rgba_hex(0x121415, 1.0);
    config->surface_color = rgba_hex(0x1e2021, 1.0);
    config->surface_opacity = 0.80;
    config->text_color = rgba_hex(0xe2e2e3, 1.0);
    config->primary_color = rgba_hex(0xa2cdde, 1.0);
    config->error_color = rgba_hex(0xffb4ab, 1.0);
    config->outline_color = rgba_hex(0x8b9295, 1.0);
    config->background_overlay = 0.22;

    config->box_width = 240.0;
    config->box_height = 42.0;
    config->border_radius = 0.0;
    config->box_x = 0.0;
    config->box_y = 0.0;
    config->dot_spacing = 9.0;
    config->dot_radius = 2.4;

    config->font_family = xstrdup("monospace");
    config->fallback_font_family = xstrdup("sans");
    config->font_size = 16.0;

    config->fade_ms = 250;
    config->animation_ms = 200;
    config->element_opacity = 1.0;
    config->element_spacing = 15.0;
    config->show_cursor = true;

    config->show_clock = false;
    config->show_date = false;
    config->clock_format = xstrdup("%H:%M");
    config->date_format = xstrdup("%A, %B %d");
    config->clock_font_family = xstrdup("");
    config->clock_font_size = 50.0;
    config->date_font_size = 17.0;
    config->clock_color = config->text_color;
    config->clock_x = 0.0;
    config->clock_y = -180.0;

    config->show_help_tips = false;
    config->help_tips_font_size = 11.0;
    config->help_tips_color = config->outline_color;
    config->show_caps_lock_indicator = true;
    config->caps_lock_font_size = 12.0;
    config->caps_lock_color = rgba_hex(0xffaa00, 1.0);

    config->show_user_selector = true;
    config->show_session_selector = true;
    config->show_power_buttons = true;
    config->show_selector_preview = true;
    config->selector_preview_font_size = 13.0;
    config->selector_preview_color = config->outline_color;
    config->selector_preview_margin = 10.0;
    config->selector_height = 35.0;
    config->selector_spacing = 10.0;
    config->selector_font_size = 14.0;
    config->selector_arrow_font_size = 17.0;
    config->selector_arrow_width = 30.0;
    config->selector_radius = 10.0;

    config->default_user = xstrdup("");
    config->default_session = xstrdup("");
    config->output_name = xstrdup("");
    config->allow_empty_password = false;
    config->clear_password_on_error = true;
    config->login_error_shake = true;
    config->login_error_border_width = 4.0;
    config->error_hold_ms = 900;
    config->auth_timeout_ms = 30000;

    config->power_helper = xstrdup("");
    config->power_dry_run = false;
    config->session_dirs = xstrdup("");
    config->theme_path = NULL;
    config->config_path = NULL;
}

void
config_free(Config *config)
{
    if (!config) {
        return;
    }
    free(config->background_image);
    free(config->font_family);
    free(config->fallback_font_family);
    free(config->clock_format);
    free(config->date_format);
    free(config->clock_font_family);
    free(config->default_user);
    free(config->default_session);
    free(config->output_name);
    free(config->power_helper);
    free(config->session_dirs);
    free(config->theme_path);
    free(config->config_path);
    memset(config, 0, sizeof(*config));
}

static void
normalize_key(const char *key, char *out, size_t cap)
{
    size_t j = 0;
    for (size_t i = 0; key[i] && j + 1 < cap; i++) {
        unsigned char c = (unsigned char)key[i];
        if (isalnum(c)) {
            out[j++] = (char)tolower(c);
        }
    }
    out[j] = '\0';
}

static bool
key_is(const char *normalized, const char *name)
{
    char want[96];
    normalize_key(name, want, sizeof(want));
    return strcmp(normalized, want) == 0;
}

static void
set_string(char **slot, const char *value)
{
    free(*slot);
    *slot = xstrdup(value);
}

static void
set_string_expanded(char **slot, const char *value)
{
    char *expanded = expand_path(value);
    free(*slot);
    *slot = expanded ? expanded : xstrdup(value);
}

static bool
value_bool(const char *value, bool *out)
{
    bool parsed = false;
    if (!parse_bool(value, &parsed)) {
        return false;
    }
    *out = parsed;
    return true;
}

static bool
value_double(const char *value, double min, double max, double *out)
{
    double parsed = 0.0;
    if (!parse_double(value, &parsed) || parsed < min || parsed > max) {
        return false;
    }
    *out = parsed;
    return true;
}

static bool
value_int(const char *value, int min, int max, int *out)
{
    long parsed = 0;
    if (!parse_long(value, &parsed) || parsed < min || parsed > max) {
        return false;
    }
    *out = (int)parsed;
    return true;
}

static bool
value_color(const char *value, Rgba *out)
{
    Rgba parsed;
    if (!color_parse(value, &parsed)) {
        return false;
    }
    *out = parsed;
    return true;
}

bool
color_parse(const char *text, Rgba *out)
{
    if (!text) {
        return false;
    }
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%s", text);
    char *value = trim_whitespace(buffer);
    strip_matching_quotes(value);
    value = trim_whitespace(value);
    if (*value == '#') {
        value++;
    } else if (value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
        value += 2;
    }

    size_t len = strlen(value);
    unsigned int r = 0, g = 0, b = 0, a = 255;
    if (len == 3) {
        for (size_t i = 0; i < 3; i++) {
            if (!isxdigit((unsigned char)value[i])) {
                return false;
            }
        }
        unsigned int rv = (unsigned int)(isdigit((unsigned char)value[0])
                                             ? value[0] - '0'
                                             : tolower((unsigned char)value[0]) - 'a' + 10);
        unsigned int gv = (unsigned int)(isdigit((unsigned char)value[1])
                                             ? value[1] - '0'
                                             : tolower((unsigned char)value[1]) - 'a' + 10);
        unsigned int bv = (unsigned int)(isdigit((unsigned char)value[2])
                                             ? value[2] - '0'
                                             : tolower((unsigned char)value[2]) - 'a' + 10);
        r = rv * 17;
        g = gv * 17;
        b = bv * 17;
    } else if (len == 6 || len == 8) {
        char part[3] = { 0, 0, 0 };
        for (size_t i = 0; i < len / 2; i++) {
            part[0] = value[i * 2];
            part[1] = value[i * 2 + 1];
            char *end = NULL;
            unsigned long parsed = strtoul(part, &end, 16);
            if (!end || *end != '\0') {
                return false;
            }
            if (i == 0) {
                r = (unsigned int)parsed;
            } else if (i == 1) {
                g = (unsigned int)parsed;
            } else if (i == 2) {
                b = (unsigned int)parsed;
            } else {
                a = (unsigned int)parsed;
            }
        }
    } else {
        return false;
    }

    out->r = r / 255.0;
    out->g = g / 255.0;
    out->b = b / 255.0;
    out->a = a / 255.0;
    return true;
}

static bool
config_apply_line_ex(Config *config, const char *line, bool warn_unknown)
{
    if (!line) {
        return false;
    }
    char *copy = xstrdup(line);
    char *text = trim_whitespace(copy);
    if (*text == '\0' || *text == '#' || *text == ';' || *text == '[') {
        free(copy);
        return false;
    }

    char *eq = strchr(text, '=');
    if (!eq) {
        free(copy);
        return false;
    }
    *eq = '\0';
    char *key = trim_whitespace(text);
    char *value = trim_whitespace(eq + 1);
    strip_matching_quotes(value);
    value = trim_whitespace(value);

    char key_norm[96];
    normalize_key(key, key_norm, sizeof(key_norm));
    bool used = true;
    bool known = true;

    if (key_is(key_norm, "background_image")) {
        set_string_expanded(&config->background_image, value);
    } else if (key_is(key_norm, "theme_path")) {
        set_string_expanded(&config->theme_path, value);
    } else if (key_is(key_norm, "session_dirs")) {
        set_string_expanded(&config->session_dirs, value);
    } else if (key_is(key_norm, "background_color")) {
        used = value_color(value, &config->background_color);
    } else if (key_is(key_norm, "background_overlay")) {
        used = value_double(value, 0.0, 1.0, &config->background_overlay);
    } else if (key_is(key_norm, "surface_color")) {
        used = value_color(value, &config->surface_color);
    } else if (key_is(key_norm, "surface_opacity")) {
        used = value_double(value, 0.0, 1.0, &config->surface_opacity);
    } else if (key_is(key_norm, "text_color")) {
        used = value_color(value, &config->text_color);
    } else if (key_is(key_norm, "primary_color")) {
        used = value_color(value, &config->primary_color);
    } else if (key_is(key_norm, "error_color") || key_is(key_norm, "login_error_color")) {
        used = value_color(value, &config->error_color);
    } else if (key_is(key_norm, "outline_color")) {
        used = value_color(value, &config->outline_color);
    } else if (key_is(key_norm, "box_width") ||
               key_is(key_norm, "password_field_width")) {
        used = value_double(value, 40.0, 4000.0, &config->box_width);
    } else if (key_is(key_norm, "box_height") ||
               key_is(key_norm, "password_field_height")) {
        used = value_double(value, 16.0, 400.0, &config->box_height);
    } else if (key_is(key_norm, "border_radius") ||
               key_is(key_norm, "password_field_radius")) {
        used = value_double(value, 0.0, 400.0, &config->border_radius);
    } else if (key_is(key_norm, "box_x")) {
        used = value_double(value, -100000.0, 100000.0, &config->box_x);
    } else if (key_is(key_norm, "box_y")) {
        used = value_double(value, -100000.0, 100000.0, &config->box_y);
    } else if (key_is(key_norm, "dot_spacing")) {
        used = value_double(value, 2.0, 80.0, &config->dot_spacing);
    } else if (key_is(key_norm, "dot_radius")) {
        used = value_double(value, 0.5, 32.0, &config->dot_radius);
    } else if (key_is(key_norm, "font_family")) {
        set_string(&config->font_family, value);
    } else if (key_is(key_norm, "fallback_font_family")) {
        set_string(&config->fallback_font_family, value);
    } else if (key_is(key_norm, "font_size")) {
        used = value_double(value, 6.0, 200.0, &config->font_size);
    } else if (key_is(key_norm, "fade_ms") ||
               key_is(key_norm, "fade_in_duration")) {
        used = value_int(value, 0, 10000, &config->fade_ms);
    } else if (key_is(key_norm, "animation_ms") ||
               key_is(key_norm, "animation_duration")) {
        used = value_int(value, 0, 2000, &config->animation_ms);
    } else if (key_is(key_norm, "element_opacity")) {
        used = value_double(value, 0.0, 1.0, &config->element_opacity);
    } else if (key_is(key_norm, "element_spacing")) {
        used = value_double(value, 0.0, 400.0, &config->element_spacing);
    } else if (key_is(key_norm, "show_cursor")) {
        used = value_bool(value, &config->show_cursor);
    } else if (key_is(key_norm, "show_clock")) {
        used = value_bool(value, &config->show_clock);
    } else if (key_is(key_norm, "show_date")) {
        used = value_bool(value, &config->show_date);
    } else if (key_is(key_norm, "clock_format")) {
        set_string(&config->clock_format, value);
    } else if (key_is(key_norm, "date_format")) {
        set_string(&config->date_format, value);
    } else if (key_is(key_norm, "clock_font_family")) {
        set_string(&config->clock_font_family, value);
    } else if (key_is(key_norm, "clock_font_size")) {
        used = value_double(value, 6.0, 400.0, &config->clock_font_size);
    } else if (key_is(key_norm, "date_font_size")) {
        used = value_double(value, 6.0, 200.0, &config->date_font_size);
    } else if (key_is(key_norm, "clock_color")) {
        used = value_color(value, &config->clock_color);
    } else if (key_is(key_norm, "clock_x")) {
        used = value_double(value, -100000.0, 100000.0, &config->clock_x);
    } else if (key_is(key_norm, "clock_y")) {
        used = value_double(value, -100000.0, 100000.0, &config->clock_y);
    } else if (key_is(key_norm, "show_help_tips")) {
        used = value_bool(value, &config->show_help_tips);
    } else if (key_is(key_norm, "help_tips_font_size")) {
        used = value_double(value, 6.0, 100.0, &config->help_tips_font_size);
    } else if (key_is(key_norm, "help_tips_color")) {
        used = value_color(value, &config->help_tips_color);
    } else if (key_is(key_norm, "show_caps_lock_indicator")) {
        used = value_bool(value, &config->show_caps_lock_indicator);
    } else if (key_is(key_norm, "caps_lock_font_size")) {
        used = value_double(value, 6.0, 100.0, &config->caps_lock_font_size);
    } else if (key_is(key_norm, "caps_lock_color") ||
               key_is(key_norm, "caps_lock_indicator_color")) {
        used = value_color(value, &config->caps_lock_color);
    } else if (key_is(key_norm, "show_user_selector")) {
        used = value_bool(value, &config->show_user_selector);
    } else if (key_is(key_norm, "show_session_selector")) {
        used = value_bool(value, &config->show_session_selector);
    } else if (key_is(key_norm, "show_power_buttons")) {
        used = value_bool(value, &config->show_power_buttons);
    } else if (key_is(key_norm, "show_selector_preview")) {
        used = value_bool(value, &config->show_selector_preview);
    } else if (key_is(key_norm, "selector_preview_font_size")) {
        used = value_double(value, 6.0, 100.0, &config->selector_preview_font_size);
    } else if (key_is(key_norm, "selector_preview_color")) {
        used = value_color(value, &config->selector_preview_color);
    } else if (key_is(key_norm, "selector_preview_margin")) {
        used = value_double(value, 0.0, 400.0, &config->selector_preview_margin);
    } else if (key_is(key_norm, "selector_height")) {
        used = value_double(value, 10.0, 400.0, &config->selector_height);
    } else if (key_is(key_norm, "selector_spacing")) {
        used = value_double(value, 0.0, 400.0, &config->selector_spacing);
    } else if (key_is(key_norm, "selector_font_size")) {
        used = value_double(value, 6.0, 200.0, &config->selector_font_size);
    } else if (key_is(key_norm, "selector_arrow_font_size")) {
        used = value_double(value, 6.0, 200.0, &config->selector_arrow_font_size);
    } else if (key_is(key_norm, "selector_arrow_width")) {
        used = value_double(value, 8.0, 200.0, &config->selector_arrow_width);
    } else if (key_is(key_norm, "selector_radius")) {
        used = value_double(value, 0.0, 400.0, &config->selector_radius);
    } else if (key_is(key_norm, "default_user")) {
        set_string(&config->default_user, value);
    } else if (key_is(key_norm, "default_session")) {
        set_string(&config->default_session, value);
    } else if (key_is(key_norm, "output") || key_is(key_norm, "output_name")) {
        set_string(&config->output_name, value);
    } else if (key_is(key_norm, "allow_empty_password")) {
        used = value_bool(value, &config->allow_empty_password);
    } else if (key_is(key_norm, "clear_password_on_error")) {
        used = value_bool(value, &config->clear_password_on_error);
    } else if (key_is(key_norm, "login_error_shake")) {
        used = value_bool(value, &config->login_error_shake);
    } else if (key_is(key_norm, "login_error_border_width")) {
        used = value_double(value, 0.0, 64.0, &config->login_error_border_width);
    } else if (key_is(key_norm, "error_hold_ms")) {
        used = value_int(value, 0, 10000, &config->error_hold_ms);
    } else if (key_is(key_norm, "auth_timeout_ms")) {
        used = value_int(value, 1000, 600000, &config->auth_timeout_ms);
    } else if (key_is(key_norm, "power_helper")) {
        set_string_expanded(&config->power_helper, value);
    } else if (key_is(key_norm, "power_dry_run")) {
        used = value_bool(value, &config->power_dry_run);
    } else {
        used = false;
        known = false;
    }

    if (warn_unknown) {
        if (!known) {
            rx_log(LOG_WARNING, "unknown configuration key '%s'", key);
        } else if (!used) {
            rx_log(LOG_WARNING, "invalid value for '%s': %s", key, value);
        }
    }
    free(copy);
    return true;
}

bool
config_load_file(Config *config, const char *path, bool warn_unknown)
{
    char *data = read_file(path, 256 * 1024, NULL);
    if (!data) {
        return false;
    }
    char *save = NULL;
    for (char *line = strtok_r(data, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        size_t len = strlen(line);
        if (len && line[len - 1] == '\r') {
            line[len - 1] = '\0';
        }
        config_apply_line_ex(config, line, warn_unknown);
    }
    free(data);
    return true;
}

bool
config_apply_line(Config *config, const char *line)
{
    return config_apply_line_ex(config, line, true);
}

static char *
find_first_readable(const char *const *paths, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (path_is_readable(paths[i])) {
            return xstrdup(paths[i]);
        }
    }
    return NULL;
}

char *
config_find_config_path(const char *explicit_path)
{
    if (explicit_path && *explicit_path) {
        return path_is_readable(explicit_path) ? xstrdup(explicit_path) : NULL;
    }
    const char *env = getenv("RX_GREETER_CONFIG");
    if (env && *env) {
        return path_is_readable(env) ? xstrdup(env) : NULL;
    }

    static const char *const system_paths[] = {
        "/etc/rx-greeter/config.conf",
        "/var/lib/rx-greeter/config.conf",
        "/usr/local/etc/rx-greeter/config.conf",
    };
    char *found = find_first_readable(system_paths,
                                      sizeof(system_paths) / sizeof(system_paths[0]));
    if (found) {
        return found;
    }

    char candidate[4096];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        snprintf(candidate, sizeof(candidate), "%s/rx-greeter/config.conf", xdg);
        if (path_is_readable(candidate)) {
            return xstrdup(candidate);
        }
    }
    const char *home = getenv("HOME");
    if (home && *home) {
        snprintf(candidate, sizeof(candidate), "%s/.config/rx-greeter/config.conf", home);
        if (path_is_readable(candidate)) {
            return xstrdup(candidate);
        }
    }
    return NULL;
}

char *
config_find_theme_path(const char *explicit_path)
{
    if (explicit_path && *explicit_path) {
        return path_is_readable(explicit_path) ? xstrdup(explicit_path) : NULL;
    }

    const char *env = getenv("RX_GREETER_THEME");
    if (env && *env) {
        return path_is_readable(env) ? xstrdup(env) : NULL;
    }

    static const char *const system_paths[] = {
        "/etc/rx-greeter/theme.conf",
        "/var/lib/rx-greeter/theme.conf",
        "/usr/local/etc/rx-greeter/theme.conf",
        "/usr/local/share/rx-greeter/theme.conf",
        "/usr/share/rx-greeter/theme.conf",
    };
    char *found = find_first_readable(system_paths,
                                      sizeof(system_paths) / sizeof(system_paths[0]));
    if (found) {
        return found;
    }

    char candidate[4096];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        snprintf(candidate, sizeof(candidate), "%s/rx-greeter/theme.conf", xdg);
        if (path_is_readable(candidate)) {
            return xstrdup(candidate);
        }
    }
    const char *home = getenv("HOME");
    if (home && *home) {
        snprintf(candidate, sizeof(candidate), "%s/.config/rx-greeter/theme.conf", home);
        if (path_is_readable(candidate)) {
            return xstrdup(candidate);
        }
    }
    return NULL;
}

static void
print_color(const char *name, Rgba color)
{
    printf("%s=#%02x%02x%02x%02x\n", name,
           (unsigned int)(color.r * 255.0 + 0.5),
           (unsigned int)(color.g * 255.0 + 0.5),
           (unsigned int)(color.b * 255.0 + 0.5),
           (unsigned int)(color.a * 255.0 + 0.5));
}

void
config_print(const Config *config)
{
    printf("# rx-greeter effective configuration\n");
    printf("config_path=%s\n", config->config_path ? config->config_path : "(none)");
    printf("theme_path=%s\n", config->theme_path ? config->theme_path : "(built-in defaults)");
    printf("background_image=%s\n", config->background_image ? config->background_image : "");
    print_color("background_color", config->background_color);
    printf("background_overlay=%.3f\n", config->background_overlay);
    print_color("surface_color", config->surface_color);
    printf("surface_opacity=%.3f\n", config->surface_opacity);
    print_color("text_color", config->text_color);
    print_color("primary_color", config->primary_color);
    print_color("error_color", config->error_color);
    print_color("outline_color", config->outline_color);
    printf("box_width=%.1f\n", config->box_width);
    printf("box_height=%.1f\n", config->box_height);
    printf("border_radius=%.1f\n", config->border_radius);
    printf("box_x=%.1f\n", config->box_x);
    printf("box_y=%.1f\n", config->box_y);
    printf("dot_spacing=%.2f\n", config->dot_spacing);
    printf("dot_radius=%.2f\n", config->dot_radius);
    printf("font_family=%s\n", config->font_family);
    printf("fallback_font_family=%s\n", config->fallback_font_family);
    printf("font_size=%.1f\n", config->font_size);
    printf("fade_ms=%d\n", config->fade_ms);
    printf("animation_ms=%d\n", config->animation_ms);
    printf("element_opacity=%.3f\n", config->element_opacity);
    printf("element_spacing=%.1f\n", config->element_spacing);
    printf("show_cursor=%s\n", config->show_cursor ? "true" : "false");
    printf("show_clock=%s\n", config->show_clock ? "true" : "false");
    printf("show_date=%s\n", config->show_date ? "true" : "false");
    printf("clock_format=%s\n", config->clock_format);
    printf("date_format=%s\n", config->date_format);
    printf("clock_font_size=%.1f\n", config->clock_font_size);
    printf("clock_x=%.1f\n", config->clock_x);
    printf("clock_y=%.1f\n", config->clock_y);
    printf("show_help_tips=%s\n", config->show_help_tips ? "true" : "false");
    printf("show_caps_lock_indicator=%s\n", config->show_caps_lock_indicator ? "true" : "false");
    printf("show_user_selector=%s\n", config->show_user_selector ? "true" : "false");
    printf("show_session_selector=%s\n", config->show_session_selector ? "true" : "false");
    printf("show_power_buttons=%s\n", config->show_power_buttons ? "true" : "false");
    printf("show_selector_preview=%s\n", config->show_selector_preview ? "true" : "false");
    printf("default_user=%s\n", config->default_user);
    printf("default_session=%s\n", config->default_session);
    printf("output_name=%s\n", config->output_name);
    printf("session_dirs=%s\n", config->session_dirs);
    printf("allow_empty_password=%s\n", config->allow_empty_password ? "true" : "false");
    printf("clear_password_on_error=%s\n", config->clear_password_on_error ? "true" : "false");
    printf("login_error_shake=%s\n", config->login_error_shake ? "true" : "false");
    printf("auth_timeout_ms=%d\n", config->auth_timeout_ms);
    printf("power_helper=%s\n", config->power_helper);
    printf("power_dry_run=%s\n", config->power_dry_run ? "true" : "false");
}
