
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

void *
xmalloc(size_t size)
{
    void *ptr = malloc(size ? size : 1);
    if (!ptr) {
        fputs("rx-greeter: out of memory\n", stderr);
        abort();
    }
    return ptr;
}

void *
xcalloc(size_t count, size_t size)
{
    void *ptr = calloc(count ? count : 1, size ? size : 1);
    if (!ptr) {
        fputs("rx-greeter: out of memory\n", stderr);
        abort();
    }
    return ptr;
}

void *
xrealloc(void *ptr, size_t size)
{
    void *next = realloc(ptr, size ? size : 1);
    if (!next) {
        fputs("rx-greeter: out of memory\n", stderr);
        abort();
    }
    return next;
}

char *
xstrdup(const char *s)
{
    if (!s) {
        return NULL;
    }
    size_t len = strlen(s);
    char *copy = xmalloc(len + 1);
    memcpy(copy, s, len + 1);
    return copy;
}

char *
xstrndup(const char *s, size_t n)
{
    size_t len = strnlen(s, n);
    char *copy = xmalloc(len + 1);
    memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}

char *
trim_whitespace(char *s)
{
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';
    return s;
}

void
strip_matching_quotes(char *s)
{
    size_t len = strlen(s);
    if (len < 2) {
        return;
    }
    if ((s[0] == '"' && s[len - 1] == '"') ||
        (s[0] == '\'' && s[len - 1] == '\'')) {
        memmove(s, s + 1, len - 2);
        s[len - 2] = '\0';
    }
}

bool
parse_bool(const char *s, bool *out)
{
    if (str_ieq(s, "true") || str_ieq(s, "yes") || str_ieq(s, "on") ||
        strcmp(s, "1") == 0) {
        *out = true;
        return true;
    }
    if (str_ieq(s, "false") || str_ieq(s, "no") || str_ieq(s, "off") ||
        strcmp(s, "0") == 0) {
        *out = false;
        return true;
    }
    return false;
}

bool
parse_double(const char *s, double *out)
{
    if (!s || !*s) {
        return false;
    }
    errno = 0;
    char *end = NULL;
    double value = strtod(s, &end);
    if (errno != 0 || end == s) {
        return false;
    }
    while (*end && isspace((unsigned char)*end)) {
        end++;
    }
    if (*end != '\0') {
        return false;
    }
    *out = value;
    return true;
}

bool
parse_long(const char *s, long *out)
{
    if (!s || !*s) {
        return false;
    }
    errno = 0;
    char *end = NULL;
    long value = strtol(s, &end, 10);
    if (errno != 0 || end == s) {
        return false;
    }
    while (*end && isspace((unsigned char)*end)) {
        end++;
    }
    if (*end != '\0') {
        return false;
    }
    *out = value;
    return true;
}

bool
str_ieq(const char *a, const char *b)
{
    return a && b && strcasecmp(a, b) == 0;
}

bool
str_has_suffix(const char *s, const char *suffix)
{
    if (!s || !suffix) {
        return false;
    }
    size_t slen = strlen(s);
    size_t xlen = strlen(suffix);
    return slen >= xlen && memcmp(s + slen - xlen, suffix, xlen) == 0;
}

const char *
path_basename(const char *path)
{
    if (!path) {
        return "";
    }
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

char *
path_dirname_dup(const char *path)
{
    if (!path || !*path) {
        return xstrdup(".");
    }
    const char *slash = strrchr(path, '/');
    if (!slash) {
        return xstrdup(".");
    }
    if (slash == path) {
        return xstrdup("/");
    }
    return xstrndup(path, (size_t)(slash - path));
}

char *
read_file(const char *path, size_t max_size, size_t *out_size)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long end = ftell(file);
    if (end < 0 || (size_t)end > max_size) {
        fclose(file);
        return NULL;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    size_t size = (size_t)end;
    char *data = xmalloc(size + 1);
    size_t got = fread(data, 1, size, file);
    fclose(file);
    if (got != size) {
        free(data);
        return NULL;
    }
    data[size] = '\0';
    if (out_size) {
        *out_size = size;
    }
    return data;
}

bool
path_is_readable(const char *path)
{
    return path && access(path, R_OK) == 0;
}

bool
path_is_executable(const char *path)
{
    return path && access(path, X_OK) == 0;
}

char *
find_sibling_executable(const char *argv0, const char *name)
{
    char exe[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (len > 0) {
        exe[len] = '\0';
        char *dir = path_dirname_dup(exe);
        size_t need = strlen(dir) + strlen(name) + 2;
        char *candidate = xmalloc(need);
        snprintf(candidate, need, "%s/%s", dir, name);
        free(dir);
        if (path_is_executable(candidate)) {
            return candidate;
        }
        free(candidate);
    }

    if (argv0 && strchr(argv0, '/')) {
        char *dir = path_dirname_dup(argv0);
        size_t need = strlen(dir) + strlen(name) + 2;
        char *candidate = xmalloc(need);
        snprintf(candidate, need, "%s/%s", dir, name);
        free(dir);
        if (path_is_executable(candidate)) {
            return candidate;
        }
        free(candidate);
    }
    return NULL;
}

int64_t
now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void
secure_clear(void *ptr, size_t len)
{
    if (!ptr || !len) {
        return;
    }
    volatile unsigned char *p = ptr;
    while (len--) {
        *p++ = 0;
    }
}

size_t
utf8_codepoint_count(const char *s, size_t len)
{
    size_t count = 0;
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        size_t step = 1;
        if ((c & 0xe0) == 0xc0) {
            step = 2;
        } else if ((c & 0xf0) == 0xe0) {
            step = 3;
        } else if ((c & 0xf8) == 0xf0) {
            step = 4;
        }
        if (i + step > len) {
            step = 1;
        }
        i += step;
        count++;
    }
    return count;
}

size_t
utf8_prev_codepoint(const char *s, size_t len)
{
    if (!len) {
        return 0;
    }
    size_t i = len - 1;
    while (i > 0 && ((unsigned char)s[i] & 0xc0) == 0x80) {
        i--;
    }
    return i;
}

size_t
utf8_next_codepoint(const char *s, size_t len)
{
    if (!len) {
        return 0;
    }
    unsigned char c = (unsigned char)s[0];
    size_t step = 1;
    if ((c & 0xe0) == 0xc0) {
        step = 2;
    } else if ((c & 0xf0) == 0xe0) {
        step = 3;
    } else if ((c & 0xf8) == 0xf0) {
        step = 4;
    }
    return step > len ? 1 : step;
}
