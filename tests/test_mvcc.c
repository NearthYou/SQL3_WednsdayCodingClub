#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "db/dbapi.h"
#include "tutil.h"

static void clr(DbRes *res) {
    memset(res, 0, sizeof(*res));
}

int main(void) {
    char root[PATH_MAX];
    DbCfg cfg;
    Db *db;
    DbRes res;
    DbSnap *snap;
    DbTx *tx1;
    DbTx *tx2;
    DbMst mst;

    T_OK(t_mktmp(root, sizeof(root)));
    T_OK(t_seed(root));
    memset(&cfg, 0, sizeof(cfg));
    cfg.root = root;
    cfg.max_sql = 4096;
    cfg.max_qry = 32;
    db = db_open(&cfg);
    T_OK(db != NULL);

    snap = db_snap(db);
    T_OK(snap != NULL);
    clr(&res);
    T_OK(db_exec(db, "INSERT INTO users VALUES (4, late@test.com, Late, 19)", &res) == 0);
    db_free(&res);
    clr(&res);
    T_OK(db_read(db, snap, "SELECT * FROM users WHERE id = 4", &res) == 0);
    T_OK(res.count == 0);
    db_free(&res);
    db_done(db, snap);

    snap = db_snap(db);
    T_OK(snap != NULL);
    clr(&res);
    T_OK(db_read(db, snap, "SELECT * FROM users WHERE id = 4", &res) == 0);
    T_OK(res.count == 1);
    db_free(&res);
    db_done(db, snap);

    T_OK(db_begin(db, &tx1) == 0);
    clr(&res);
    T_OK(db_txdo(tx1, "INSERT INTO users VALUES (5, tx1@test.com, TxOne, 30)", &res) == 0);
    db_free(&res);
    snap = db_snap(db);
    T_OK(snap != NULL);
    clr(&res);
    T_OK(db_read(db, snap, "SELECT * FROM users WHERE id = 5", &res) == 0);
    T_OK(res.count == 0);
    db_free(&res);
    db_done(db, snap);
    clr(&res);
    T_OK(db_txdo(tx1, "SELECT * FROM users WHERE id = 5", &res) == 0);
    T_OK(res.count == 1);
    db_free(&res);
    T_OK(db_commit(db, tx1) == 0);

    T_OK(db_begin(db, &tx1) == 0);
    T_OK(db_begin(db, &tx2) == 0);
    clr(&res);
    T_OK(db_txdo(tx1, "UPDATE users SET age = 31 WHERE id = 5", &res) == 0);
    db_free(&res);
    clr(&res);
    T_OK(db_txdo(tx2, "UPDATE users SET age = 32 WHERE id = 5", &res) == 0);
    db_free(&res);
    T_OK(db_commit(db, tx1) == 0);
    T_OK(db_commit(db, tx2) != 0);
    T_OK(db_abort(db, tx2) == 0);

    snap = db_snap(db);
    T_OK(snap != NULL);
    clr(&res);
    T_OK(db_exec(db, "UPDATE users SET age = 40 WHERE id = 5", &res) == 0);
    db_free(&res);
    T_OK(db_mst(db, &mst) == 0);
    T_OK(mst.gc_wait > 0);
    db_done(db, snap);
    T_OK(db_mst(db, &mst) == 0);
    T_OK(mst.gc_wait == 0);

    db_close(db);
    printf("test_mvcc: ok\n");
    return 0;
}
