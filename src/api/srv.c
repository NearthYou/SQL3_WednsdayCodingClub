#include "srv.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "resp.h"
#include "route.h"
#include "cache/query_cache.h"
#include "stats/stats.h"

#define CONN_BUF_INIT 32768
#define MAX_HDR_BYTES 8192

typedef struct {
    Srv *srv;
    int fd;
} Conn;

typedef struct {
    int fd;
    char *buf;
    size_t used;
    size_t cap;
    size_t max_total;
} HttpConn;

static int write_all(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = send(fd, buf, len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return 0;
        }
        buf += (size_t)n;
        len -= (size_t)n;
    }
    return 1;
}

static void conn_init(HttpConn *conn, int fd, int max_body) {
    size_t body = (max_body > 0) ? (size_t)max_body : 1048576U;
    size_t hard = body + MAX_HDR_BYTES + 4;
    size_t init = CONN_BUF_INIT;

    if (hard < init) init = hard;
    conn->fd = fd;
    conn->buf = (char *)calloc(init + 1, 1);
    conn->used = 0;
    conn->cap = conn->buf ? init : 0;
    conn->max_total = hard;
}

static void conn_free(HttpConn *conn) {
    if (!conn) return;
    free(conn->buf);
    memset(conn, 0, sizeof(*conn));
}

static unsigned long long now_ns(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ULL + (unsigned long long)ts.tv_nsec;
}

static void free_req(HttpReq *req) {
    free((char *)req->body);
    memset(req, 0, sizeof(*req));
}

static char *find_hdr_end(const char *buf, size_t used) {
    size_t i;

    if (!buf || used < 4) return NULL;
    for (i = 0; i + 3 < used; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            return (char *)(buf + i);
        }
    }
    return NULL;
}

static void trim(char *s) {
    char *start = s;
    char *end;

    while (*start && (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')) start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) end--;
    *end = '\0';
}

static int ensure_buffered(HttpConn *conn, size_t target, const char *eof_msg) {
    while (conn->used < target) {
        ssize_t nread;
        size_t ncap;
        char *nbuf;

        if (target > conn->max_total) return 413;
        if (conn->cap < target) {
            ncap = conn->cap > 0 ? conn->cap : CONN_BUF_INIT;
            while (ncap < target) {
                if (ncap > conn->max_total / 2) {
                    ncap = conn->max_total;
                    break;
                }
                ncap *= 2;
            }
            if (ncap > conn->max_total) ncap = conn->max_total;
            if (ncap < target) return 413;
            nbuf = (char *)realloc(conn->buf, ncap + 1);
            if (!nbuf) return 500;
            conn->buf = nbuf;
            conn->cap = ncap;
        }
        nread = recv(conn->fd, conn->buf + conn->used, conn->cap - conn->used, 0);
        if (nread == 0) return conn->used == 0 ? 0 : 400;
        if (nread < 0) {
            if (errno == EINTR) continue;
            (void)eof_msg;
            return 400;
        }
        conn->used += (size_t)nread;
    }
    return 200;
}

static int parse_req(HttpConn *conn, int max_body, HttpReq *req, int *conn_close) {
    char hdr_copy[MAX_HDR_BYTES + 1];
    char *hdr_end;
    size_t hdr_len;
    size_t req_bytes;
    char *line;
    char *saveptr = NULL;
    size_t body_len = 0;
    size_t body_cap = max_body > 0 ? (size_t)max_body : 0;
    char version[16];

    memset(req, 0, sizeof(*req));
    *conn_close = 0;

    if (!conn->buf) return 500;
    while ((hdr_end = find_hdr_end(conn->buf, conn->used)) == NULL) {
        int rc;

        if (conn->used >= MAX_HDR_BYTES) return 413;
        rc = ensure_buffered(conn, conn->used + 1, "failed to read request header");
        if (rc != 200) return rc;
    }

    hdr_len = (size_t)(hdr_end - conn->buf);
    if (hdr_len > MAX_HDR_BYTES) return 413;
    memcpy(hdr_copy, conn->buf, hdr_len);
    hdr_copy[hdr_len] = '\0';

    line = strtok_r(hdr_copy, "\r\n", &saveptr);
    if (!line || sscanf(line, "%7s %255s %15s", req->meth, req->path, version) != 3) return 400;
    if (strcmp(version, "HTTP/1.1") != 0) return 400;

    for (line = strtok_r(NULL, "\r\n", &saveptr); line; line = strtok_r(NULL, "\r\n", &saveptr)) {
        char *colon = strchr(line, ':');

        if (!colon) continue;
        *colon = '\0';
        colon++;
        trim(line);
        trim(colon);
        if (strncasecmp(line, "Content-Length", 14) == 0) {
            char *endptr = NULL;
            unsigned long v = strtoul(colon, &endptr, 10);
            if (!endptr || *endptr != '\0') return 400;
            body_len = (size_t)v;
        } else if (strncasecmp(line, "Connection", 10) == 0) {
            if (strncasecmp(colon, "close", 5) == 0) *conn_close = 1;
        } else if (strncasecmp(line, "Transfer-Encoding", 17) == 0) {
            return 400;
        }
    }

    if (body_len > body_cap) return 413;
    req_bytes = hdr_len + 4 + body_len;
    if (req_bytes > conn->max_total) return 413;
    {
        int rc = ensure_buffered(conn, req_bytes, "failed to read request body");
        if (rc != 200) return rc;
    }

    if (body_len > 0) {
        req->body = (char *)calloc(body_len + 1, 1);
        if (!req->body) return 500;
        memcpy((char *)req->body, conn->buf + hdr_len + 4, body_len);
        ((char *)req->body)[body_len] = '\0';
    }
    req->body_len = body_len;

    {
        char *q = strchr(req->path, '?');
        if (q) {
            strncpy(req->qry, q + 1, sizeof(req->qry) - 1);
            req->qry[sizeof(req->qry) - 1] = '\0';
            *q = '\0';
        }
    }

    if (conn->used > req_bytes) {
        memmove(conn->buf, conn->buf + req_bytes, conn->used - req_bytes);
    }
    conn->used -= req_bytes;
    return 200;
}

static const char *reason_of(int code) {
    if (code == 200) return "OK";
    if (code == 201) return "Created";
    if (code == 400) return "Bad Request";
    if (code == 404) return "Not Found";
    if (code == 405) return "Method Not Allowed";
    if (code == 413) return "Payload Too Large";
    if (code == 429) return "Too Many Requests";
    if (code == 503) return "Service Unavailable";
    return "Internal Server Error";
}

static void send_res(int fd, HttpRes *res, int conn_close) {
    char head[256];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: %s\r\n\r\n",
                     res->code, reason_of(res->code), res->len, conn_close ? "close" : "keep-alive");
    if (n > 0) write_all(fd, head, (size_t)n);
    if (res->body && res->len > 0) write_all(fd, res->body, res->len);
}

static void conn_run(void *arg) {
    Conn *conn = (Conn *)arg;
    HttpConn hc;
    HttpReq req;
    HttpRes res;
    static unsigned long req_seq = 0;
    int req_count = 0;

    memset(&hc, 0, sizeof(hc));
    conn_init(&hc, conn->fd, conn->srv->max_body);
    while (1) {
        int code;
        int conn_close = 0;
        unsigned long long t0;

        memset(&res, 0, sizeof(res));
        t0 = now_ns();
        code = parse_req(&hc, conn->srv->max_body, &req, &conn_close);
        api_stats_add_parse_ns(now_ns() - t0);
        if (code == 0) break;
        api_stats_note_request_started();
        if (req_count > 0) api_stats_note_keep_alive_reuse();
        req_count++;
        snprintf(req.req_id, sizeof(req.req_id), "%lu", __sync_add_and_fetch(&req_seq, 1));
        if (code != 200) {
            Buf buf;
            const char *errc = "BAD_REQ";
            const char *msg = "request parse failed";
            unsigned long long s0;

            if (code == 413) {
                errc = "TOO_BIG";
                msg = "request body too large";
            } else if (code == 500) {
                errc = "INT_ERR";
                msg = "internal server error";
            }
            buf_init(&buf);
            buf_put(&buf, "{\"ok\":false,\"err\":{\"code\":");
            buf_jsn(&buf, errc);
            buf_put(&buf, ",\"msg\":");
            buf_jsn(&buf, msg);
            buf_put(&buf, "},\"meta\":{\"req_id\":");
            buf_jsn(&buf, req.req_id);
            buf_put(&buf, "}}");
            res_set(&res, code, &buf);
            s0 = now_ns();
            send_res(conn->fd, &res, 1);
            api_stats_add_send_ns(now_ns() - s0);
            api_stats_note_request_finished(code);
            res_free(&res);
            break;
        }
        route_do(conn->srv, &req, &res);
        {
            unsigned long long s0 = now_ns();
            send_res(conn->fd, &res, conn_close);
            api_stats_add_send_ns(now_ns() - s0);
        }
        api_stats_note_request_finished(res.code);
        res_free(&res);
        free_req(&req);
        if (conn_close) break;
    }
    conn_free(&hc);
    close(conn->fd);
    free(conn);
}

Srv *srv_new(const SrvCfg *cfg) {
    Srv *srv;

    if (!cfg || !cfg->db) return NULL;
    srv = (Srv *)calloc(1, sizeof(Srv));
    if (!srv) return NULL;
    srv->db = cfg->db;
    srv->port = cfg->port;
    srv->max_body = cfg->max_body;
    srv->max_sql = cfg->max_sql;
    srv->max_qry = cfg->max_qry;
    srv->apip = pool_new(cfg->api_thr, cfg->que_max);
    srv->dbp = pool_new(cfg->db_thr, cfg->que_max);
    if (!srv->apip || !srv->dbp) {
        srv_del(srv);
        return NULL;
    }
    api_stats_init();
    query_cache_init();
    return srv;
}

void srv_stop(Srv *srv) {
    if (!srv) return;
    srv->stop = 1;
    if (srv->lfd > 0) close(srv->lfd);
    srv->lfd = -1;
    pool_stop(srv->apip);
    pool_stop(srv->dbp);
}

void srv_del(Srv *srv) {
    if (!srv) return;
    srv_stop(srv);
    pool_del(srv->apip);
    pool_del(srv->dbp);
    query_cache_destroy();
    api_stats_destroy();
    free(srv);
}

int srv_run(Srv *srv) {
    int fd;
    int one = 1;
    struct sockaddr_in addr;

    if (!srv) return -1;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)srv->port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(fd, 128) != 0) {
        close(fd);
        return -1;
    }
    srv->lfd = fd;
    printf("dbsrv listening on :%d\n", srv->port);
    fflush(stdout);
    while (!srv->stop) {
        int cfd = accept(fd, NULL, NULL);
        Conn *conn;

        if (cfd < 0) {
            if (srv->stop) break;
            continue;
        }
        conn = (Conn *)calloc(1, sizeof(Conn));
        if (!conn) {
            close(cfd);
            continue;
        }
        conn->srv = srv;
        conn->fd = cfd;
        if (pool_add(srv->apip, conn_run, conn) != 0) {
            HttpRes res;
            Buf buf;

            memset(&res, 0, sizeof(res));
            buf_init(&buf);
            buf_put(&buf, "{\"ok\":false,\"err\":{\"code\":\"Q_FULL\",\"msg\":\"api queue full\"}}");
            res_set(&res, 503, &buf);
            send_res(cfd, &res, 1);
            api_stats_note_immediate_503();
            res_free(&res);
            close(cfd);
            free(conn);
        }
    }
    return 0;
}
