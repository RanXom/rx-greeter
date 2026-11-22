#ifndef RX_GREETER_PASSWORD_H
#define RX_GREETER_PASSWORD_H

#include <stdbool.h>
#include <stddef.h>

#define RX_PASSWORD_MAX 1024

typedef struct {
    char data[RX_PASSWORD_MAX + 1];
    size_t len;
} PasswordBuffer;

void password_init(PasswordBuffer *buffer);
void password_clear(PasswordBuffer *buffer);
bool password_append(PasswordBuffer *buffer, const char *text, size_t len);
bool password_backspace(PasswordBuffer *buffer);
size_t password_codepoints(const PasswordBuffer *buffer);
bool password_is_empty(const PasswordBuffer *buffer);

#endif
