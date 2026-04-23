#ifndef ROUTE_H
#define ROUTE_H

#include <stddef.h>

typedef struct Srv Srv;
typedef struct HttpRes HttpRes;

typedef struct {
    char meth[8];
    char path[256];
    char qry[512];
    const char *body;
    size_t body_len;
    char req_id[32];
} HttpReq;

int route_do(Srv *srv, const HttpReq *req, HttpRes *res);

#endif
