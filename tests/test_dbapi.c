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
    DbRes res;
    DbRes *resv = NULL;
    DbBat bat;
    int rcnt = 0;

    T_OK(t_mktmp(root, sizeof(root)));
    T_OK(t_seed(root));
    memset(&cfg, 0, sizeof(cfg));
    cfg.root = root;
    cfg.max_sql = 4096;
    cfg.max_qry = 32;
    db = db_open(&cfg);
    T_OK(db != NULL);

    clr(&res);
    T_OK(db_exec(db, "SELECT * FROM users WHERE id = 1", &res) == 0);
    T_OK(res.count == 1);
    T_OK(res.row_cnt == 1);
    T_OK(res.rows[0].len == 4);
    T_OK(strcmp(res.rows[0].cells[1].key, "email") == 0);
    T_OK(strcmp(res.rows[0].cells[1].val, "admin@test.com") == 0);
    db_free(&res);

    clr(&res);
    T_OK(db_exec(db, "INSERT INTO users VALUES (NULL, add@test.com, Added, 19)", &res) == 0);
    T_OK(res.last_id == 4);
    db_free(&res);
    clr(&res);
    T_OK(db_exec(db, "UPDATE users SET name = Updated WHERE id = 4", &res) == 0);
    T_OK(res.count == 1);
    db_free(&res);
    clr(&res);
    T_OK(db_exec(db, "DELETE FROM users WHERE id = 4", &res) == 0);
    T_OK(res.count == 1);
    db_free(&res);

    bat.tx = 0;
    bat.sqln = 2;
    {
        static const char *sqlv[] = {
            "SELECT * FROM users WHERE id = 1",
            "SELECT * FROM restaurants WHERE zone = 'seoul_east'"
        };
        bat.sqlv = sqlv;
        T_OK(db_batch(db, &bat, &resv, &rcnt) == 0);
        T_OK(rcnt == 2);
        T_OK(resv[0].count == 1);
        T_OK(resv[1].count == 2);
        db_free(&resv[0]);
        db_free(&resv[1]);
        free(resv);
        resv = NULL;
    }

    clr(&res);
    T_OK(db_exec(db, "CREATE TABLE nope", &res) != 0);
    T_OK(strcmp(res.err.code, "BAD_SQL") == 0);
    db_free(&res);

    db_close(db);
    printf("test_dbapi: ok\n");
    return 0;
}
