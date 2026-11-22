#ifndef RX_GREETER_UTIL_H
#define RX_GREETER_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void *xmalloc(size_t size);
void *xcalloc(size_t count, size_t size);
void *xrealloc(void *ptr, size_t size);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

char *trim_whitespace(char *s);
void strip_matching_quotes(char *s);
bool parse_bool(const char *s, bool *out);
bool parse_double(const char *s, double *out);
bool parse_long(const char *s, long *out);

bool str_ieq(const char *a, const char *b);
bool str_has_suffix(const char *s, const char *suffix);
const char *path_basename(const char *path);
char *path_dirname_dup(const char *path);
char *read_file(const char *path, size_t max_size, size_t *out_size);
bool path_is_readable(const char *path);
bool path_is_executable(const char *path);
char *find_sibling_executable(const char *argv0, const char *name);

int64_t now_ms(void);
void secure_clear(void *ptr, size_t len);

/* UTF-8 helpers. These do not validate sequences beyond structural bounds. */
size_t utf8_codepoint_count(const char *s, size_t len);
size_t utf8_prev_codepoint(const char *s, size_t len);
size_t utf8_next_codepoint(const char *s, size_t len);

#endif
