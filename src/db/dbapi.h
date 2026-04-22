#ifndef DBAPI_H
#define DBAPI_H

#include <stddef.h>

typedef struct Db Db;
typedef struct DbTx DbTx;
typedef struct DbSnap DbSnap;

typedef struct {
    char *key;
    char *val;
} DbCell;

typedef struct {
    int len;
    DbCell *cells;
} DbObj;

typedef struct {
    char code[16];
    char *msg;
} DbErr;

typedef struct {
    DbObj *rows;
    int row_cnt;
    int count;
    long last_id;
    unsigned long snap_id;
    DbErr err;
} DbRes;

typedef struct {
    const char *root;
    int max_sql;
    int max_qry;
} DbCfg;

typedef struct {
    const char **sqlv;
    int sqln;
    int tx;
} DbBat;

typedef struct {
    unsigned long tx_live;
    unsigned long snap_min;
    unsigned long ver_now;
    unsigned long gc_wait;
} DbMst;

Db *db_open(const DbCfg *cfg);
void db_close(Db *db);
int db_exec(Db *db, const char *sql, DbRes *res);
int db_read(Db *db, DbSnap *snap, const char *sql, DbRes *res);
int db_batch(Db *db, const DbBat *bat, DbRes **resv, int *rcnt);
int db_begin(Db *db, DbTx **out);
int db_txdo(DbTx *tx, const char *sql, DbRes *res);
int db_commit(Db *db, DbTx *tx);
int db_abort(Db *db, DbTx *tx);
unsigned long long db_tab_ver(Db *db, const char *table);
DbSnap *db_snap(Db *db);
void db_done(Db *db, DbSnap *snap);
void db_free(DbRes *res);
int db_mst(Db *db, DbMst *out);

#endif
