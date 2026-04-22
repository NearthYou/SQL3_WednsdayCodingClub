#include "route.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "resp.h"
#include "srv.h"
#include "cache/query_cache.h"
#include "db/dbapi.h"
#include "legacy/parser.h"
#include "stats/stats.h"
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
    long start_ms;
    long lat_ms;
    long base_ms;
    int delay_ms;
} QJob;

static const char *skip_ws(const char *ptr);

#define PAGE_QUERY_COUNT 4
#define PAGE_DELAY_MS_MAX 200
#define PAGE_WORKERS_MIN 1
#define PAGE_WORKERS_MAX 4

typedef enum {
    PAGE_MODE_PARALLEL = 0,
    PAGE_MODE_SEQUENTIAL = 1,
    PAGE_MODE_COMPARE = 2
} PageMode;

typedef struct {
    long total_ms;
    long sum_lat_ms;
} PageRunMetrics;

typedef struct {
    unsigned long rss_kb;
    unsigned long ctx_voluntary;
    unsigned long ctx_nonvoluntary;
} ProcRuntimeStats;

/* 요청 지연 시간을 계산하기 위해 현재 시간을 밀리초 단위로 반환한다. */
static long now_ms(void) {
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (long)tv.tv_sec * 1000L + tv.tv_usec / 1000L;
}

/* 세부 성능 통계를 위해 monotonic 시간을 나노초 단위로 반환한다. */
static unsigned long long now_ns(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ULL + (unsigned long long)ts.tv_nsec;
}

/* 데모용 지연을 주기 위해 밀리초 단위 sleep을 수행한다. */
static void sleep_ms(int ms) {
    struct timespec ts;

    if (ms <= 0) return;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* DB worker pool에서 읽기 쿼리 하나를 실행하고 대기 중인 스레드에 완료를 알린다. */
static void qjob_run(void *arg) {
    QJob *job = (QJob *)arg;
    long start = now_ms();

    memset(&job->res, 0, sizeof(job->res));
    job->tid = pool_tid();
    job->start_ms = start - job->base_ms;
    if (job->delay_ms > 0) sleep_ms(job->delay_ms);
    job->rc = db_read(job->srv->db, job->snap, job->sql, &job->res);
    job->lat_ms = now_ms() - start;
    pthread_mutex_lock(&job->mu);
    job->done = 1;
    pthread_cond_signal(&job->cv);
    pthread_mutex_unlock(&job->mu);
}

/* 큐에 넣은 DB 읽기 작업이 끝날 때까지 대기한다. */
static void qjob_wait(QJob *job) {
    pthread_mutex_lock(&job->mu);
    while (!job->done) pthread_cond_wait(&job->cv, &job->mu);
    pthread_mutex_unlock(&job->mu);
}

/* DB 읽기 작업과 그 작업에 필요한 mutex/condition 변수를 초기화한다. */
static void qjob_init(QJob *job,
                      Srv *srv,
                      DbSnap *snap,
                      const char *sql,
                      const char *name,
                      long base_ms,
                      int delay_ms) {
    memset(job, 0, sizeof(*job));
    pthread_mutex_init(&job->mu, NULL);
    pthread_cond_init(&job->cv, NULL);
    job->srv = srv;
    job->snap = snap;
    job->sql = sql;
    job->name = name;
    job->base_ms = base_ms;
    job->delay_ms = delay_ms;
}

/* 현재 스레드에서 읽기 쿼리를 순차 실행할 때 qjob 데이터를 채운다. */
static void qjob_run_sync(QJob *job) {
    long start = now_ms();

    memset(&job->res, 0, sizeof(job->res));
    job->tid = pool_tid();
    job->start_ms = start - job->base_ms;
    if (job->delay_ms > 0) sleep_ms(job->delay_ms);
    job->rc = db_read(job->srv->db, job->snap, job->sql, &job->res);
    job->lat_ms = now_ms() - start;
    job->done = 1;
}

/* 완료된 작업 결과의 소유권을 호출자에게 넘겨 qjob_free가 해제하지 않게 한다. */
static void qjob_take(QJob *job, DbRes *out) {
    *out = job->res;
    memset(&job->res, 0, sizeof(job->res));
}

/* 작업이 소유한 DB 결과를 해제하고 동기화 객체를 정리한다. */
static void qjob_free(QJob *job) {
    db_free(&job->res);
    pthread_cond_destroy(&job->cv);
    pthread_mutex_destroy(&job->mu);
}

/* 내부 HTTP 상태값을 이 서버가 지원하는 응답 상태값으로 정리한다. */
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

/* 내부 DB/API 에러 코드를 HTTP 상태 코드로 변환한다. */
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

/* 공통 형식의 JSON 에러 응답 본문을 만든다. */
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

/* 요청 body가 JSON 객체 형태인지 간단히 검사한다. */
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

/* JSON/query 파싱 위치에서 공백 문자를 건너뛴다. */
static const char *skip_ws(const char *ptr) {
    while (*ptr && isspace((unsigned char)*ptr)) ptr++;
    return ptr;
}

/* 단순 JSON 파서로 최상위 key의 value 시작 위치를 찾는다. */
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

/* JSON 문자열 값을 디코딩하고 필요하면 다음 파싱 위치를 돌려준다. */
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

/* key에 해당하는 JSON 문자열 필드 하나를 추출한다. */
static int json_str(const char *json, const char *key, char **out) {
    const char *ptr;

    *out = NULL;
    ptr = find_key(json, key);
    return scan_str(ptr, out, NULL);
}

/* key에 해당하는 JSON boolean 필드 하나를 추출한다. */
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

/* key에 해당하는 JSON 문자열 배열을 추출한다. */
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

/* query string에서 key에 해당하는 값을 그대로 추출한다. */
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

/* SQL 문장이 SELECT 읽기 쿼리인지 확인한다. */
static int sql_is_rd(const char *sql) {
    Statement stmt;

    if (!parse_statement(sql, &stmt)) return 0;
    return stmt.type == STMT_SELECT;
}

/* query cache lookup miss reason을 API 응답용 문자열로 변환한다. */
static const char *cache_reason_of(QueryCacheMissReason reason) {
    if (reason == QUERY_CACHE_MISS_TTL_EXPIRED) return "ttl_expired";
    if (reason == QUERY_CACHE_MISS_VERSION_CHANGED) return "version_changed";
    if (reason == QUERY_CACHE_MISS_NO_ENTRY) return "no_entry";
    return "no_entry";
}

/* DbObj 행 하나를 JSON 객체로 버퍼에 추가한다. */
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

/* DbRes의 여러 행을 JSON 배열로 버퍼에 추가한다. */
static int add_rows(Buf *buf, DbRes *res) {
    int i;

    if (!buf_chr(buf, '[')) return 0;
    for (i = 0; i < res->row_cnt; i++) {
        if (i > 0 && !buf_chr(buf, ',')) return 0;
        if (!add_obj(buf, &res->rows[i])) return 0;
    }
    return buf_chr(buf, ']');
}

/* 첫 번째 행을 JSON으로 추가하고, 행이 없으면 빈 객체를 추가한다. */
static int add_one(Buf *buf, DbRes *res) {
    if (res->row_cnt == 0) return buf_put(buf, "{}");
    return add_obj(buf, &res->rows[0]);
}

/* 공통 SQL 응답 data 객체를 버퍼에 추가한다. */
static int sql_data(Buf *buf, DbRes *dres) {
    if (!buf_chr(buf, '{') ||
        !buf_put(buf, "\"rows\":") ||
        !add_rows(buf, dres) ||
        !buf_fmt(buf, ",\"count\":%d", dres->count)) {
        return 0;
    }
    if (dres->last_id > 0 && !buf_fmt(buf, ",\"last_id\":%ld", dres->last_id)) {
        return 0;
    }
    return buf_chr(buf, '}');
}

/* 응답과 캐시에서 재사용할 수 있도록 SQL 응답 data를 문자열로 렌더링한다. */
static int sql_data_dump(DbRes *dres, char **out, size_t *olen) {
    Buf buf;

    *out = NULL;
    *olen = 0;
    buf_init(&buf);
    if (!sql_data(&buf, dres)) {
        buf_free(&buf);
        return 0;
    }
    *out = buf.buf;
    *olen = buf.len;
    memset(&buf, 0, sizeof(buf));
    return 1;
}

/* 이미 렌더링된 data JSON으로 성공 SQL 응답을 만든다. */
static int sql_ok_data(HttpRes *res,
                       const HttpReq *req,
                       const char *data,
                       size_t data_len,
                       int tid,
                       long lat_ms,
                       long lat_us,
                       const char *cache_status,
                       const char *cache_reason) {
    Buf buf;

    buf_init(&buf);
    if (!buf_put(&buf, "{\"ok\":true,\"data\":")) {
        buf_free(&buf);
        return 0;
    }
    if (data_len > 0) {
        if (buf.cap < buf.len + data_len + 1) {
            size_t cap = buf.cap == 0 ? 256 : buf.cap;
            char *nbuf;

            while (cap < buf.len + data_len + 1) cap *= 2;
            nbuf = (char *)realloc(buf.buf, cap);
            if (!nbuf) {
                buf_free(&buf);
                return 0;
            }
            buf.buf = nbuf;
            buf.cap = cap;
        }
        memcpy(buf.buf + buf.len, data, data_len);
        buf.len += data_len;
        buf.buf[buf.len] = '\0';
    }
    if (!buf_put(&buf, ",\"meta\":{\"req_id\":") ||
        !buf_jsn(&buf, req->req_id) ||
        !buf_fmt(&buf, ",\"thr_id\":%d,\"lat_ms\":%ld,\"lat_us\":%ld,\"cache\":{\"status\":", tid, lat_ms, lat_us) ||
        !buf_jsn(&buf, cache_status ? cache_status : "miss") ||
        !buf_put(&buf, ",\"reason\":") ||
        !buf_jsn(&buf, cache_reason ? cache_reason : "not_cacheable") ||
        !buf_put(&buf, "}}}")) {
        buf_free(&buf);
        return 0;
    }
    return res_set(res, 200, &buf);
}

/* DB 결과로 성공 SQL 응답을 만든다. */
static int sql_ok(HttpRes *res,
                  const HttpReq *req,
                  DbRes *dres,
                  int tid,
                  long lat_ms,
                  long lat_us,
                  const char *cache_status,
                  const char *cache_reason) {
    char *data = NULL;
    size_t data_len = 0;
    int ok;

    if (!sql_data_dump(dres, &data, &data_len)) return 0;
    ok = sql_ok_data(res, req, data, data_len, tid, lat_ms, lat_us, cache_status, cache_reason);
    free(data);
    return ok;
}

/* 각 쿼리 결과를 담은 성공 batch 응답을 만든다. */
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

/* 트랜잭션 응답을 만들고, 실패 시 rollback 정보를 함께 담는다. */
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

/* 단순 JSON 배열 파서가 만든 문자열 배열을 해제한다. */
static void free_arr(char **arr, int cnt) {
    int i;

    for (i = 0; i < cnt; i++) free(arr[i]);
    free(arr);
}

/* 정적 asset 파일 전체를 메모리로 읽는다. */
static int read_file_all(const char *path, char **out, size_t *olen) {
    FILE *fp;
    long sz;
    size_t nread;
    char *buf;

    *out = NULL;
    *olen = 0;
    fp = fopen(path, "rb");
    if (!fp) return 0;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return 0;
    }
    sz = ftell(fp);
    if (sz < 0) {
        fclose(fp);
        return 0;
    }
    rewind(fp);
    buf = (char *)calloc((size_t)sz + 1, 1);
    if (!buf) {
        fclose(fp);
        return -1;
    }
    nread = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    if (nread != (size_t)sz) {
        free(buf);
        return 0;
    }
    buf[sz] = '\0';
    *out = buf;
    *olen = (size_t)sz;
    return 1;
}

/* 지정한 content type으로 데모 정적 asset 하나를 응답한다. */
static int do_asset(const HttpReq *req,
                    HttpRes *res,
                    const char *path,
                    const char *content_type) {
    char *raw = NULL;
    size_t len = 0;
    Buf buf;
    int rc = read_file_all(path, &raw, &len);

    if (rc == 0) {
        return err_res(res, 500, "INT_ERR", "failed to load demo asset", req->req_id);
    }
    if (rc < 0) {
        return err_res(res, 500, "OOM", "memory error", req->req_id);
    }
    buf_init(&buf);
    buf.buf = raw;
    buf.len = len;
    buf.cap = len + 1;
    if (!res_set_ct(res, 200, &buf, content_type)) {
        free(raw);
        return 0;
    }
    return 1;
}

/* 데모 좌표를 간단한 배달 zone 값으로 변환한다. */
static void zone_of(const char *lat, const char *lng, char *out, size_t osz) {
    if (lat && lng && strcmp(lat, "37.5") == 0 && strcmp(lng, "127.0") == 0) {
        strncpy(out, "seoul_east", osz - 1);
    } else {
        strncpy(out, "default", osz - 1);
    }
    out[osz - 1] = '\0';
}

/* GET /api/v1/health 요청을 처리한다. */
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

/* POST /api/v1/sql 요청을 처리하고, SELECT는 캐시 조회도 수행한다. */
static int do_sql(Srv *srv, const HttpReq *req, HttpRes *res) {
    char *sql = NULL;
    char *data_json = NULL;
    char *cached_data = NULL;
    size_t data_len = 0;
    size_t cached_len = 0;
    DbRes dres;
    Statement stmt;
    unsigned long long start_ns = now_ns();
    unsigned long long ns0;
    unsigned long long table_ver = 0;
    int can_cache = 0;
    int cache_hit = 0;
    QueryCacheMissReason cache_miss_reason = QUERY_CACHE_MISS_NO_ENTRY;
    const char *cache_status = "miss";
    const char *cache_reason = "not_cacheable";
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
    if (parse_statement(sql, &stmt) && stmt.type == STMT_SELECT && stmt.table_name[0] != '\0') {
        can_cache = 1;
        cache_reason = "no_entry";
        ns0 = now_ns();
        table_ver = db_tab_ver(srv->db, stmt.table_name);
        api_stats_add_lock_wait_ns(now_ns() - ns0);
        if (table_ver > 0 &&
            query_cache_lookup(sql, table_ver, &cached_data, &cached_len, &cache_miss_reason)) {
            unsigned long long elapsed_ns = now_ns() - start_ns;
            long lat_us = (long)(elapsed_ns / 1000ULL);
            long lat_ms = (long)(elapsed_ns / 1000000ULL);
            cache_hit = 1;
            cache_status = "hit";
            cache_reason = "cache_hit";
            api_stats_note_cache_hit();
            ns0 = now_ns();
            sql_ok_data(res, req, cached_data, cached_len, tid, lat_ms, lat_us, cache_status, cache_reason);
            api_stats_add_json_ns(now_ns() - ns0);
            free(cached_data);
            free(sql);
            return 1;
        }
        cache_reason = cache_reason_of(cache_miss_reason);
        api_stats_note_cache_miss();
    }
    ns0 = now_ns();
    rc = db_exec(srv->db, sql, &dres);
    api_stats_add_engine_ns(now_ns() - ns0);
    if (rc != 0) {
        int http = http_of(dres.err.code);
        err_res(res, http, dres.err.code[0] ? dres.err.code : "BAD_SQL",
                dres.err.msg ? dres.err.msg : "query failed", req->req_id);
        free(sql);
        db_free(&dres);
        return 1;
    }
    ns0 = now_ns();
    if (sql_data_dump(&dres, &data_json, &data_len)) {
        unsigned long long elapsed_ns = now_ns() - start_ns;
        long lat_us = (long)(elapsed_ns / 1000ULL);
        long lat_ms = (long)(elapsed_ns / 1000000ULL);
        sql_ok_data(res, req, data_json, data_len, tid, lat_ms, lat_us, cache_status, cache_reason);
    } else {
        unsigned long long elapsed_ns = now_ns() - start_ns;
        long lat_us = (long)(elapsed_ns / 1000ULL);
        long lat_ms = (long)(elapsed_ns / 1000000ULL);
        sql_ok(res, req, &dres, tid, lat_ms, lat_us, cache_status, cache_reason);
    }
    api_stats_add_json_ns(now_ns() - ns0);
    if (can_cache && !cache_hit && table_ver > 0 && data_json) {
        query_cache_store(sql, table_ver, data_json, data_len);
    }
    free(data_json);
    free(sql);
    db_free(&dres);
    return 1;
}

/* POST /api/v1/batch와 /api/v1/tx 요청을 처리한다. */
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
        long job_base = now_ms();
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
            qjob_init(&jobs[i], srv, snap, sqlv[i], NULL, job_base, 0);
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
        api_stats_add_engine_ns((unsigned long long)(now_ms() - start) * 1000000ULL);
        {
            unsigned long long j0 = now_ns();
            batch_ok(res, req, resv, sqln, now_ms() - start);
            api_stats_add_json_ns(now_ns() - j0);
        }
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
        {
            unsigned long long e0 = now_ns();
            ok = db_batch(srv->db, &bat, &resv, &rcnt) == 0;
            api_stats_add_engine_ns(now_ns() - e0);
        }
        {
            unsigned long long j0 = now_ns();
            if (tx && !ok) {
                tx_res(res, req, resv, rcnt, now_ms() - start, 0);
            } else {
                batch_ok(res, req, resv, rcnt, now_ms() - start);
            }
            api_stats_add_json_ns(now_ns() - j0);
        }
    }
    for (i = 0; i < rcnt; i++) db_free(&resv[i]);
    free(resv);
    free_arr(sqlv, sqln);
    return 1;
}

/* mode 쿼리 문자열을 page 실행 모드 enum으로 변환한다. */
static int page_mode_of(const char *raw, PageMode *out) {
    if (!raw || raw[0] == '\0' || strcmp(raw, "parallel") == 0) {
        *out = PAGE_MODE_PARALLEL;
        return 1;
    }
    if (strcmp(raw, "sequential") == 0) {
        *out = PAGE_MODE_SEQUENTIAL;
        return 1;
    }
    if (strcmp(raw, "compare") == 0) {
        *out = PAGE_MODE_COMPARE;
        return 1;
    }
    return 0;
}

/* delay_ms 쿼리를 파싱하고 허용 범위(0..PAGE_DELAY_MS_MAX)로 맞춘다. */
static int page_delay_of(const char *raw, int *out) {
    long val;
    char *end = NULL;

    if (!raw || raw[0] == '\0') {
        *out = 0;
        return 1;
    }
    val = strtol(raw, &end, 10);
    if (!end || *end != '\0' || val < 0) return 0;
    if (val > PAGE_DELAY_MS_MAX) val = PAGE_DELAY_MS_MAX;
    *out = (int)val;
    return 1;
}

/* workers 쿼리를 파싱하고 허용 범위(1..PAGE_WORKERS_MAX)로 맞춘다. */
static int page_workers_of(const char *raw, int *out) {
    long val;
    char *end = NULL;

    if (!raw || raw[0] == '\0') {
        *out = PAGE_WORKERS_MAX;
        return 1;
    }
    val = strtol(raw, &end, 10);
    if (!end || *end != '\0') return 0;
    if (val < PAGE_WORKERS_MIN || val > PAGE_WORKERS_MAX) return 0;
    *out = (int)val;
    return 1;
}

/* /page 조회 SQL 4개를 요청 파라미터 기준으로 구성한다. */
static void page_sqls(const HttpReq *req,
                      char *sql0,
                      size_t sz0,
                      char *sql1,
                      size_t sz1,
                      char *sql2,
                      size_t sz2,
                      char *sql3,
                      size_t sz3) {
    char user[64];
    char lat[64];
    char lng[64];
    char zone[64];

    qry_get(req->qry, "user_id", user, sizeof(user));
    qry_get(req->qry, "lat", lat, sizeof(lat));
    qry_get(req->qry, "lng", lng, sizeof(lng));
    if (user[0] == '\0') strncpy(user, "1", sizeof(user) - 1);
    zone_of(lat, lng, zone, sizeof(zone));

    snprintf(sql0, sz0, "SELECT * FROM restaurants WHERE zone = '%s'", zone);
    snprintf(sql1, sz1, "SELECT * FROM orders WHERE user_id = '%s'", user);
    snprintf(sql2, sz2, "SELECT * FROM coupons WHERE user_id = '%s'", user);
    snprintf(sql3, sz3, "SELECT * FROM cart WHERE user_id = '%s'", user);
}

/* page 쿼리 결과를 원래 응답 data 구조(restaurants/order/coupons/cart)로 추가한다. */
static int add_page_data(Buf *buf, QJob jobs[PAGE_QUERY_COUNT]) {
    return buf_put(buf, "{\"restaurants\":") &&
           add_rows(buf, &jobs[0].res) &&
           buf_put(buf, ",\"order\":") &&
           add_one(buf, &jobs[1].res) &&
           buf_put(buf, ",\"coupons\":") &&
           add_rows(buf, &jobs[2].res) &&
           buf_put(buf, ",\"cart\":") &&
           add_one(buf, &jobs[3].res) &&
           buf_chr(buf, '}');
}

/* page trace 배열(name/thr_id/start_ms/lat_ms/ok)을 JSON으로 추가한다. */
static int add_page_trace(Buf *buf, QJob jobs[PAGE_QUERY_COUNT]) {
    int i;

    if (!buf_chr(buf, '[')) return 0;
    for (i = 0; i < PAGE_QUERY_COUNT; i++) {
        if (i > 0 && !buf_chr(buf, ',')) return 0;
        if (!buf_put(buf, "{\"name\":") ||
            !buf_jsn(buf, jobs[i].name) ||
            !buf_fmt(buf,
                     ",\"thr_id\":%d,\"start_ms\":%ld,\"lat_ms\":%ld,\"ok\":%s}",
                     jobs[i].tid,
                     jobs[i].start_ms,
                     jobs[i].lat_ms,
                     jobs[i].rc == 0 && jobs[i].res.err.code[0] == '\0' ? "true" : "false")) {
            return 0;
        }
    }
    return buf_chr(buf, ']');
}

/* page 작업들의 lat_ms 합계를 계산한다. */
static long page_sum_lat(QJob jobs[PAGE_QUERY_COUNT]) {
    int i;
    long sum = 0;

    for (i = 0; i < PAGE_QUERY_COUNT; i++) sum += jobs[i].lat_ms;
    return sum;
}

/* DB pool을 사용해 /page 4개 SELECT를 병렬로 실행한다. */
static int page_run_parallel(Srv *srv,
                             const char *sqlv[PAGE_QUERY_COUNT],
                             const char *namev[PAGE_QUERY_COUNT],
                             int delay_ms,
                             int workers,
                             QJob jobs[PAGE_QUERY_COUNT],
                             PageRunMetrics *metrics) {
    DbSnap *snap = db_snap(srv->db);
    long phase_start = now_ms();
    int i;

    if (!snap) return 0;
    for (i = 0; i < PAGE_QUERY_COUNT; i++) {
        qjob_init(&jobs[i], srv, snap, sqlv[i], namev[i], phase_start, delay_ms);
    }
    for (i = 0; i < PAGE_QUERY_COUNT; i += workers) {
        int end = i + workers;
        int j;

        if (end > PAGE_QUERY_COUNT) end = PAGE_QUERY_COUNT;
        for (j = i; j < end; j++) {
            if (pool_add(srv->dbp, qjob_run, &jobs[j]) != 0) {
                int k;

                for (k = i; k < j; k++) qjob_wait(&jobs[k]);
                db_done(srv->db, snap);
                for (k = 0; k < PAGE_QUERY_COUNT; k++) qjob_free(&jobs[k]);
                return -1;
            }
        }
        for (j = i; j < end; j++) qjob_wait(&jobs[j]);
    }
    db_done(srv->db, snap);
    metrics->total_ms = now_ms() - phase_start;
    metrics->sum_lat_ms = page_sum_lat(jobs);
    return 1;
}

/* 현재 API worker 스레드에서 /page 4개 SELECT를 순차로 실행한다. */
static int page_run_sequential(Srv *srv,
                               const char *sqlv[PAGE_QUERY_COUNT],
                               const char *namev[PAGE_QUERY_COUNT],
                               int delay_ms,
                               QJob jobs[PAGE_QUERY_COUNT],
                               PageRunMetrics *metrics) {
    DbSnap *snap = db_snap(srv->db);
    long phase_start = now_ms();
    int i;

    if (!snap) return 0;
    for (i = 0; i < PAGE_QUERY_COUNT; i++) {
        qjob_init(&jobs[i], srv, snap, sqlv[i], namev[i], phase_start, delay_ms);
        qjob_run_sync(&jobs[i]);
    }
    db_done(srv->db, snap);
    metrics->total_ms = now_ms() - phase_start;
    metrics->sum_lat_ms = page_sum_lat(jobs);
    return 1;
}

/* 단일 mode(parallel/sequential) page 응답을 직렬화한다. */
static int page_single_res(const HttpReq *req,
                           HttpRes *res,
                           PageMode mode,
                           int delay_ms,
                           int workers,
                           QJob jobs[PAGE_QUERY_COUNT],
                           const PageRunMetrics *metrics) {
    Buf buf;
    const char *mode_name = mode == PAGE_MODE_SEQUENTIAL ? "sequential" : "parallel";

    buf_init(&buf);
    if (!buf_put(&buf, "{\"ok\":true,\"data\":") ||
        !add_page_data(&buf, jobs) ||
        !buf_put(&buf, ",\"trace\":") ||
        !add_page_trace(&buf, jobs) ||
        !buf_put(&buf, ",\"meta\":{\"req_id\":") ||
        !buf_jsn(&buf, req->req_id) ||
        !buf_put(&buf, ",\"mode\":") ||
        !buf_jsn(&buf, mode_name) ||
        !buf_fmt(&buf, ",\"delay_ms\":%d,\"workers\":%d,\"sum_lat_ms\":%ld,\"total_ms\":%ld}}",
                 delay_ms,
                 workers,
                 metrics->sum_lat_ms,
                 metrics->total_ms)) {
        buf_free(&buf);
        return 0;
    }
    return res_set(res, 200, &buf);
}

/* compare mode 응답: sequential/parallel 실행 결과와 speedup을 함께 반환한다. */
static int page_compare_res(const HttpReq *req,
                            HttpRes *res,
                            int delay_ms,
                            int workers,
                            QJob seq[PAGE_QUERY_COUNT],
                            const PageRunMetrics *seq_metrics,
                            QJob par[PAGE_QUERY_COUNT],
                            const PageRunMetrics *par_metrics) {
    Buf buf;
    double speedup = par_metrics->total_ms > 0
                         ? (double)seq_metrics->total_ms / (double)par_metrics->total_ms
                         : 0.0;

    buf_init(&buf);
    if (!buf_put(&buf, "{\"ok\":true,\"data\":{\"sequential\":{\"trace\":") ||
        !add_page_trace(&buf, seq) ||
        !buf_fmt(&buf, ",\"sum_lat_ms\":%ld,\"total_ms\":%ld},",
                 seq_metrics->sum_lat_ms,
                 seq_metrics->total_ms) ||
        !buf_put(&buf, "\"parallel\":{\"trace\":") ||
        !add_page_trace(&buf, par) ||
        !buf_fmt(&buf, ",\"sum_lat_ms\":%ld,\"total_ms\":%ld},",
                 par_metrics->sum_lat_ms,
                 par_metrics->total_ms) ||
        !buf_fmt(&buf, "\"speedup\":%.1f},\"meta\":{\"req_id\":", speedup) ||
        !buf_jsn(&buf, req->req_id) ||
        !buf_fmt(&buf, ",\"mode\":\"compare\",\"delay_ms\":%d,\"workers\":%d}}", delay_ms, workers)) {
        buf_free(&buf);
        return 0;
    }
    return res_set(res, 200, &buf);
}

/* mode별로 /page를 실행해 trace와 속도 비교를 시연한다. */
static int do_page(Srv *srv, const HttpReq *req, HttpRes *res) {
    char mode_raw[32];
    char delay_raw[32];
    char workers_raw[32];
    char sql0[256];
    char sql1[256];
    char sql2[256];
    char sql3[256];
    const char *sqlv[PAGE_QUERY_COUNT];
    const char *namev[PAGE_QUERY_COUNT] = {"restaurants", "order", "coupons", "cart"};
    PageMode mode;
    int delay_ms = 0;
    int workers = PAGE_WORKERS_MAX;
    int rc;
    QJob jobs[PAGE_QUERY_COUNT];
    PageRunMetrics metrics;
    int i;

    qry_get(req->qry, "mode", mode_raw, sizeof(mode_raw));
    qry_get(req->qry, "delay_ms", delay_raw, sizeof(delay_raw));
    qry_get(req->qry, "workers", workers_raw, sizeof(workers_raw));
    if (!page_mode_of(mode_raw, &mode)) {
        return err_res(res, 400, "BAD_REQ", "invalid mode", req->req_id);
    }
    if (!page_delay_of(delay_raw, &delay_ms)) {
        return err_res(res, 400, "BAD_REQ", "invalid delay_ms", req->req_id);
    }
    if (!page_workers_of(workers_raw, &workers)) {
        return err_res(res, 400, "BAD_REQ", "invalid workers", req->req_id);
    }

    page_sqls(req,
              sql0, sizeof(sql0),
              sql1, sizeof(sql1),
              sql2, sizeof(sql2),
              sql3, sizeof(sql3));
    sqlv[0] = sql0;
    sqlv[1] = sql1;
    sqlv[2] = sql2;
    sqlv[3] = sql3;

    if (mode == PAGE_MODE_COMPARE) {
        QJob seq_jobs[PAGE_QUERY_COUNT];
        QJob par_jobs[PAGE_QUERY_COUNT];
        PageRunMetrics seq_metrics;
        PageRunMetrics par_metrics;

        rc = page_run_sequential(srv, sqlv, namev, delay_ms, seq_jobs, &seq_metrics);
        if (rc == 0) return err_res(res, 500, "OOM", "memory error", req->req_id);
        rc = page_run_parallel(srv, sqlv, namev, delay_ms, workers, par_jobs, &par_metrics);
        if (rc == 0) {
            for (i = 0; i < PAGE_QUERY_COUNT; i++) qjob_free(&seq_jobs[i]);
            return err_res(res, 500, "OOM", "memory error", req->req_id);
        }
        if (rc < 0) {
            for (i = 0; i < PAGE_QUERY_COUNT; i++) qjob_free(&seq_jobs[i]);
            return err_res(res, 503, "Q_FULL", "db queue full", req->req_id);
        }
        rc = page_compare_res(req, res, delay_ms, workers, seq_jobs, &seq_metrics, par_jobs, &par_metrics);
        for (i = 0; i < PAGE_QUERY_COUNT; i++) qjob_free(&seq_jobs[i]);
        for (i = 0; i < PAGE_QUERY_COUNT; i++) qjob_free(&par_jobs[i]);
        return rc;
    }

    if (mode == PAGE_MODE_SEQUENTIAL) {
        rc = page_run_sequential(srv, sqlv, namev, delay_ms, jobs, &metrics);
        if (rc == 0) return err_res(res, 500, "OOM", "memory error", req->req_id);
        rc = page_single_res(req, res, PAGE_MODE_SEQUENTIAL, delay_ms, workers, jobs, &metrics);
        for (i = 0; i < PAGE_QUERY_COUNT; i++) qjob_free(&jobs[i]);
        return rc;
    }

    rc = page_run_parallel(srv, sqlv, namev, delay_ms, workers, jobs, &metrics);
    if (rc == 0) return err_res(res, 500, "OOM", "memory error", req->req_id);
    if (rc < 0) return err_res(res, 503, "Q_FULL", "db queue full", req->req_id);
    rc = page_single_res(req, res, PAGE_MODE_PARALLEL, delay_ms, workers, jobs, &metrics);
    for (i = 0; i < PAGE_QUERY_COUNT; i++) qjob_free(&jobs[i]);
    return rc;
}

/* pool, MVCC, HTTP, cache, timing 통계를 모아 GET /api/v1/metrics를 처리한다. */
static int do_mst(Srv *srv, const HttpReq *req, HttpRes *res) {
    PoolSt ap;
    PoolSt dp;
    DbMst mst;
    ApiStatsSnapshot st;
    Buf buf;

    pool_stat(srv->apip, &ap);
    pool_stat(srv->dbp, &dp);
    db_mst(srv->db, &mst);
    api_stats_snapshot(&st);
    buf_init(&buf);
    if (!buf_fmt(&buf,
                 "{\"ok\":true,\"data\":{\"api_pool\":{\"size\":%d,\"active\":%d,\"queued\":%d,\"done\":%lu},"
                 "\"db_pool\":{\"size\":%d,\"active\":%d,\"queued\":%d,\"done\":%lu},"
                 "\"mvcc\":{\"tx_live\":%lu,\"snap_min\":%lu,\"ver_now\":%lu,\"gc_wait\":%lu},"
                 "\"http\":{\"uptime_ms\":%llu,\"total_requests\":%llu,\"inflight_requests\":%llu,"
                 "\"ok_responses\":%llu,\"error_responses\":%llu,\"status_503\":%llu,\"keep_alive_reuse\":%llu},"
                 "\"cache\":{\"hits\":%llu,\"misses\":%llu},"
                 "\"timing_ns\":{\"parse\":%llu,\"lock_wait\":%llu,\"engine\":%llu,\"json\":%llu,\"send\":%llu}},"
                 "\"meta\":{\"req_id\":",
                 ap.size, ap.active, ap.queued, ap.done,
                 dp.size, dp.active, dp.queued, dp.done,
                 mst.tx_live, mst.snap_min, mst.ver_now, mst.gc_wait,
                 st.uptime_ms, st.total_requests, st.inflight_requests,
                 st.ok_responses, st.error_responses, st.status_503, st.keep_alive_reuse,
                 st.cache_hits, st.cache_misses,
                 st.parse_ns, st.lock_wait_ns, st.engine_ns, st.json_ns, st.send_ns) ||
        !buf_jsn(&buf, req->req_id) ||
        !buf_put(&buf, "}}")) {
        buf_free(&buf);
        return 0;
    }
    return res_set(res, 200, &buf);
}

/* 파싱된 HTTP 요청을 일치하는 API 또는 정적 route로 분기한다. */
int route_do(Srv *srv, const HttpReq *req, HttpRes *res) {
    if (strcmp(req->path, "/demo") == 0) {
        if (strcmp(req->meth, "GET") != 0) return err_res(res, 405, "BAD_METH", "method not allowed", req->req_id);
        return do_asset(req, res, "web/demo.html", "text/html; charset=utf-8");
    }
    if (strcmp(req->path, "/demo.css") == 0) {
        if (strcmp(req->meth, "GET") != 0) return err_res(res, 405, "BAD_METH", "method not allowed", req->req_id);
        return do_asset(req, res, "web/demo.css", "text/css; charset=utf-8");
    }
    if (strcmp(req->path, "/demo.js") == 0) {
        if (strcmp(req->meth, "GET") != 0) return err_res(res, 405, "BAD_METH", "method not allowed", req->req_id);
        return do_asset(req, res, "web/demo.js", "application/javascript; charset=utf-8");
    }
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
