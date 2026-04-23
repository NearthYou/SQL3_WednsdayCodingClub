#ifndef SRV_H
#define SRV_H

#include "db/dbapi.h"
#include "db/mvcc.h"
#include "thr/pool.h"

typedef struct Srv {
    Db *db;
    Pool *apip;
    Pool *dbp;
    int port;
    int max_body;
    int max_sql;
    int max_qry;
    int stop;
    int lfd;
} Srv;

typedef struct {
    Db *db;
    int port;
    int api_thr;
    int db_thr;
    int que_max;
    int max_body;
    int max_sql;
    int max_qry;
} SrvCfg;

Srv *srv_new(const SrvCfg *cfg);
void srv_del(Srv *srv);
int srv_run(Srv *srv);
void srv_stop(Srv *srv);

#endif
