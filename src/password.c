#include "password.h"

#include "util.h"

#include <string.h>

void
password_init(PasswordBuffer *buffer)
{
    secure_clear(buffer->data, sizeof(buffer->data));
    buffer->len = 0;
}

void
password_clear(PasswordBuffer *buffer)
{
    secure_clear(buffer->data, sizeof(buffer->data));
    buffer->len = 0;
}

bool
password_append(PasswordBuffer *buffer, const char *text, size_t len)
{
    if (!text || len == 0) {
        return true;
    }
    if (len > RX_PASSWORD_MAX - buffer->len) {
        return false;
    }
    memcpy(buffer->data + buffer->len, text, len);
    buffer->len += len;
    buffer->data[buffer->len] = '\0';
    return true;
}

bool
password_backspace(PasswordBuffer *buffer)
{
    if (buffer->len == 0) {
        return false;
    }
    size_t start = utf8_prev_codepoint(buffer->data, buffer->len);
    secure_clear(buffer->data + start, buffer->len - start);
    buffer->len = start;
    buffer->data[buffer->len] = '\0';
    return true;
}

size_t
password_codepoints(const PasswordBuffer *buffer)
{
    return utf8_codepoint_count(buffer->data, buffer->len);
}

bool
password_is_empty(const PasswordBuffer *buffer)
{
    return buffer->len == 0;
}
