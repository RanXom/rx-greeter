
#include "greetd.h"

#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static void
set_error(char *error, size_t error_len, const char *fmt, ...)
{
    if (!error || error_len == 0) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vsnprintf(error, error_len, fmt, args);
    va_end(args);
}

void
gd_init(GdConn *conn)
{
    memset(conn, 0, sizeof(*conn));
    conn->fd = -1;
}

void
gd_close(GdConn *conn)
{
    if (!conn) {
        return;
    }
    if (conn->fd >= 0) {
        close(conn->fd);
    }
    if (conn->payload) {
        secure_clear(conn->payload, conn->payload_len);
        free(conn->payload);
    }
    secure_clear(conn, sizeof(*conn));
    conn->fd = -1;
}

int
gd_fd(const GdConn *conn)
{
    return conn ? conn->fd : -1;
}

bool
gd_connect(GdConn *conn, const char *socket_path, char *error, size_t error_len)
{
    gd_close(conn);
    if (!socket_path || !*socket_path) {
        set_error(error, error_len, "GREETD_SOCK is not set");
        return false;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(socket_path) >= sizeof(addr.sun_path)) {
        set_error(error, error_len, "greetd socket path is too long");
        return false;
    }
    strcpy(addr.sun_path, socket_path);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        set_error(error, error_len, "socket(): %s", strerror(errno));
        return false;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        set_error(error, error_len, "cannot connect to %s: %s", socket_path,
                  strerror(errno));
        close(fd);
        return false;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        set_error(error, error_len, "cannot make greetd socket nonblocking: %s",
                  strerror(errno));
        close(fd);
        return false;
    }
    conn->fd = fd;
    return true;
}

static bool
write_all(GdConn *conn, const unsigned char *data, size_t len,
          char *error, size_t error_len)
{
    size_t offset = 0;
    while (offset < len) {
        ssize_t written = send(conn->fd, data + offset, len - offset, MSG_NOSIGNAL);
        if (written > 0) {
            offset += (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd = { .fd = conn->fd, .events = POLLOUT };
            int rc = poll(&pfd, 1, 5000);
            if (rc > 0) {
                continue;
            }
            set_error(error, error_len, "timed out writing to greetd");
            return false;
        }
        set_error(error, error_len, "write to greetd failed: %s", strerror(errno));
        return false;
    }
    return true;
}

static bool
send_json(GdConn *conn, struct json_object *object, char *error, size_t error_len)
{
    if (!conn || conn->fd < 0) {
        set_error(error, error_len, "not connected to greetd");
        return false;
    }
    const char *payload = json_object_to_json_string_ext(
        object, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE);
    size_t payload_len = strlen(payload);

    unsigned char *frame = xmalloc(payload_len + 4);
    uint32_t native_len = (uint32_t)payload_len;
    memcpy(frame, &native_len, sizeof(native_len));
    memcpy(frame + sizeof(native_len), payload, payload_len);

    bool ok = write_all(conn, frame, payload_len + 4, error, error_len);
    secure_clear(frame, payload_len + 4);
    free(frame);
    /* json-c owns the serialized payload. It can contain a password, so do
     * not leave it lying around after the request has been written. */
    secure_clear((void *)payload, payload_len);
    return ok;
}

bool
gd_send_create(GdConn *conn, const char *username, char *error, size_t error_len)
{
    struct json_object *object = json_object_new_object();
    json_object_object_add(object, "type", json_object_new_string("create_session"));
    json_object_object_add(object, "username", json_object_new_string(username));
    bool ok = send_json(conn, object, error, error_len);
    json_object_put(object);
    return ok;
}

bool
gd_send_post(GdConn *conn, const char *response, char *error, size_t error_len)
{
    struct json_object *object = json_object_new_object();
    json_object_object_add(object, "type",
                           json_object_new_string("post_auth_message_response"));
    struct json_object *response_object = NULL;
    if (response) {
        response_object = json_object_new_string(response);
        json_object_object_add(object, "response", response_object);
    }
    bool ok = send_json(conn, object, error, error_len);
    if (response_object) {
        char *value = (char *)json_object_get_string(response_object);
        if (value) {
            secure_clear(value, strlen(value));
        }
    }
    json_object_put(object);
    return ok;
}

bool
gd_send_start(GdConn *conn, char *const argv[], size_t argc,
              char *const env[], size_t envc, char *error, size_t error_len)
{
    struct json_object *object = json_object_new_object();
    json_object_object_add(object, "type", json_object_new_string("start_session"));

    struct json_object *cmd = json_object_new_array_ext((int)argc);
    for (size_t i = 0; i < argc; i++) {
        json_object_array_add(cmd, json_object_new_string(argv[i]));
    }
    json_object_object_add(object, "cmd", cmd);

    struct json_object *env_object = json_object_new_array_ext((int)envc);
    for (size_t i = 0; i < envc; i++) {
        json_object_array_add(env_object, json_object_new_string(env[i]));
    }
    json_object_object_add(object, "env", env_object);

    bool ok = send_json(conn, object, error, error_len);
    json_object_put(object);
    return ok;
}

bool
gd_send_cancel(GdConn *conn, char *error, size_t error_len)
{
    struct json_object *object = json_object_new_object();
    json_object_object_add(object, "type", json_object_new_string("cancel_session"));
    bool ok = send_json(conn, object, error, error_len);
    json_object_put(object);
    return ok;
}

void
gd_cancel_and_close(GdConn *conn)
{
    if (!conn || conn->fd < 0) {
        return;
    }
    char error[256];
    if (!gd_send_cancel(conn, error, sizeof(error))) {
        gd_close(conn);
        return;
    }
    /* Wait briefly for greetd to acknowledge the cancellation. The reply is
     * what guarantees the next create_session cannot race the old one. */
    int64_t deadline = now_ms() + 250;
    for (;;) {
        int64_t remaining = deadline - now_ms();
        if (remaining <= 0) {
            break;
        }
        struct pollfd pfd = { .fd = conn->fd, .events = POLLIN };
        int rc = poll(&pfd, 1, (int)remaining);
        if (rc <= 0) {
            break;
        }
        GdEvent event;
        int read = gd_read(conn, &event);
        if (read < 0) {
            break;
        }
        if (read == 0) {
            continue;
        }
        if (event.type == GD_EVENT_SUCCESS || event.type == GD_EVENT_ERROR) {
            break;
        }
    }
    gd_close(conn);
}

static void
copy_string(char *dest, size_t dest_len, const char *src)
{
    if (!dest || dest_len == 0) {
        return;
    }
    if (!src) {
        dest[0] = '\0';
        return;
    }
    snprintf(dest, dest_len, "%s", src);
}

static const char *
json_string_field(struct json_object *object, const char *key,
                  const char *fallback)
{
    struct json_object *value = NULL;
    if (json_object_object_get_ex(object, key, &value) &&
        json_object_is_type(value, json_type_string)) {
        return json_object_get_string(value);
    }
    return fallback;
}

bool
gd_parse_response(const char *json, GdEvent *event, char *error, size_t error_len)
{
    if (!json || !event) {
        set_error(error, error_len, "empty greetd response");
        return false;
    }
    struct json_object *object = json_tokener_parse(json);
    if (!object || !json_object_is_type(object, json_type_object)) {
        if (object) {
            json_object_put(object);
        }
        set_error(error, error_len, "greetd returned invalid JSON");
        return false;
    }

    memset(event, 0, sizeof(*event));
    struct json_object *type_object = NULL;
    if (!json_object_object_get_ex(object, "type", &type_object) ||
        !json_object_is_type(type_object, json_type_string)) {
        json_object_put(object);
        set_error(error, error_len, "greetd response has no type");
        return false;
    }
    const char *type = json_object_get_string(type_object);

    if (strcmp(type, "success") == 0) {
        event->type = GD_EVENT_SUCCESS;
    } else if (strcmp(type, "error") == 0) {
        event->type = GD_EVENT_ERROR;
        copy_string(event->error_type, sizeof(event->error_type),
                    json_string_field(object, "error_type", "error"));
        copy_string(event->description, sizeof(event->description),
                    json_string_field(object, "description", ""));
    } else if (strcmp(type, "auth_message") == 0) {
        event->type = GD_EVENT_AUTH_MESSAGE;
        copy_string(event->auth_message_type, sizeof(event->auth_message_type),
                    json_string_field(object, "auth_message_type", "info"));
        copy_string(event->auth_message, sizeof(event->auth_message),
                    json_string_field(object, "auth_message", ""));
    } else {
        char unknown[64];
        snprintf(unknown, sizeof(unknown), "%s", type);
        json_object_put(object);
        set_error(error, error_len, "unknown greetd response type '%s'", unknown);
        return false;
    }

    json_object_put(object);
    return true;
}

static int
transport_error(GdConn *conn, GdEvent *event, const char *message)
{
    gd_close(conn);
    if (event) {
        memset(event, 0, sizeof(*event));
        event->type = GD_EVENT_ERROR;
        snprintf(event->error_type, sizeof(event->error_type), "transport");
        snprintf(event->description, sizeof(event->description), "%s", message);
    }
    return -1;
}

int
gd_read(GdConn *conn, GdEvent *event)
{
    if (!conn || conn->fd < 0) {
        if (event) {
            memset(event, 0, sizeof(*event));
            event->type = GD_EVENT_ERROR;
            snprintf(event->error_type, sizeof(event->error_type), "transport");
            snprintf(event->description, sizeof(event->description),
                     "not connected to greetd");
        }
        return -1;
    }

    for (;;) {
        if (conn->header_got < sizeof(conn->header)) {
            ssize_t got = recv(conn->fd, conn->header + conn->header_got,
                               sizeof(conn->header) - conn->header_got, 0);
            if (got > 0) {
                conn->header_got += (size_t)got;
                continue;
            }
            if (got == 0) {
                return transport_error(conn, event, "greetd closed the connection");
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }
            return transport_error(conn, event, "read from greetd failed");
        }

        if (!conn->payload) {
            uint32_t payload_len = 0;
            memcpy(&payload_len, conn->header, sizeof(payload_len));
            if (payload_len == 0 || payload_len > GD_MESSAGE_MAX) {
                return transport_error(conn, event, "invalid greetd message size");
            }
            conn->payload_len = payload_len;
            conn->payload_got = 0;
            conn->payload = xmalloc(payload_len + 1);
        }

        if (conn->payload_got < conn->payload_len) {
            ssize_t got = recv(conn->fd, conn->payload + conn->payload_got,
                               conn->payload_len - conn->payload_got, 0);
            if (got > 0) {
                conn->payload_got += (size_t)got;
                continue;
            }
            if (got == 0) {
                return transport_error(conn, event, "greetd closed the connection");
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }
            return transport_error(conn, event, "read from greetd failed");
        }

        conn->payload[conn->payload_len] = '\0';
        char parse_error[256];
        bool parsed = gd_parse_response(conn->payload, event, parse_error,
                                        sizeof(parse_error));
        secure_clear(conn->payload, conn->payload_len);
        free(conn->payload);
        conn->payload = NULL;
        conn->payload_len = 0;
        conn->payload_got = 0;
        conn->header_got = 0;

        if (!parsed) {
            return transport_error(conn, event, parse_error);
        }
        return 1;
    }
}
