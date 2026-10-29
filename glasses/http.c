/*
 * http.c - minimal HTTP/1.1 server: one request per connection, then close.
 *
 * That is all the iOS app needs, and it keeps the server tiny: no keep-alive,
 * no request bodies, no external libraries.
 */
#include "http.h"

#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define THREAD_STACK (256 * 1024)   /* default is 8 MB of address space per thread */

typedef struct {
    int fd;
    http_handler handler;
} conn_t;

static const char *status_text(int status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "BAD REQUEST";
    case 404: return "NOT FOUND";
    case 405: return "METHOD NOT ALLOWED";
    case 500: return "INTERNAL SERVER ERROR";
    case 503: return "SERVICE UNAVAILABLE";
    default:  return "";
    }
}

static int send_all(int fd, const void *data, size_t len)
{
    const char *p = data;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        p += n;
        len -= n;
    }
    return 0;
}

static int writev_all(int fd, struct iovec *iov, int n)
{
    while (n > 0) {
        ssize_t w = writev(fd, iov, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return -1;
        while (n > 0 && (size_t)w >= iov->iov_len) {
            w -= iov->iov_len;
            iov++;
            n--;
        }
        if (n > 0) {
            iov->iov_base = (char *)iov->iov_base + w;
            iov->iov_len -= w;
        }
    }
    return 0;
}

/* Read the request head and split "GET /path?query HTTP/1.1" */
static int parse_request(int fd, http_req_t *req)
{
    char buf[4096];
    size_t len = 0;

    while (len < sizeof(buf) - 1) {
        ssize_t n = recv(fd, buf + len, sizeof(buf) - 1 - len, 0);
        if (n <= 0)
            return -1;
        len += n;
        buf[len] = '\0';
        if (strstr(buf, "\r\n\r\n") || strstr(buf, "\n\n"))
            break;
    }

    char target[512];
    if (sscanf(buf, "%7s %511s", req->method, target) != 2)
        return -1;
    char *q = strchr(target, '?');
    if (q)
        *q++ = '\0';
    snprintf(req->path, sizeof(req->path), "%s", target);
    snprintf(req->query, sizeof(req->query), "%s", q ? q : "");
    return 0;
}

static void *conn_thread(void *arg)
{
    conn_t c = *(conn_t *)arg;
    free(arg);

    struct timeval tv = { .tv_sec = 5 };
    setsockopt(c.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    tv.tv_sec = 10;    /* a client that stops reading is dropped after 10 s */
    setsockopt(c.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int one = 1;
    setsockopt(c.fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    http_req_t req = { .fd = c.fd };
    if (parse_request(c.fd, &req) == 0)
        c.handler(&req);
    else
        http_respond_json(&req, 400, "{\"status\":\"error\",\"message\":\"Bad request\"}");

    close(c.fd);
    return NULL;
}

int http_serve(int port, http_handler handler)
{
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (srv < 0 || bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(srv, 16) < 0) {
        perror("[http] cannot listen");
        return -1;
    }
    printf("[http] listening on 0.0.0.0:%d\n", port);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, THREAD_STACK);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    for (;;) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0)
            continue;
        conn_t *c = malloc(sizeof(*c));
        pthread_t th;
        if (!c) {
            close(fd);
            continue;
        }
        c->fd = fd;
        c->handler = handler;
        if (pthread_create(&th, &attr, conn_thread, c)) {
            close(fd);
            free(c);
        }
    }
}

const char *http_query(const http_req_t *req, const char *name, char *buf, size_t size)
{
    size_t nlen = strlen(name);
    const char *p = req->query;

    while (*p) {
        const char *end = strchr(p, '&');
        size_t plen = end ? (size_t)(end - p) : strlen(p);
        if (plen > nlen && strncmp(p, name, nlen) == 0 && p[nlen] == '=') {
            size_t vlen = plen - nlen - 1;
            if (vlen >= size)
                vlen = size - 1;
            memcpy(buf, p + nlen + 1, vlen);
            buf[vlen] = '\0';
            return buf;
        }
        p += plen + (end ? 1 : 0);
    }
    return NULL;
}

int http_respond(http_req_t *req, int status, const char *ctype,
                 const void *body, size_t len, const char *extra_headers)
{
    char head[512];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n%s"
                     "Connection: close\r\n\r\n",
                     status, status_text(status), ctype, len, extra_headers ? extra_headers : "");
    struct iovec iov[2] = { { head, n }, { (void *)body, len } };
    return writev_all(req->fd, iov, len ? 2 : 1);
}

int http_respond_json(http_req_t *req, int status, const char *json)
{
    return http_respond(req, status, "application/json", json, strlen(json), NULL);
}

int http_start_chunked(http_req_t *req, const char *ctype)
{
    char head[256];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nTransfer-Encoding: chunked\r\n"
                     "Connection: close\r\n\r\n", ctype);
    return send_all(req->fd, head, n);
}

int http_send_chunk(http_req_t *req, const struct iovec *parts, int n)
{
    struct iovec iov[8];
    char size_line[24];
    size_t total = 0;

    if (n > 6)
        return -1;
    for (int i = 0; i < n; i++) {
        iov[i + 1] = parts[i];
        total += parts[i].iov_len;
    }
    if (total == 0)
        return 0;   /* an empty chunk would end the stream */
    iov[0].iov_base = size_line;
    iov[0].iov_len = snprintf(size_line, sizeof(size_line), "%zx\r\n", total);
    iov[n + 1].iov_base = "\r\n";
    iov[n + 1].iov_len = 2;
    return writev_all(req->fd, iov, n + 2);
}

int http_end_chunked(http_req_t *req)
{
    return send_all(req->fd, "0\r\n\r\n", 5);
}
