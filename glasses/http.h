#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>
#include <sys/uio.h>

typedef struct {
    int fd;
    char method[8];
    char path[512];
    char query[512];
} http_req_t;

typedef void (*http_handler)(http_req_t *req);

/* Listen on port and run handler on a new thread per connection. Never returns
 * unless the socket can't be opened. The connection is closed after handler. */
int http_serve(int port, http_handler handler);

/* Copy query parameter `name` into buf; returns buf, or NULL if absent. */
const char *http_query(const http_req_t *req, const char *name, char *buf, size_t size);

/* Complete response with Content-Length. extra_headers may be NULL. */
int http_respond(http_req_t *req, int status, const char *ctype,
                 const void *body, size_t len, const char *extra_headers);
int http_respond_json(http_req_t *req, int status, const char *json);

/* Streaming response (Transfer-Encoding: chunked, like Flask's generators). */
int http_start_chunked(http_req_t *req, const char *ctype);
int http_send_chunk(http_req_t *req, const struct iovec *parts, int n);
int http_end_chunked(http_req_t *req);

#endif
