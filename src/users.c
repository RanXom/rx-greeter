#include "users.h"

#include "util.h"

#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

bool
user_uid_allowed(uid_t uid)
{
    return uid >= 1000 && uid < 65534;
}

bool
user_shell_allowed(const char *shell)
{
    if (!shell || !*shell) {
        return false;
    }
    const char *base = path_basename(shell);
    return !str_ieq(base, "nologin") && !str_ieq(base, "false");
}

static int
compare_users(const void *a, const void *b)
{
    const User *ua = a;
    const User *ub = b;
    return strcmp(ua->name, ub->name);
}

void
users_load(UserList *list)
{
    list->items = NULL;
    list->count = 0;

    size_t capacity = 8;
    list->items = xcalloc(capacity, sizeof(*list->items));

    setpwent();
    struct passwd *pw = NULL;
    while ((pw = getpwent()) != NULL) {
        if (!user_uid_allowed(pw->pw_uid) || !user_shell_allowed(pw->pw_shell)) {
            continue;
        }
        if (list->count == capacity) {
            capacity *= 2;
            list->items = xrealloc(list->items, capacity * sizeof(*list->items));
        }
        User *user = &list->items[list->count++];
        user->name = xstrdup(pw->pw_name);
        user->uid = pw->pw_uid;
        user->shell = xstrdup(pw->pw_shell);
    }
    endpwent();

    if (list->count > 1) {
        qsort(list->items, list->count, sizeof(*list->items), compare_users);
    }
}

void
users_free(UserList *list)
{
    if (!list) {
        return;
    }
    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i].name);
        free(list->items[i].shell);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
}

int
users_find(const UserList *list, const char *name)
{
    if (!list || !name) {
        return -1;
    }
    for (size_t i = 0; i < list->count; i++) {
        if (strcmp(list->items[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}
