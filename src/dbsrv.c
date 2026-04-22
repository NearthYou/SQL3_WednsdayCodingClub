#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api/srv.h"

static Srv *g_srv = NULL;

static int env_i(const char *key, int def) {
    const char *val = getenv(key);
    return val && val[0] ? atoi(val) : def;
}

static void on_sig(int sig) {
    (void)sig;
    if (g_srv) srv_stop(g_srv);
}

int main(void) {
    DbCfg dbcfg;
    Db *db;
    SrvCfg cfg;
    const char *root = getenv("DB_ROOT");
    int port = env_i("DBSV_PORT", 8080);
    int api_thr = env_i("API_THR", 4);
    int db_thr = env_i("DB_THR", 4);
    int que_max = env_i("QUE_MAX", 128);
    int max_body = env_i("MAX_BODY", 1048576);
    int max_sql = env_i("MAX_SQL", 4096);
    int max_qry = env_i("MAX_QRY", 32);

    memset(&dbcfg, 0, sizeof(dbcfg));
    dbcfg.root = (root && root[0]) ? root : "data";
    dbcfg.max_sql = max_sql;
    dbcfg.max_qry = max_qry;
    db = db_open(&dbcfg);
    if (!db) {
        fprintf(stderr, "db_open failed\n");
        return 1;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.db = db;
    cfg.port = port;
    cfg.api_thr = api_thr;
    cfg.db_thr = db_thr;
    cfg.que_max = que_max;
    cfg.max_body = max_body;
    cfg.max_sql = max_sql;
    cfg.max_qry = max_qry;
    g_srv = srv_new(&cfg);
    if (!g_srv) {
        fprintf(stderr, "srv_new failed\n");
        db_close(db);
        return 1;
    }
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    printf("dbsrv listening on :%d (api=%d db=%d q=%d)\n", port, api_thr, db_thr, que_max);
    fflush(stdout);
    srv_run(g_srv);
    srv_del(g_srv);
    db_close(db);
    g_srv = NULL;
    return 0;
}
