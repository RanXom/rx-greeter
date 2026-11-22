#ifndef RX_GREETER_GREETD_H
#define RX_GREETER_GREETD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GD_MESSAGE_MAX (1024U * 1024U)

typedef enum {
    GD_EVENT_NONE = 0,
    GD_EVENT_SUCCESS,
    GD_EVENT_ERROR,
    GD_EVENT_AUTH_MESSAGE,
} GdEventType;

typedef struct {
    GdEventType type;
    char auth_message_type[32];
    char auth_message[1024];
    char error_type[32];
    char description[1024];
} GdEvent;

typedef struct {
    int fd;
    unsigned char header[4];
    size_t header_got;
    char *payload;
    size_t payload_len;
    size_t payload_got;
} GdConn;

void gd_init(GdConn *conn);
void gd_close(GdConn *conn);
int gd_fd(const GdConn *conn);

bool gd_connect(GdConn *conn, const char *socket_path, char *error, size_t error_len);
bool gd_send_create(GdConn *conn, const char *username, char *error, size_t error_len);
bool gd_send_post(GdConn *conn, const char *response, char *error, size_t error_len);
bool gd_send_start(GdConn *conn, char *const argv[], size_t argc,
                   char *const env[], size_t envc, char *error, size_t error_len);
bool gd_send_cancel(GdConn *conn, char *error, size_t error_len);
void gd_cancel_and_close(GdConn *conn);

/* Returns 1 when an event was produced, 0 when more data is needed, -1 on
 * a transport or protocol failure (also reported as GD_EVENT_ERROR). */
int gd_read(GdConn *conn, GdEvent *event);

bool gd_parse_response(const char *json, GdEvent *event,
                       char *error, size_t error_len);

#endif
