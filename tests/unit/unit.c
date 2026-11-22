/* _GNU_SOURCE is provided by the build system. */

#include "../../src/config.h"
#include "../../src/greetd.h"
#include "../../src/password.h"
#include "../../src/sessions.h"
#include "../../src/users.h"
#include "../../src/util.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;

#define EXPECT(condition)                                                        \
    do {                                                                         \
        if (!(condition)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            failures++;                                                          \
        }                                                                        \
    } while (0)

static void
write_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");
    if (!file) {
        perror(path);
        exit(1);
    }
    fputs(text, file);
    fclose(file);
}

static bool
near(double a, double b)
{
    return fabs(a - b) < 0.002;
}

static void
test_colors(void)
{
    Rgba color;
    EXPECT(color_parse("#121415", &color));
    EXPECT(near(color.r, 0x12 / 255.0));
    EXPECT(near(color.g, 0x14 / 255.0));
    EXPECT(near(color.b, 0x15 / 255.0));
    EXPECT(near(color.a, 1.0));

    EXPECT(color_parse("0x11223344", &color));
    EXPECT(near(color.r, 0x11 / 255.0));
    EXPECT(near(color.a, 0x44 / 255.0));

    EXPECT(color_parse("#abc", &color));
    EXPECT(near(color.r, 0xaa / 255.0));

    EXPECT(!color_parse("not-a-color", &color));
    EXPECT(!color_parse("#12", &color));
}

static void
test_config(void)
{
    char dir[] = "/tmp/rx-greeter-unit-XXXXXX";
    EXPECT(mkdtemp(dir) != NULL);
    char path[512];
    char cmd[1024];
    snprintf(path, sizeof(path), "%s/theme.conf", dir);
    write_text(path,
               "# comment\n"
               "background_color = #010203\n"
               "surface_color = #101112\n"
               "surface_opacity = 0.5\n"
               "text_color = #202122\n"
               "primary_color = #303132\n"
               "error_color = #404142\n"
               "outline_color = #505152\n"
               "box_width = 300\n"
               "box_height = 44\n"
               "border_radius = 7\n"
               "dot_spacing = 11\n"
               "dot_radius = 3\n"
               "font_family = Cartograph CF\n"
               "font_size = 19\n"
               "fade_ms = 500\n"
               "show_clock = true\n"
               "clock_format = %H:%M:%S\n"
               "show_cursor = false\n"
               "default_user = alice\n"
               "default_session = Sway\n"
               "allow_empty_password = yes\n"
               "power_dry_run = true\n"
               "unknown_key = ignored\n");

    Config config;
    config_init(&config);
    EXPECT(strcmp(config.default_user, "ranxom") == 0);
    EXPECT(near(config.background_color.r, 0x12 / 255.0));
    EXPECT(strcmp(config.font_family, "IosevkaTerm Nerd Font Mono") == 0);

    EXPECT(config_load_file(&config, path));
    EXPECT(near(config.background_color.r, 1 / 255.0));
    EXPECT(near(config.surface_opacity, 0.5));
    EXPECT(near(config.box_width, 300));
    EXPECT(near(config.box_height, 44));
    EXPECT(near(config.border_radius, 7));
    EXPECT(near(config.dot_spacing, 11));
    EXPECT(near(config.font_size, 19));
    EXPECT(config.fade_ms == 500);
    EXPECT(config.show_clock);
    EXPECT(!config.show_cursor);
    EXPECT(strcmp(config.clock_format, "%H:%M:%S") == 0);
    EXPECT(strcmp(config.default_user, "alice") == 0);
    EXPECT(strcmp(config.default_session, "Sway") == 0);
    EXPECT(config.allow_empty_password);
    EXPECT(config.power_dry_run);
    EXPECT(config.theme_path && strcmp(config.theme_path, path) == 0);

    config_apply_line(&config, "surface_opacity = nope");
    EXPECT(near(config.surface_opacity, 0.5));
    config_apply_line(&config, "surface_opacity = 2.0");
    EXPECT(near(config.surface_opacity, 0.5));

    config_free(&config);

    setenv("RX_GREETER_THEME", path, 1);
    char *found = config_find_theme_path(NULL);
    EXPECT(found && strcmp(found, path) == 0);
    free(found);
    unsetenv("RX_GREETER_THEME");

    char missing[512];
    snprintf(missing, sizeof(missing), "%s/missing.conf", dir);
    EXPECT(config_find_theme_path(missing) == NULL);

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    EXPECT(system(cmd) == 0);
}

static void
test_exec_parser(void)
{
    char **argv = NULL;
    size_t argc = 0;

    EXPECT(desktop_parse_exec("/usr/bin/foo --bar \"two words\" %U",
                              "Foo", "", "/tmp/foo.desktop", &argv, &argc));
    EXPECT(argc == 3);
    EXPECT(strcmp(argv[0], "/usr/bin/foo") == 0);
    EXPECT(strcmp(argv[1], "--bar") == 0);
    EXPECT(strcmp(argv[2], "two words") == 0);
    for (size_t i = 0; i < argc; i++) {
        free(argv[i]);
    }
    free(argv);

    EXPECT(desktop_parse_exec("env FOO=bar app %% %c",
                              "Foo Name", "", "/tmp/foo.desktop", &argv, &argc));
    EXPECT(argc == 5);
    EXPECT(strcmp(argv[0], "env") == 0);
    EXPECT(strcmp(argv[1], "FOO=bar") == 0);
    EXPECT(strcmp(argv[2], "app") == 0);
    EXPECT(strcmp(argv[3], "%") == 0);
    EXPECT(strcmp(argv[4], "Foo Name") == 0);
    for (size_t i = 0; i < argc; i++) {
        free(argv[i]);
    }
    free(argv);

    EXPECT(desktop_parse_exec("/bin/app %i", "App", "app-icon",
                              "/tmp/app.desktop", &argv, &argc));
    EXPECT(argc == 3);
    EXPECT(strcmp(argv[1], "--icon") == 0);
    EXPECT(strcmp(argv[2], "app-icon") == 0);
    for (size_t i = 0; i < argc; i++) {
        free(argv[i]);
    }
    free(argv);

    EXPECT(!desktop_parse_exec("/bin/app \"unterminated", "App", "",
                               "/tmp/app.desktop", &argv, &argc));
    EXPECT(!desktop_parse_exec("", "App", "", "/tmp/app.desktop", &argv, &argc));
}

static void
test_sessions(void)
{
    char root[] = "/tmp/rx-greeter-sessions-XXXXXX";
    EXPECT(mkdtemp(root) != NULL);
    char local[512];
    char system_dir[512];
    snprintf(local, sizeof(local), "%s/local", root);
    snprintf(system_dir, sizeof(system_dir), "%s/system", root);
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s' '%s'", local, system_dir);
    EXPECT(system(cmd) == 0);

    char path[1024];
    snprintf(path, sizeof(path), "%s/sway.desktop", local);
    write_text(path,
               "[Desktop Entry]\n"
               "Name=Sway Local\n"
               "Exec=/home/ranxom/.local/bin/sway-session\n"
               "DesktopNames=sway;wlroots\n"
               "Type=Application\n");
    snprintf(path, sizeof(path), "%s/sway.desktop", system_dir);
    write_text(path,
               "[Desktop Entry]\n"
               "Name=Sway System\n"
               "Exec=sway\n"
               "Type=Application\n");
    snprintf(path, sizeof(path), "%s/niri.desktop", system_dir);
    write_text(path,
               "[Desktop Entry]\n"
               "Name=Niri\n"
               "Exec=dbus-run-session niri --session\n"
               "Type=Application\n");
    snprintf(path, sizeof(path), "%s/hidden.desktop", system_dir);
    write_text(path,
               "[Desktop Entry]\n"
               "Name=Hidden\n"
               "Exec=hidden\n"
               "Hidden=true\n");

    char dirs[1024];
    snprintf(dirs, sizeof(dirs), "%s:%s", local, system_dir);
    setenv("RX_GREETER_SESSION_DIRS", dirs, 1);

    SessionList list;
    sessions_load(&list);
    EXPECT(list.count == 2);
    EXPECT(strcmp(list.items[0].id, "sway") == 0);
    EXPECT(strcmp(list.items[0].name, "Sway Local") == 0);
    EXPECT(strcmp(list.items[1].id, "niri") == 0);
    EXPECT(sessions_default_index(&list, "sway") == 0);
    EXPECT(sessions_default_index(&list, "niri") == 1);
    EXPECT(sessions_default_index(&list, "missing") == 0);
    EXPECT(list.items[0].argc == 1);
    EXPECT(strcmp(list.items[0].argv[0],
                  "/home/ranxom/.local/bin/sway-session") == 0);
    EXPECT(list.items[1].argc == 3);
    EXPECT(strcmp(list.items[1].argv[0], "dbus-run-session") == 0);

    size_t env_count = 0;
    char **env = session_build_env(&list.items[0], &env_count);
    EXPECT(env_count == 5);
    bool has_type = false;
    bool has_desktop = false;
    for (size_t i = 0; i < env_count; i++) {
        if (strcmp(env[i], "XDG_SESSION_TYPE=wayland") == 0) {
            has_type = true;
        }
        if (strcmp(env[i], "XDG_CURRENT_DESKTOP=sway:wlroots") == 0) {
            has_desktop = true;
        }
    }
    EXPECT(has_type);
    EXPECT(has_desktop);
    session_free_env(env, env_count);

    sessions_free(&list);
    unsetenv("RX_GREETER_SESSION_DIRS");
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    EXPECT(system(cmd) == 0);
}

static void
test_password(void)
{
    PasswordBuffer buffer;
    password_init(&buffer);
    EXPECT(password_is_empty(&buffer));
    EXPECT(password_append(&buffer, "p\xc3\xa4ss", 5));
    EXPECT(buffer.len == 5);
    EXPECT(password_codepoints(&buffer) == 4);
    EXPECT(password_backspace(&buffer));
    EXPECT(strcmp(buffer.data, "p\xc3\xa4s") == 0);
    EXPECT(password_backspace(&buffer));
    EXPECT(strcmp(buffer.data, "p\xc3\xa4") == 0);
    EXPECT(password_backspace(&buffer));
    EXPECT(strcmp(buffer.data, "p") == 0);
    EXPECT(password_backspace(&buffer));
    EXPECT(password_is_empty(&buffer));
    EXPECT(!password_backspace(&buffer));

    password_append(&buffer, "secret", 6);
    password_clear(&buffer);
    EXPECT(buffer.len == 0);
    for (size_t i = 0; i < sizeof(buffer.data); i++) {
        EXPECT(buffer.data[i] == 0);
    }
}

static void
test_greetd_parser(void)
{
    GdEvent event;
    char error[256];

    EXPECT(gd_parse_response("{\"type\":\"success\"}", &event, error,
                             sizeof(error)));
    EXPECT(event.type == GD_EVENT_SUCCESS);

    EXPECT(gd_parse_response(
        "{\"type\":\"error\",\"error_type\":\"auth_error\","
        "\"description\":\"nope\"}",
        &event, error, sizeof(error)));
    EXPECT(event.type == GD_EVENT_ERROR);
    EXPECT(strcmp(event.error_type, "auth_error") == 0);
    EXPECT(strcmp(event.description, "nope") == 0);

    EXPECT(gd_parse_response(
        "{\"type\":\"auth_message\",\"auth_message_type\":\"secret\","
        "\"auth_message\":\"Password:\"}",
        &event, error, sizeof(error)));
    EXPECT(event.type == GD_EVENT_AUTH_MESSAGE);
    EXPECT(strcmp(event.auth_message_type, "secret") == 0);
    EXPECT(strcmp(event.auth_message, "Password:") == 0);

    EXPECT(!gd_parse_response("{\"type\":\"nonsense\"}", &event, error,
                              sizeof(error)));
    EXPECT(!gd_parse_response("not json", &event, error, sizeof(error)));
}

static void
test_user_filters(void)
{
    EXPECT(user_uid_allowed(1000));
    EXPECT(user_uid_allowed(65533));
    EXPECT(!user_uid_allowed(999));
    EXPECT(!user_uid_allowed(65534));
    EXPECT(user_shell_allowed("/bin/bash"));
    EXPECT(user_shell_allowed("/usr/bin/zsh"));
    EXPECT(!user_shell_allowed("/sbin/nologin"));
    EXPECT(!user_shell_allowed("/usr/sbin/nologin"));
    EXPECT(!user_shell_allowed("/bin/false"));
    EXPECT(!user_shell_allowed(""));
}

int
main(void)
{
    test_colors();
    test_config();
    test_exec_parser();
    test_sessions();
    test_password();
    test_greetd_parser();
    test_user_filters();

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    puts("all unit tests passed");
    return 0;
}
