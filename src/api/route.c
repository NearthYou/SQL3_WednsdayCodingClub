#include "route.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "resp.h"
#include "srv.h"
#include "db/dbapi.h"
#include "legacy/parser.h"
#include "thr/pool.h"

typedef struct {
    Srv *srv;
    DbSnap *snap;
    const char *sql;
    const char *name;
    DbRes res;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int done;
    int rc;
    int tid;
    long lat_ms;
} QJob;

static const char *skip_ws(const char *ptr);

static long now_ms(void) {
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (long)tv.tv_sec * 1000L + tv.tv_usec / 1000L;
}

static void qjob_run(void *arg) {
    QJob *job = (QJob *)arg;
    long start = now_ms();

    memset(&job->res, 0, sizeof(job->res));
    job->tid = pool_tid();
    job->rc = db_read(job->srv->db, job->snap, job->sql, &job->res);
    job->lat_ms = now_ms() - start;
    pthread_mutex_lock(&job->mu);
    job->done = 1;
    pthread_cond_signal(&job->cv);
    pthread_mutex_unlock(&job->mu);
}

static void qjob_wait(QJob *job) {
    pthread_mutex_lock(&job->mu);
    while (!job->done) pthread_cond_wait(&job->cv, &job->mu);
    pthread_mutex_unlock(&job->mu);
}

static void qjob_init(QJob *job, Srv *srv, DbSnap *snap, const char *sql, const char *name) {
    memset(job, 0, sizeof(*job));
    pthread_mutex_init(&job->mu, NULL);
    pthread_cond_init(&job->cv, NULL);
    job->srv = srv;
    job->snap = snap;
    job->sql = sql;
    job->name = name;
}

static void qjob_take(QJob *job, DbRes *out) {
    *out = job->res;
    memset(&job->res, 0, sizeof(job->res));
}

static void qjob_free(QJob *job) {
    db_free(&job->res);
    pthread_cond_destroy(&job->cv);
    pthread_mutex_destroy(&job->mu);
}

static int status_of(int code) {
    if (code == 200) return 200;
    if (code == 201) return 201;
    if (code == 400) return 400;
    if (code == 404) return 404;
    if (code == 405) return 405;
    if (code == 413) return 413;
    if (code == 429) return 429;
    if (code == 503) return 503;
    return 500;
}

static int http_of(const char *code) {
    if (!code || code[0] == '\0') return 500;
    if (strcmp(code, "NO_ROUTE") == 0) return 404;
    if (strcmp(code, "BAD_METH") == 0) return 405;
    if (strcmp(code, "TOO_BIG") == 0) return 413;
    if (strcmp(code, "TOO_MANY") == 0) return 429;
    if (strcmp(code, "Q_FULL") == 0) return 503;
    if (strcmp(code, "INT_ERR") == 0) return 500;
    if (strcmp(code, "OOM") == 0) return 500;
    return 400;
}

static int err_res(HttpRes *res, int http, const char *code, const char *msg, const char *req_id) {
    Buf buf;

    buf_init(&buf);
    if (!buf_put(&buf, "{\"ok\":false,\"err\":{\"code\":") ||
        !buf_jsn(&buf, code) ||
        !buf_put(&buf, ",\"msg\":") ||
        !buf_jsn(&buf, msg) ||
        !buf_put(&buf, "},\"meta\":{\"req_id\":") ||
        !buf_jsn(&buf, req_id ? req_id : "") ||
        !buf_put(&buf, "}}")) {
        buf_free(&buf);
        return 0;
    }
    return res_set(res, status_of(http), &buf);
}

static int json_doc(const char *body) {
    const char *ptr;
    const char *end;

    if (!body) return 0;
    ptr = skip_ws(body);
    if (*ptr != '{') return 0;
    end = body + strlen(body);
    while (end > ptr && isspace((unsigned char)end[-1])) end--;
    return end > ptr && end[-1] == '}';
}

static const char *skip_ws(const char *ptr) {
    while (*ptr && isspace((unsigned char)*ptr)) ptr++;
    return ptr;
}

static const char *find_key(const char *json, const char *key) {
    char pat[64];
    const char *ptr;

    snprintf(pat, sizeof(pat), "\"%s\"", key);
    ptr = strstr(json, pat);
    if (!ptr) return NULL;
    ptr += strlen(pat);
    ptr = strchr(ptr, ':');
    return ptr ? skip_ws(ptr + 1) : NULL;
}

static int scan_str(const char *ptr, char **out, const char **endp) {
    const char *end;
    char *dst;
    size_t len = 0;
    int esc = 0;

    *out = NULL;
    if (!ptr || *ptr != '"') return 0;
    ptr++;
    end = ptr;
    while (*end) {
        if (!esc && *end == '"') break;
        if (!esc && *end == '\\') esc = 1;
        else esc = 0;
        end++;
    }
    if (*end != '"') return 0;
    dst = (char *)calloc((size_t)(end - ptr) + 1, 1);
    if (!dst) return -1;
    while (ptr < end) {
        if (*ptr == '\\' && ptr + 1 < end) {
            ptr++;
            if (*ptr == 'n') dst[len++] = '\n';
            else if (*ptr == 'r') dst[len++] = '\r';
            else if (*ptr == 't') dst[len++] = '\t';
            else dst[len++] = *ptr;
        } else {
            dst[len++] = *ptr;
        }
        ptr++;
    }
    dst[len] = '\0';
    *out = dst;
    if (endp) *endp = end + 1;
    return 1;
}

static int json_str(const char *json, const char *key, char **out) {
    const char *ptr;

    *out = NULL;
    ptr = find_key(json, key);
    return scan_str(ptr, out, NULL);
}

static int json_bool(const char *json, const char *key, int *out) {
    const char *ptr;

    ptr = find_key(json, key);
    if (!ptr) return 0;
    if (strncmp(ptr, "true", 4) == 0) {
        *out = 1;
        return 1;
    }
    if (strncmp(ptr, "false", 5) == 0) {
        *out = 0;
        return 1;
    }
    return 0;
}

static int json_arr(const char *json, const char *key, char ***outv, int *outn) {
    const char *ptr;
    char **vals = NULL;
    int cnt = 0;
    int cap = 0;

    *outv = NULL;
    *outn = 0;
    ptr = find_key(json, key);
    if (!ptr || *ptr != '[') return 0;
    ptr++;
    while (*ptr) {
        char *item = NULL;
        char **nvals;

        ptr = skip_ws(ptr);
        if (*ptr == ']') break;
        if (*ptr != '"') break;
        if (scan_str(ptr, &item, &ptr) < 0) goto fail;
        if (!item) goto fail;
        if (cnt == cap) {
            cap = cap == 0 ? 4 : cap * 2;
            nvals = (char **)realloc(vals, (size_t)cap * sizeof(char *));
            if (!nvals) goto fail;
            vals = nvals;
        }
        vals[cnt++] = item;
        ptr = skip_ws(ptr);
        if (*ptr == ',') ptr++;
    }
    *outv = vals;
    *outn = cnt;
    return 1;

fail:
    while (cnt > 0) free(vals[--cnt]);
    free(vals);
    return -1;
}

static int qry_get(const char *qry, const char *key, char *out, size_t osz) {
    const char *ptr = qry;
    size_t klen = strlen(key);

    out[0] = '\0';
    while (ptr && *ptr) {
        const char *eq = strchr(ptr, '=');
        const char *amp = strchr(ptr, '&');
        size_t len;

        if (!eq) break;
        len = (size_t)(eq - ptr);
        if (len == klen && strncmp(ptr, key, klen) == 0) {
            const char *val = eq + 1;
            size_t vlen = amp ? (size_t)(amp - val) : strlen(val);
            if (vlen >= osz) vlen = osz - 1;
            memcpy(out, val, vlen);
            out[vlen] = '\0';
            return 1;
        }
        if (!amp) break;
        ptr = amp + 1;
    }
    return 0;
}

static int sql_is_rd(const char *sql) {
    Statement stmt;

    if (!parse_statement(sql, &stmt)) return 0;
    return stmt.type == STMT_SELECT;
}

static int add_obj(Buf *buf, DbObj *obj) {
    int i;

    if (!buf_chr(buf, '{')) return 0;
    for (i = 0; i < obj->len; i++) {
        if (i > 0 && !buf_chr(buf, ',')) return 0;
        if (!buf_jsn(buf, obj->cells[i].key) ||
            !buf_chr(buf, ':') ||
            !buf_jsn(buf, obj->cells[i].val)) {
            return 0;
        }
    }
    return buf_chr(buf, '}');
}

static int add_rows(Buf *buf, DbRes *res) {
    int i;

    if (!buf_chr(buf, '[')) return 0;
    for (i = 0; i < res->row_cnt; i++) {
        if (i > 0 && !buf_chr(buf, ',')) return 0;
        if (!add_obj(buf, &res->rows[i])) return 0;
    }
    return buf_chr(buf, ']');
}

static int add_one(Buf *buf, DbRes *res) {
    if (res->row_cnt == 0) return buf_put(buf, "{}");
    return add_obj(buf, &res->rows[0]);
}

static int add_meta(Buf *buf, const HttpReq *req, int tid, long lat_ms) {
    return buf_put(buf, ",\"meta\":{\"req_id\":") &&
           buf_jsn(buf, req->req_id) &&
           buf_fmt(buf, ",\"thr_id\":%d,\"lat_ms\":%ld}}", tid, lat_ms);
}

static int sql_ok(HttpRes *res, const HttpReq *req, DbRes *dres, int tid, long lat_ms) {
    Buf buf;

    buf_init(&buf);
    if (!buf_put(&buf, "{\"ok\":true,\"data\":{\"rows\":") ||
        !add_rows(&buf, dres) ||
        !buf_fmt(&buf, ",\"count\":%d", dres->count)) {
        buf_free(&buf);
        return 0;
    }
    if (dres->last_id > 0 && !buf_fmt(&buf, ",\"last_id\":%ld", dres->last_id)) {
        buf_free(&buf);
        return 0;
    }
    if (!buf_chr(&buf, '}') || !add_meta(&buf, req, tid, lat_ms)) {
        buf_free(&buf);
        return 0;
    }
    return res_set(res, 200, &buf);
}

static int batch_ok(HttpRes *res, const HttpReq *req, DbRes *resv, int rcnt, long lat_ms) {
    Buf buf;
    int i;

    buf_init(&buf);
    if (!buf_put(&buf, "{\"ok\":true,\"data\":{\"results\":[")) {
        buf_free(&buf);
        return 0;
    }
    for (i = 0; i < rcnt; i++) {
        if (i > 0 && !buf_chr(&buf, ',')) {
            buf_free(&buf);
            return 0;
        }
        if (!buf_put(&buf, "{\"rows\":") ||
            !add_rows(&buf, &resv[i]) ||
            !buf_fmt(&buf, ",\"count\":%d}", resv[i].count)) {
            buf_free(&buf);
            return 0;
        }
    }
    if (!buf_put(&buf, "]") ||
        !buf_put(&buf, "},\"meta\":{\"req_id\":") ||
        !buf_jsn(&buf, req->req_id) ||
        !buf_fmt(&buf, ",\"lat_ms\":%ld}}", lat_ms)) {
        buf_free(&buf);
        return 0;
    }
    return res_set(res, 200, &buf);
}

static int tx_res(HttpRes *res, const HttpReq *req, DbRes *resv, int rcnt, long lat_ms, int ok) {
    int i;
    int fail_at = 0;
    int done = 0;
    Buf buf;

    for (i = 0; i < rcnt; i++) {
        if (resv[i].err.code[0] != '\0') {
            fail_at = i + 1;
            break;
        }
        done++;
    }
    if (ok) return batch_ok(res, req, resv, rcnt, lat_ms);
    buf_init(&buf);
    if (!buf_put(&buf, "{\"ok\":false,\"err\":{\"code\":\"TX_ABORT\",\"msg\":\"query failed, rollback completed\"},\"data\":{") ||
        !buf_fmt(&buf, "\"done\":%d,\"fail_at\":%d", done, fail_at) ||
        !buf_put(&buf, "},\"meta\":{\"req_id\":") ||
        !buf_jsn(&buf, req->req_id) ||
        !buf_fmt(&buf, ",\"lat_ms\":%ld}}", lat_ms)) {
        buf_free(&buf);
        return 0;
    }
    return res_set(res, 400, &buf);
}

static void free_arr(char **arr, int cnt) {
    int i;

    for (i = 0; i < cnt; i++) free(arr[i]);
    free(arr);
}

static void zone_of(const char *lat, const char *lng, char *out, size_t osz) {
    if (lat && lng && strcmp(lat, "37.5") == 0 && strcmp(lng, "127.0") == 0) {
        strncpy(out, "seoul_east", osz - 1);
    } else {
        strncpy(out, "default", osz - 1);
    }
    out[osz - 1] = '\0';
}

static int do_health(const HttpReq *req, HttpRes *res) {
    Buf buf;

    buf_init(&buf);
    if (!buf_put(&buf, "{\"ok\":true,\"data\":{\"status\":\"up\"},\"meta\":{\"req_id\":") ||
        !buf_jsn(&buf, req->req_id) ||
        !buf_put(&buf, "}}")) {
        buf_free(&buf);
        return 0;
    }
    return res_set(res, 200, &buf);
}

static int do_sql(Srv *srv, const HttpReq *req, HttpRes *res) {
    char *sql = NULL;
    DbRes dres;
    long start = now_ms();
    int rc;
    int tid = pool_tid();

    memset(&dres, 0, sizeof(dres));
    if (!req->body || skip_ws(req->body)[0] == '\0') {
        return err_res(res, 400, "BAD_REQ", "empty body", req->req_id);
    }
    if (!json_doc(req->body)) {
        return err_res(res, 400, "BAD_JSON", "invalid json body", req->req_id);
    }
    rc = json_str(req->body, "query", &sql);
    if (rc <= 0 || !sql || sql[0] == '\0') {
        free(sql);
        return err_res(res, 400, rc < 0 ? "OOM" : "BAD_REQ", "missing query", req->req_id);
    }
    rc = db_exec(srv->db, sql, &dres);
    free(sql);
    if (rc != 0) {
        int http = http_of(dres.err.code);
        err_res(res, http, dres.err.code[0] ? dres.err.code : "BAD_SQL",
                dres.err.msg ? dres.err.msg : "query failed", req->req_id);
        db_free(&dres);
        return 1;
    }
    sql_ok(res, req, &dres, tid, now_ms() - start);
    db_free(&dres);
    return 1;
}

static int do_batch(Srv *srv, const HttpReq *req, HttpRes *res, int force_tx) {
    char **sqlv = NULL;
    int sqln = 0;
    int tx = force_tx;
    int all_rd = 1;
    int i;
    long start = now_ms();
    DbRes *resv = NULL;
    int rcnt = 0;

    if (!req->body || skip_ws(req->body)[0] == '\0') {
        return err_res(res, 400, "BAD_REQ", "empty body", req->req_id);
    }
    if (!json_doc(req->body)) {
        return err_res(res, 400, "BAD_JSON", "invalid json body", req->req_id);
    }
    if (!force_tx && json_bool(req->body, "tx", &tx) < 0) return err_res(res, 500, "OOM", "memory error", req->req_id);
    if (json_arr(req->body, "queries", &sqlv, &sqln) <= 0 || sqln == 0) {
        free_arr(sqlv, sqln);
        return err_res(res, 400, "BAD_REQ", "missing queries", req->req_id);
    }
    if (sqln > srv->max_qry) {
        free_arr(sqlv, sqln);
        return err_res(res, 429, "TOO_MANY", "too many queries", req->req_id);
    }
    for (i = 0; i < sqln; i++) {
        if (!sql_is_rd(sqlv[i])) all_rd = 0;
    }

    if (!tx && all_rd && sqln > 1) {
        DbSnap *snap = db_snap(srv->db);
        QJob *jobs;
        int add_n = 0;

        if (!snap) {
            free_arr(sqlv, sqln);
            return err_res(res, 500, "OOM", "memory error", req->req_id);
        }
        jobs = (QJob *)calloc((size_t)sqln, sizeof(QJob));
        if (!jobs) {
            db_done(srv->db, snap);
            free_arr(sqlv, sqln);
            return err_res(res, 500, "OOM", "memory error", req->req_id);
        }
        resv = (DbRes *)calloc((size_t)sqln, sizeof(DbRes));
        if (!resv) {
            free(jobs);
            db_done(srv->db, snap);
            free_arr(sqlv, sqln);
            return err_res(res, 500, "OOM", "memory error", req->req_id);
        }
        for (i = 0; i < sqln; i++) {
            qjob_init(&jobs[i], srv, snap, sqlv[i], NULL);
            if (pool_add(srv->dbp, qjob_run, &jobs[i]) != 0) {
                int j;

                for (j = 0; j < add_n; j++) qjob_wait(&jobs[j]);
                err_res(res, 503, "Q_FULL", "db queue full", req->req_id);
                for (j = 0; j < sqln; j++) qjob_free(&jobs[j]);
                free(jobs);
                free(resv);
                db_done(srv->db, snap);
                free_arr(sqlv, sqln);
                return 1;
            }
            add_n++;
        }
        for (i = 0; i < sqln; i++) {
            qjob_wait(&jobs[i]);
            qjob_take(&jobs[i], &resv[i]);
            qjob_free(&jobs[i]);
        }
        free(jobs);
        db_done(srv->db, snap);
        batch_ok(res, req, resv, sqln, now_ms() - start);
        for (i = 0; i < sqln; i++) db_free(&resv[i]);
        free(resv);
        free_arr(sqlv, sqln);
        return 1;
    }

    {
        DbBat bat;
        int ok;

        bat.sqlv = (const char **)sqlv;
        bat.sqln = sqln;
        bat.tx = tx;
        ok = db_batch(srv->db, &bat, &resv, &rcnt) == 0;
        if (tx && !ok) {
            tx_res(res, req, resv, rcnt, now_ms() - start, 0);
        } else {
            batch_ok(res, req, resv, rcnt, now_ms() - start);
        }
    }
    for (i = 0; i < rcnt; i++) db_free(&resv[i]);
    free(resv);
    free_arr(sqlv, sqln);
    return 1;
}

static int do_page(Srv *srv, const HttpReq *req, HttpRes *res) {
    char user[64];
    char lat[64];
    char lng[64];
    char zone[64];
    char sql0[256];
    char sql1[256];
    char sql2[256];
    char sql3[256];
    DbSnap *snap;
    QJob jobs[4];
    long start = now_ms();
    int i;
    Buf buf;

    qry_get(req->qry, "user_id", user, sizeof(user));
    qry_get(req->qry, "lat", lat, sizeof(lat));
    qry_get(req->qry, "lng", lng, sizeof(lng));
    if (user[0] == '\0') strncpy(user, "1", sizeof(user) - 1);
    zone_of(lat, lng, zone, sizeof(zone));
    snprintf(sql0, sizeof(sql0), "SELECT * FROM restaurants WHERE zone = '%s'", zone);
    snprintf(sql1, sizeof(sql1), "SELECT * FROM orders WHERE user_id = '%s'", user);
    snprintf(sql2, sizeof(sql2), "SELECT * FROM coupons WHERE user_id = '%s'", user);
    snprintf(sql3, sizeof(sql3), "SELECT * FROM cart WHERE user_id = '%s'", user);
    snap = db_snap(srv->db);
    if (!snap) return err_res(res, 500, "OOM", "memory error", req->req_id);
    for (i = 0; i < 4; i++) {
        qjob_init(&jobs[i], srv, snap, NULL, NULL);
    }
    jobs[0].sql = sql0; jobs[0].name = "restaurants";
    jobs[1].sql = sql1; jobs[1].name = "order";
    jobs[2].sql = sql2; jobs[2].name = "coupons";
    jobs[3].sql = sql3; jobs[3].name = "cart";
    for (i = 0; i < 4; i++) {
        if (pool_add(srv->dbp, qjob_run, &jobs[i]) != 0) {
            int j;

            for (j = 0; j < i; j++) qjob_wait(&jobs[j]);
            db_done(srv->db, snap);
            for (j = 0; j < 4; j++) qjob_free(&jobs[j]);
            return err_res(res, 503, "Q_FULL", "db queue full", req->req_id);
        }
    }
    for (i = 0; i < 4; i++) qjob_wait(&jobs[i]);
    db_done(srv->db, snap);

    buf_init(&buf);
    if (!buf_put(&buf, "{\"ok\":true,\"data\":{\"restaurants\":") ||
        !add_rows(&buf, &jobs[0].res) ||
        !buf_put(&buf, ",\"order\":") ||
        !add_one(&buf, &jobs[1].res) ||
        !buf_put(&buf, ",\"coupons\":") ||
        !add_rows(&buf, &jobs[2].res) ||
        !buf_put(&buf, ",\"cart\":") ||
        !add_one(&buf, &jobs[3].res) ||
        !buf_put(&buf, "},\"trace\":[")) {
        buf_free(&buf);
        return 0;
    }
    for (i = 0; i < 4; i++) {
        if (i > 0 && !buf_chr(&buf, ',')) {
            buf_free(&buf);
            return 0;
        }
        if (!buf_put(&buf, "{\"name\":") ||
            !buf_jsn(&buf, jobs[i].name) ||
            !buf_fmt(&buf, ",\"thr_id\":%d,\"lat_ms\":%ld,\"ok\":%s}",
                     jobs[i].tid, jobs[i].lat_ms,
                     jobs[i].rc == 0 && jobs[i].res.err.code[0] == '\0' ? "true" : "false")) {
            buf_free(&buf);
            return 0;
        }
    }
    if (!buf_put(&buf, "],\"meta\":{\"req_id\":") ||
        !buf_jsn(&buf, req->req_id) ||
        !buf_fmt(&buf, ",\"total_ms\":%ld}}", now_ms() - start)) {
        buf_free(&buf);
        return 0;
    }
    res_set(res, 200, &buf);
    for (i = 0; i < 4; i++) {
        qjob_free(&jobs[i]);
    }
    return 1;
}

static int do_mst(Srv *srv, const HttpReq *req, HttpRes *res) {
    PoolSt ap;
    PoolSt dp;
    DbMst mst;
    Buf buf;

    pool_stat(srv->apip, &ap);
    pool_stat(srv->dbp, &dp);
    db_mst(srv->db, &mst);
    buf_init(&buf);
    if (!buf_fmt(&buf,
                 "{\"ok\":true,\"data\":{\"api_pool\":{\"size\":%d,\"active\":%d,\"queued\":%d,\"done\":%lu},"
                 "\"db_pool\":{\"size\":%d,\"active\":%d,\"queued\":%d,\"done\":%lu},"
                 "\"mvcc\":{\"tx_live\":%lu,\"snap_min\":%lu,\"ver_now\":%lu,\"gc_wait\":%lu}},"
                 "\"meta\":{\"req_id\":",
                 ap.size, ap.active, ap.queued, ap.done,
                 dp.size, dp.active, dp.queued, dp.done,
                 mst.tx_live, mst.snap_min, mst.ver_now, mst.gc_wait) ||
        !buf_jsn(&buf, req->req_id) ||
        !buf_put(&buf, "}}")) {
        buf_free(&buf);
        return 0;
    }
    return res_set(res, 200, &buf);
}

int route_do(Srv *srv, const HttpReq *req, HttpRes *res) {
    if (strcmp(req->path, "/api/v1/health") == 0) {
        if (strcmp(req->meth, "GET") != 0) return err_res(res, 405, "BAD_METH", "method not allowed", req->req_id);
        return do_health(req, res);
    }
    if (strcmp(req->path, "/api/v1/sql") == 0) {
        if (strcmp(req->meth, "POST") != 0) return err_res(res, 405, "BAD_METH", "method not allowed", req->req_id);
        return do_sql(srv, req, res);
    }
    if (strcmp(req->path, "/api/v1/batch") == 0) {
        if (strcmp(req->meth, "POST") != 0) return err_res(res, 405, "BAD_METH", "method not allowed", req->req_id);
        return do_batch(srv, req, res, 0);
    }
    if (strcmp(req->path, "/api/v1/tx") == 0) {
        if (strcmp(req->meth, "POST") != 0) return err_res(res, 405, "BAD_METH", "method not allowed", req->req_id);
        return do_batch(srv, req, res, 1);
    }
    if (strcmp(req->path, "/api/v1/page") == 0) {
        if (strcmp(req->meth, "GET") != 0) return err_res(res, 405, "BAD_METH", "method not allowed", req->req_id);
        return do_page(srv, req, res);
    }
    if (strcmp(req->path, "/api/v1/metrics") == 0) {
        if (strcmp(req->meth, "GET") != 0) return err_res(res, 405, "BAD_METH", "method not allowed", req->req_id);
        return do_mst(srv, req, res);
    }
    return err_res(res, 404, "NO_ROUTE", "route not found", req->req_id);
}
