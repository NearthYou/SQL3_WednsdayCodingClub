#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
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
    DbBat bat;
    DbRes *resv = NULL;
    int rcnt = 0;
    DbRes res;

    T_OK(t_mktmp(root, sizeof(root)));
    T_OK(t_seed(root));
    memset(&cfg, 0, sizeof(cfg));
    cfg.root = root;
    cfg.max_sql = 4096;
    cfg.max_qry = 2;
    db = db_open(&cfg);
    T_OK(db != NULL);

    {
        static const char *sqlv[] = {
            "INSERT INTO users VALUES (4, ok@test.com, OkName, 19)",
            "UPDATE users SET age = 20 WHERE id = 4"
        };
        bat.sqlv = sqlv;
        bat.sqln = 2;
        bat.tx = 1;
        T_OK(db_batch(db, &bat, &resv, &rcnt) == 0);
        T_OK(rcnt == 2);
        db_free(&resv[0]);
        db_free(&resv[1]);
        free(resv);
        resv = NULL;
        rcnt = 0;
        clr(&res);
        T_OK(db_exec(db, "SELECT * FROM users WHERE id = 4", &res) == 0);
        T_OK(res.count == 1);
        T_OK(strcmp(res.rows[0].cells[3].val, "20") == 0);
        db_free(&res);
    }

    {
        static const char *sqlv[] = {
            "INSERT INTO users VALUES (5, bad@test.com, BadName, 18)",
            "INVALID SQL"
        };
        bat.sqlv = sqlv;
        bat.sqln = 2;
        bat.tx = 1;
        T_OK(db_batch(db, &bat, &resv, &rcnt) != 0);
        T_OK(rcnt == 2);
        T_OK(strcmp(resv[1].err.code, "TX_ABORT") == 0);
        db_free(&resv[0]);
        db_free(&resv[1]);
        free(resv);
        resv = NULL;
        rcnt = 0;
        clr(&res);
        T_OK(db_exec(db, "SELECT * FROM users WHERE id = 5", &res) == 0);
        T_OK(res.count == 0);
        db_free(&res);
    }

    bat.sqlv = NULL;
    bat.sqln = 0;
    bat.tx = 1;
    T_OK(db_batch(db, &bat, &resv, &rcnt) == 0);
    T_OK(resv == NULL);
    T_OK(rcnt == 0);

    {
        static const char *sqlv[] = {
            "SELECT * FROM users WHERE id = 1",
            "SELECT * FROM users WHERE id = 2",
            "SELECT * FROM users WHERE id = 3"
        };
        bat.sqlv = sqlv;
        bat.sqln = 3;
        bat.tx = 0;
        T_OK(db_batch(db, &bat, &resv, &rcnt) != 0);
    }

    db_close(db);
    printf("test_tx: ok\n");
    return 0;
}
