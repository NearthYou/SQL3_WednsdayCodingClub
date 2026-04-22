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
#include <unistd.h>

#include "resp.h"
#include "route.h"

typedef struct {
    Srv *srv;
    int fd;
} Conn;

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

static void free_req(HttpReq *req) {
    free((char *)req->body);
    memset(req, 0, sizeof(*req));
}

static int parse_req(int fd, int max_body, HttpReq *req) {
    char *buf = NULL;
    size_t cap = 0;
    size_t len = 0;
    ssize_t nread;
    char *hdr;
    char *line;
    char *ptr;
    size_t need = 0;

    memset(req, 0, sizeof(*req));
    while (1) {
        if (cap - len < 1025) {
            size_t ncap = cap == 0 ? 4097 : cap * 2 + 1;
            char *nbuf = (char *)realloc(buf, ncap);
            if (!nbuf) {
                free(buf);
                return 500;
            }
            buf = nbuf;
            cap = ncap;
        }
        nread = recv(fd, buf + len, cap - len - 1, 0);
        if (nread <= 0) {
            free(buf);
            return 400;
        }
        len += (size_t)nread;
        buf[len] = '\0';
        hdr = strstr(buf, "\r\n\r\n");
        if (hdr) break;
        hdr = strstr(buf, "\n\n");
        if (hdr) break;
        if (len > 65536) {
            free(buf);
            return 413;
        }
    }
    if (strstr(buf, "\r\n\r\n")) hdr = strstr(buf, "\r\n\r\n") + 4;
    else hdr = strstr(buf, "\n\n") + 2;
    line = strtok(buf, "\r\n");
    if (!line || sscanf(line, "%7s %255s", req->meth, req->path) != 2) {
        free(buf);
        return 400;
    }
    ptr = strchr(req->path, '?');
    if (ptr) {
        strncpy(req->qry, ptr + 1, sizeof(req->qry) - 1);
        *ptr = '\0';
    }
    for (line = strtok(NULL, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        if (line[0] == '\0') break;
        if (strncasecmp(line, "Content-Length:", 15) == 0) need = (size_t)strtoul(line + 15, NULL, 10);
    }
    if ((int)need > max_body) {
        free(buf);
        return 413;
    }
    req->body = (char *)calloc(need + 1, 1);
    if (!req->body) {
        free(buf);
        return 500;
    }
    req->body_len = need;
    if (need > 0) {
        size_t have = len - (size_t)(hdr - buf);
        if (have > need) have = need;
        memcpy((char *)req->body, hdr, have);
        while (have < need) {
            nread = recv(fd, (char *)req->body + have, need - have, 0);
            if (nread <= 0) {
                free_req(req);
                free(buf);
                return 400;
            }
            have += (size_t)nread;
        }
        ((char *)req->body)[need] = '\0';
    }
    free(buf);
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

static void send_res(int fd, HttpRes *res) {
    char head[256];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n\r\n",
                     res->code, reason_of(res->code), res->len);
    if (n > 0) write_all(fd, head, (size_t)n);
    if (res->body && res->len > 0) write_all(fd, res->body, res->len);
}

static void conn_run(void *arg) {
    Conn *conn = (Conn *)arg;
    HttpReq req;
    HttpRes res;
    int code;
    static unsigned long req_seq = 0;

    memset(&res, 0, sizeof(res));
    code = parse_req(conn->fd, conn->srv->max_body, &req);
    snprintf(req.req_id, sizeof(req.req_id), "%lu", __sync_add_and_fetch(&req_seq, 1));
    if (code != 200) {
        Buf buf;
        const char *errc = "BAD_REQ";
        const char *msg = "request parse failed";

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
        send_res(conn->fd, &res);
        res_free(&res);
        close(conn->fd);
        free(conn);
        return;
    }
    route_do(conn->srv, &req, &res);
    send_res(conn->fd, &res);
    res_free(&res);
    free_req(&req);
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
            send_res(cfd, &res);
            res_free(&res);
            close(cfd);
            free(conn);
        }
    }
    return 0;
}
