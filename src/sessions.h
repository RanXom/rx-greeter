#ifndef RX_GREETER_SESSIONS_H
#define RX_GREETER_SESSIONS_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char *id;
    char *name;
    char *comment;
    char *exec;
    char *desktop_names;
    char *path;
    char **argv;
    size_t argc;
} Session;

typedef struct {
    Session *items;
    size_t count;
} SessionList;

void sessions_load(SessionList *list);
void sessions_free(SessionList *list);
int sessions_default_index(const SessionList *list, const char *preferred);

char **session_build_env(const Session *session, size_t *out_count);
void session_free_env(char **env, size_t count);

bool desktop_parse_exec(const char *exec, const char *name, const char *icon,
                        const char *desktop_path, char ***out_argv,
                        size_t *out_argc);

#endif
