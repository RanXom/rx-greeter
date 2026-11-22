#ifndef RX_GREETER_USERS_H
#define RX_GREETER_USERS_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

typedef struct {
    char *name;
    uid_t uid;
    char *shell;
} User;

typedef struct {
    User *items;
    size_t count;
} UserList;

void users_load(UserList *list);
void users_free(UserList *list);
int users_find(const UserList *list, const char *name);

bool user_uid_allowed(uid_t uid);
bool user_shell_allowed(const char *shell);

#endif
