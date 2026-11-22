
#include "sessions.h"

#include "util.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <syslog.h>

struct string_list {
    char **items;
    size_t count;
    size_t capacity;
};

static void
string_list_add(struct string_list *list, const char *value)
{
    if (list->count == list->capacity) {
        list->capacity = list->capacity ? list->capacity * 2 : 8;
        list->items = xrealloc(list->items, list->capacity * sizeof(*list->items));
    }
    list->items[list->count++] = xstrdup(value);
}

static void
string_list_free(struct string_list *list)
{
    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static int
compare_strings(const void *a, const void *b)
{
    const char *const *sa = a;
    const char *const *sb = b;
    return strcmp(*sa, *sb);
}

static void
add_search_dirs(struct string_list *dirs, const char *dirs_override)
{
    const char *override = (dirs_override && *dirs_override)
                               ? dirs_override
                               : getenv("RX_GREETER_SESSION_DIRS");
    if (override && *override) {
        char *copy = xstrdup(override);
        char *save = NULL;
        for (char *part = strtok_r(copy, ":", &save); part;
             part = strtok_r(NULL, ":", &save)) {
            if (*part) {
                string_list_add(dirs, part);
            }
        }
        free(copy);
        return;
    }
    string_list_add(dirs, "/usr/local/share/wayland-sessions");
    string_list_add(dirs, "/usr/share/wayland-sessions");
}

static char *
desktop_unescape(const char *value)
{
    size_t len = strlen(value);
    char *out = xmalloc(len + 1);
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (value[i] == '\\' && i + 1 < len) {
            char next = value[i + 1];
            switch (next) {
            case 's':
                out[j++] = ' ';
                i++;
                break;
            case 'n':
                out[j++] = '\n';
                i++;
                break;
            case 't':
                out[j++] = '\t';
                i++;
                break;
            case 'r':
                out[j++] = '\r';
                i++;
                break;
            case '\\':
                out[j++] = '\\';
                i++;
                break;
            default:
                out[j++] = value[i];
                break;
            }
        } else {
            out[j++] = value[i];
        }
    }
    out[j] = '\0';
    return out;
}

static bool
parse_desktop_file(const char *path, Session *session)
{
    char *data = read_file(path, 256 * 1024, NULL);
    if (!data) {
        return false;
    }

    char *name = NULL;
    char *exec = NULL;
    char *comment = NULL;
    char *icon = NULL;
    char *try_exec = NULL;
    char *desktop_names = NULL;
    bool hidden = false;
    bool no_display = false;
    bool in_desktop_entry = false;

    char *save = NULL;
    for (char *line = strtok_r(data, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        size_t len = strlen(line);
        if (len && line[len - 1] == '\r') {
            line[len - 1] = '\0';
        }
        char *text = trim_whitespace(line);
        if (*text == '\0' || *text == '#') {
            continue;
        }
        if (*text == '[') {
            in_desktop_entry = strcmp(text, "[Desktop Entry]") == 0;
            continue;
        }
        if (!in_desktop_entry) {
            continue;
        }
        char *eq = strchr(text, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *key = trim_whitespace(text);
        char *value = trim_whitespace(eq + 1);
        char *unescaped = desktop_unescape(value);

        if (strcmp(key, "Name") == 0) {
            free(name);
            name = unescaped;
        } else if (strcmp(key, "Exec") == 0) {
            free(exec);
            exec = unescaped;
        } else if (strcmp(key, "Comment") == 0) {
            free(comment);
            comment = unescaped;
        } else if (strcmp(key, "Icon") == 0) {
            free(icon);
            icon = unescaped;
        } else if (strcmp(key, "TryExec") == 0) {
            free(try_exec);
            try_exec = unescaped;
        } else if (strcmp(key, "DesktopNames") == 0) {
            free(desktop_names);
            desktop_names = unescaped;
        } else if (strcmp(key, "Hidden") == 0) {
            free(unescaped);
            hidden = str_ieq(value, "true");
        } else if (strcmp(key, "NoDisplay") == 0) {
            free(unescaped);
            no_display = str_ieq(value, "true");
        } else {
            free(unescaped);
        }
    }
    free(data);

    if (hidden || no_display || !exec || !*exec) {
        free(name);
        free(exec);
        free(comment);
        free(icon);
        free(try_exec);
        free(desktop_names);
        return false;
    }
    if (try_exec && *try_exec && !find_in_path(try_exec, NULL, 0)) {
        rx_log(LOG_INFO, "skipping session %s: TryExec %s was not found", path,
               try_exec);
        free(name);
        free(exec);
        free(comment);
        free(icon);
        free(try_exec);
        free(desktop_names);
        return false;
    }

    const char *id = path_basename(path);
    size_t id_len = strlen(id);
    char *id_copy = xstrndup(id, str_has_suffix(id, ".desktop") ? id_len - 8 : id_len);

    if (!name || !*name) {
        free(name);
        name = xstrdup(id_copy);
    }

    char *name_unescaped = xstrdup(name);
    char **argv = NULL;
    size_t argc = 0;
    bool parsed = desktop_parse_exec(exec, name_unescaped, icon ? icon : "",
                                     path, &argv, &argc);
    free(name_unescaped);
    if (!parsed || argc == 0) {
        rx_log(LOG_WARNING, "skipping session %s: invalid Exec line", path);
        free(id_copy);
        free(name);
        free(exec);
        free(comment);
        free(icon);
        free(try_exec);
        free(desktop_names);
        return false;
    }

    session->id = id_copy;
    session->name = name;
    session->comment = comment;
    session->exec = exec;
    session->desktop_names = desktop_names;
    session->path = xstrdup(path);
    session->argv = argv;
    session->argc = argc;
    free(icon);
    free(try_exec);
    return true;
}

static void
load_dir(SessionList *list, const char *dir)
{
    DIR *handle = opendir(dir);
    if (!handle) {
        return;
    }

    struct string_list names = { 0 };
    struct dirent *entry = NULL;
    while ((entry = readdir(handle)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        if (!str_has_suffix(entry->d_name, ".desktop")) {
            continue;
        }
        string_list_add(&names, entry->d_name);
    }
    closedir(handle);
    qsort(names.items, names.count, sizeof(*names.items), compare_strings);

    for (size_t i = 0; i < names.count; i++) {
        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", dir, names.items[i]);

        const char *id = names.items[i];
        size_t id_len = strlen(id);
        char *id_copy = xstrndup(id, id_len - 8);
        bool duplicate = false;
        for (size_t j = 0; j < list->count; j++) {
            if (strcmp(list->items[j].id, id_copy) == 0) {
                duplicate = true;
                break;
            }
        }
        free(id_copy);
        if (duplicate) {
            continue;
        }

        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }

        Session session = { 0 };
        if (parse_desktop_file(path, &session)) {
            list->items = xrealloc(list->items,
                                   (list->count + 1) * sizeof(*list->items));
            list->items[list->count++] = session;
        }
    }
    string_list_free(&names);
}

void
sessions_load(SessionList *list, const char *dirs_override)
{
    list->items = NULL;
    list->count = 0;
    struct string_list dirs = { 0 };
    add_search_dirs(&dirs, dirs_override);
    for (size_t i = 0; i < dirs.count; i++) {
        load_dir(list, dirs.items[i]);
    }
    string_list_free(&dirs);
}

void
sessions_free(SessionList *list)
{
    if (!list) {
        return;
    }
    for (size_t i = 0; i < list->count; i++) {
        Session *session = &list->items[i];
        free(session->id);
        free(session->name);
        free(session->comment);
        free(session->exec);
        free(session->desktop_names);
        free(session->path);
        for (size_t j = 0; j < session->argc; j++) {
            free(session->argv[j]);
        }
        free(session->argv);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
}

int
sessions_default_index(const SessionList *list, const char *preferred)
{
    if (!list || list->count == 0) {
        return -1;
    }
    if (!preferred || !*preferred) {
        return 0;
    }
    for (size_t i = 0; i < list->count; i++) {
        const Session *session = &list->items[i];
        if (str_ieq(session->id, preferred) ||
            str_ieq(session->name, preferred) ||
            str_ieq(path_basename(session->path), preferred)) {
            return (int)i;
        }
    }
    return 0;
}

char **
session_build_env(const Session *session, size_t *out_count)
{
    const char *desktop = session->desktop_names && *session->desktop_names
                              ? session->desktop_names
                              : session->id;
    char current[2048];
    if (strchr(desktop, ';')) {
        size_t j = 0;
        for (size_t i = 0; desktop[i] && j + 1 < sizeof(current); i++) {
            current[j++] = desktop[i] == ';' ? ':' : desktop[i];
        }
        current[j] = '\0';
    } else {
        snprintf(current, sizeof(current), "%s", desktop);
    }

    char session_type[256];
    snprintf(session_type, sizeof(session_type), "XDG_SESSION_TYPE=wayland");
    char session_class[256];
    snprintf(session_class, sizeof(session_class), "XDG_SESSION_CLASS=user");
    char session_desktop[512];
    snprintf(session_desktop, sizeof(session_desktop), "XDG_SESSION_DESKTOP=%s",
             session->id);
    char current_desktop[4096];
    snprintf(current_desktop, sizeof(current_desktop), "XDG_CURRENT_DESKTOP=%.2047s",
             current);
    char desktop_session[512];
    snprintf(desktop_session, sizeof(desktop_session), "DESKTOP_SESSION=%s",
             session->id);

    char **env = xcalloc(5, sizeof(*env));
    env[0] = xstrdup(session_type);
    env[1] = xstrdup(session_class);
    env[2] = xstrdup(session_desktop);
    env[3] = xstrdup(current_desktop);
    env[4] = xstrdup(desktop_session);
    *out_count = 5;
    return env;
}

void
session_free_env(char **env, size_t count)
{
    if (!env) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free(env[i]);
    }
    free(env);
}

static void
argv_add(char ***argv, size_t *argc, size_t *capacity, const char *value)
{
    if (*argc == *capacity) {
        *capacity = *capacity ? *capacity * 2 : 8;
        *argv = xrealloc(*argv, *capacity * sizeof(**argv));
    }
    (*argv)[(*argc)++] = xstrdup(value);
}

static void
token_add(char ***argv, size_t *argc, size_t *capacity, char **token,
          size_t *token_len, size_t *token_cap, bool *has_token)
{
    if (!*has_token && *token_len == 0) {
        return;
    }
    if (*token_len == *token_cap) {
        *token_cap = *token_cap ? *token_cap * 2 : 32;
        *token = xrealloc(*token, *token_cap);
    }
    (*token)[*token_len] = '\0';
    argv_add(argv, argc, capacity, *token);
    *token_len = 0;
    *has_token = false;
}

static void
token_char(char **token, size_t *token_len, size_t *token_cap, char c,
           bool *has_token)
{
    if (*token_len + 1 >= *token_cap) {
        *token_cap = *token_cap ? *token_cap * 2 : 32;
        *token = xrealloc(*token, *token_cap);
    }
    (*token)[(*token_len)++] = c;
    *has_token = true;
}

bool
desktop_parse_exec(const char *exec, const char *name, const char *icon,
                   const char *desktop_path, char ***out_argv,
                   size_t *out_argc)
{
    if (!exec) {
        return false;
    }

    char **argv = NULL;
    size_t argc = 0;
    size_t capacity = 0;
    char *token = NULL;
    size_t token_len = 0;
    size_t token_cap = 0;
    bool has_token = false;
    bool ok = true;

    const char *p = exec;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }
        if (!*p) {
            break;
        }

        while (*p) {
            if (isspace((unsigned char)*p)) {
                break;
            }
            if (*p == '\'') {
                p++;
                has_token = true;
                while (*p && *p != '\'') {
                    token_char(&token, &token_len, &token_cap, *p++, &has_token);
                }
                if (*p != '\'') {
                    ok = false;
                    goto done;
                }
                p++;
            } else if (*p == '"') {
                p++;
                has_token = true;
                while (*p && *p != '"') {
                    if (*p == '\\' && p[1] &&
                        (p[1] == '"' || p[1] == '`' || p[1] == '$' || p[1] == '\\')) {
                        p++;
                    }
                    token_char(&token, &token_len, &token_cap, *p++, &has_token);
                }
                if (*p != '"') {
                    ok = false;
                    goto done;
                }
                p++;
            } else if (*p == '\\') {
                p++;
                if (*p) {
                    token_char(&token, &token_len, &token_cap, *p++, &has_token);
                }
            } else if (*p == '%') {
                p++;
                char code = *p;
                if (!code) {
                    break;
                }
                p++;
                if (code == '%') {
                    token_char(&token, &token_len, &token_cap, '%', &has_token);
                } else if (code == 'c') {
                    for (const char *q = name ? name : ""; *q; q++) {
                        token_char(&token, &token_len, &token_cap, *q, &has_token);
                    }
                } else if (code == 'k') {
                    for (const char *q = desktop_path ? desktop_path : ""; *q; q++) {
                        token_char(&token, &token_len, &token_cap, *q, &has_token);
                    }
                } else if (code == 'i') {
                    token_add(&argv, &argc, &capacity, &token, &token_len,
                              &token_cap, &has_token);
                    if (icon && *icon) {
                        argv_add(&argv, &argc, &capacity, "--icon");
                        argv_add(&argv, &argc, &capacity, icon);
                    }
                } else {
                    /* %f, %F, %u, %U, %d, %D, %n, %N, %v, %m: no argv value. */
                }
            } else {
                token_char(&token, &token_len, &token_cap, *p++, &has_token);
            }
        }

        token_add(&argv, &argc, &capacity, &token, &token_len, &token_cap,
                  &has_token);
    }

done:
    free(token);
    if (!ok || argc == 0) {
        for (size_t i = 0; i < argc; i++) {
            free(argv[i]);
        }
        free(argv);
        return false;
    }
    *out_argv = argv;
    *out_argc = argc;
    return true;
}
