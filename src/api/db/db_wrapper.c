#include "db_wrapper.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../cache/query_cache.h"
#include "../json/json_builder.h"
#include "../stats/stats.h"
#include "../../executor.h"
#include "../../parser.h"
#include "../../types.h"

static pthread_rwlock_t g_db_lock;
static int g_db_initialized = 0;

#define TABLE_VERSION_SLOTS 16

typedef struct {
    int in_use;
    char table_name[256];
    unsigned long long version;
} TableVersionEntry;

typedef struct {
    DbResult *result;
} RowCollector;

typedef struct {
    JsonRowsWriter *writer;
} JsonRowCollector;

static pthread_mutex_t g_table_versions_mutex = PTHREAD_MUTEX_INITIALIZER;
static TableVersionEntry g_table_versions[TABLE_VERSION_SLOTS];

static unsigned long long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ULL + (unsigned long long)ts.tv_nsec;
}

static char *dup_string(const char *s) {
    size_t n;
    char *out;
    if (!s) s = "";
    n = strlen(s) + 1;
    out = (char *)malloc(n);
    if (!out) return NULL;
    memcpy(out, s, n);
    return out;
}

static DbResult make_error(int status, const char *msg) {
    DbResult result;
    memset(&result, 0, sizeof(result));
    result.ok = 0;
    result.http_status = status;
    result.error_message = dup_string(msg ? msg : "unknown error");
    return result;
}

static DbResult make_ok_empty(void) {
    DbResult result;
    memset(&result, 0, sizeof(result));
    result.ok = 1;
    result.http_status = 200;
    return result;
}

static void set_result_error(DbResult *result, int status, const char *msg) {
    if (!result) return;
    result->ok = 0;
    result->http_status = status;
    free(result->error_message);
    result->error_message = dup_string(msg ? msg : "unknown error");
}

static void init_json_response(DbJsonResponse *response) {
    if (!response) return;
    memset(response, 0, sizeof(*response));
}

static int set_json_body(DbJsonResponse *response, int ok, int status, char *json_body, size_t json_len) {
    if (!response || !json_body) return 0;
    response->ok = ok;
    response->http_status = status;
    response->json_body = json_body;
    response->json_len = json_len;
    return 1;
}

static int set_json_error(DbJsonResponse *response, int status, const char *message) {
    unsigned long long json_start;
    size_t json_len = 0;
    char *json_body = NULL;

    if (!response) return 0;
    json_start = now_ns();
    json_body = json_build_error(message, &json_len);
    api_stats_add_json_ns(now_ns() - json_start);
    if (!json_body) return 0;
    return set_json_body(response, 0, status, json_body, json_len);
}

static int set_json_ok_empty(DbJsonResponse *response) {
    unsigned long long json_start;
    size_t json_len = 0;
    char *json_body = NULL;

    if (!response) return 0;
    json_start = now_ns();
    if (!json_build_ok_response(&json_body, &json_len)) {
        api_stats_add_json_ns(now_ns() - json_start);
        return 0;
    }
    api_stats_add_json_ns(now_ns() - json_start);
    return set_json_body(response, 1, 200, json_body, json_len);
}

static void clear_table_versions(void) {
    pthread_mutex_lock(&g_table_versions_mutex);
    memset(g_table_versions, 0, sizeof(g_table_versions));
    pthread_mutex_unlock(&g_table_versions_mutex);
}

static TableVersionEntry *find_table_version_slot(const char *table_name, int create_if_missing) {
    TableVersionEntry *free_slot = NULL;
    int i;

    if (!table_name || table_name[0] == '\0') return NULL;
    for (i = 0; i < TABLE_VERSION_SLOTS; i++) {
        if (g_table_versions[i].in_use &&
            strcmp(g_table_versions[i].table_name, table_name) == 0) {
            return &g_table_versions[i];
        }
        if (!g_table_versions[i].in_use && !free_slot) free_slot = &g_table_versions[i];
    }
    if (!create_if_missing || !free_slot) return NULL;
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->in_use = 1;
    snprintf(free_slot->table_name, sizeof(free_slot->table_name), "%s", table_name);
    free_slot->version = 0;
    return free_slot;
}

static unsigned long long table_version_get(const char *table_name) {
    TableVersionEntry *slot;
    unsigned long long version = 0;

    pthread_mutex_lock(&g_table_versions_mutex);
    slot = find_table_version_slot(table_name, 1);
    if (slot) version = slot->version;
    pthread_mutex_unlock(&g_table_versions_mutex);
    return version;
}

static unsigned long long table_version_bump(const char *table_name) {
    TableVersionEntry *slot;
    unsigned long long version = 0;

    pthread_mutex_lock(&g_table_versions_mutex);
    slot = find_table_version_slot(table_name, 1);
    if (slot) {
        slot->version++;
        version = slot->version;
    }
    pthread_mutex_unlock(&g_table_versions_mutex);
    return version;
}

static int is_single_statement_sql(const char *sql) {
    int in_quote = 0;
    int semi_count = 0;
    const char *last_non_space = NULL;
    const char *p;

    for (p = sql; p && *p; p++) {
        if (!isspace((unsigned char)*p)) last_non_space = p;
        if (*p == '\'') {
            in_quote = !in_quote;
        } else if (*p == ';' && !in_quote) {
            semi_count++;
        }
    }
    if (semi_count == 0) return 1;
    if (semi_count == 1 && last_non_space && *last_non_space == ';') return 1;
    return 0;
}

static void strip_optional_trailing_semicolon(char *sql) {
    char *end;
    if (!sql) return;
    end = sql + strlen(sql);
    while (end > sql && isspace((unsigned char)end[-1])) end--;
    if (end > sql && end[-1] == ';') {
        end[-1] = '\0';
    }
}

static char *trim_sql_copy(const char *sql) {
    const char *start;
    const char *end;
    size_t len;
    char *out;

    if (!sql) return NULL;
    start = sql;
    if ((unsigned char)start[0] == 0xEF &&
        (unsigned char)start[1] == 0xBB &&
        (unsigned char)start[2] == 0xBF) {
        start += 3;
    }
    while (*start && isspace((unsigned char)*start)) start++;
    end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) end--;
    len = (size_t)(end - start);
    out = (char *)malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, start, len);
    out[len] = '\0';
    return out;
}

void db_result_free(DbResult *result) {
    size_t i;
    size_t j;
    if (!result) return;
    for (i = 0; i < result->row_count; i++) {
        for (j = 0; j < result->rows[i].cell_count; j++) {
            free(result->rows[i].cells[j].name);
            free(result->rows[i].cells[j].value);
        }
        free(result->rows[i].cells);
    }
    free(result->rows);
    free(result->error_message);
    memset(result, 0, sizeof(*result));
}

void db_json_response_free(DbJsonResponse *response) {
    if (!response) return;
    free(response->json_body);
    memset(response, 0, sizeof(*response));
}

int db_wrapper_init(void) {
    if (!g_db_initialized) {
        if (pthread_rwlock_init(&g_db_lock, NULL) != 0) return 0;
        clear_table_versions();
        query_cache_init();
        g_db_initialized = 1;
    }
    return 1;
}

void db_wrapper_destroy(void) {
    if (g_db_initialized) {
        query_cache_destroy();
        clear_table_versions();
        pthread_rwlock_destroy(&g_db_lock);
        g_db_initialized = 0;
    }
}

static int collect_row_sink(const char **col_names,
                            const char **values,
                            int col_count,
                            void *ctx) {
    RowCollector *collector = (RowCollector *)ctx;
    DbResult *result = collector ? collector->result : NULL;
    DbRow *rows;
    DbRow *row;
    int i;

    if (!result || col_count < 0) return 0;
    rows = (DbRow *)realloc(result->rows, (result->row_count + 1) * sizeof(DbRow));
    if (!rows) return 0;
    result->rows = rows;
    row = &result->rows[result->row_count];
    memset(row, 0, sizeof(*row));
    row->cells = (DbCell *)calloc((size_t)col_count, sizeof(DbCell));
    if (!row->cells && col_count > 0) return 0;
    row->cell_count = (size_t)col_count;

    for (i = 0; i < col_count; i++) {
        row->cells[i].name = dup_string(col_names ? col_names[i] : "");
        row->cells[i].value = dup_string(values ? values[i] : "");
        if (!row->cells[i].name || !row->cells[i].value) return 0;
    }
    result->row_count++;
    return 1;
}

static int collect_json_row_sink(const char **col_names,
                                 const char **values,
                                 int col_count,
                                 void *ctx) {
    JsonRowCollector *collector = (JsonRowCollector *)ctx;
    return collector && collector->writer &&
           json_rows_writer_add_row(collector->writer, col_names, values, col_count);
}

DbResult db_execute_sql(const char *sql) {
    DbResult result;
    Statement stmt;
    char exec_err[256];
    char *clean_sql = NULL;
    int ok = 0;
    RowCollector collector;

    if (!g_db_initialized) {
        if (!db_wrapper_init()) return make_error(500, "failed to initialize DB wrapper");
    }

    clean_sql = trim_sql_copy(sql);
    if (!clean_sql) return make_error(500, "out of memory");
    if (clean_sql[0] == '\0') {
        free(clean_sql);
        return make_error(400, "empty SQL");
    }
    if (!is_single_statement_sql(clean_sql)) {
        free(clean_sql);
        return make_error(400, "only one SQL statement is allowed");
    }
    strip_optional_trailing_semicolon(clean_sql);
    if (!parse_statement(clean_sql, &stmt)) {
        free(clean_sql);
        return make_error(400, "invalid SQL");
    }

    result = make_ok_empty();
    collector.result = &result;
    exec_err[0] = '\0';

    pthread_rwlock_wrlock(&g_db_lock);
    if (stmt.type == STMT_SELECT) {
        ok = execute_select_result(&stmt, collect_row_sink, &collector, exec_err, sizeof(exec_err));
    } else if (stmt.type == STMT_INSERT) {
        long inserted_id = 0;
        ok = execute_insert_result(&stmt, &inserted_id, exec_err, sizeof(exec_err));
    } else if (stmt.type == STMT_UPDATE) {
        int affected = -1;
        ok = execute_update_result(&stmt, &affected, exec_err, sizeof(exec_err));
    } else if (stmt.type == STMT_DELETE) {
        int affected = -1;
        ok = execute_delete_result(&stmt, &affected, exec_err, sizeof(exec_err));
    } else {
        ok = 0;
        snprintf(exec_err, sizeof(exec_err), "unsupported SQL statement");
    }
    pthread_rwlock_unlock(&g_db_lock);
    free(clean_sql);

    if (!ok) {
        set_result_error(&result, 500, exec_err[0] ? exec_err : "DB execution failed");
        return result;
    }
    return result;
}

int db_execute_sql_json(const char *sql, DbJsonResponse *response) {
    Statement stmt;
    char exec_err[256];
    char *clean_sql = NULL;
    int ok = 0;
    unsigned long long lock_start;
    unsigned long long engine_start;
    unsigned long long json_start;
    unsigned long long table_version = 0;

    if (!response) return 0;
    init_json_response(response);
    exec_err[0] = '\0';

    if (!g_db_initialized) {
        if (!db_wrapper_init()) {
            return set_json_error(response, 500, "failed to initialize DB wrapper");
        }
    }

    clean_sql = trim_sql_copy(sql);
    if (!clean_sql) return set_json_error(response, 500, "out of memory");
    if (clean_sql[0] == '\0') {
        free(clean_sql);
        return set_json_error(response, 400, "empty SQL");
    }
    if (!is_single_statement_sql(clean_sql)) {
        free(clean_sql);
        return set_json_error(response, 400, "only one SQL statement is allowed");
    }
    strip_optional_trailing_semicolon(clean_sql);
    if (!parse_statement(clean_sql, &stmt)) {
        free(clean_sql);
        return set_json_error(response, 400, "invalid SQL");
    }

    if (stmt.type == STMT_SELECT) {
        lock_start = now_ns();
        pthread_rwlock_rdlock(&g_db_lock);
        api_stats_add_lock_wait_ns(now_ns() - lock_start);
        table_version = table_version_get(stmt.table_name);
        if (query_cache_lookup(clean_sql, table_version, &response->json_body, &response->json_len)) {
            pthread_rwlock_unlock(&g_db_lock);
            response->ok = 1;
            response->http_status = 200;
            response->cache_hit = 1;
            api_stats_note_cache_hit();
            free(clean_sql);
            return 1;
        }
        pthread_rwlock_unlock(&g_db_lock);
        api_stats_note_cache_miss();
    }

    lock_start = now_ns();
    pthread_rwlock_wrlock(&g_db_lock);
    api_stats_add_lock_wait_ns(now_ns() - lock_start);

    if (stmt.type == STMT_SELECT) {
        JsonRowsWriter writer;
        JsonRowCollector collector;

        memset(&writer, 0, sizeof(writer));
        collector.writer = &writer;
        if (!json_rows_writer_init(&writer)) {
            pthread_rwlock_unlock(&g_db_lock);
            free(clean_sql);
            return set_json_error(response, 500, "failed to initialize JSON writer");
        }

        engine_start = now_ns();
        ok = execute_select_result(&stmt, collect_json_row_sink, &collector, exec_err, sizeof(exec_err));
        api_stats_add_engine_ns(now_ns() - engine_start);
        if (!ok) {
            json_rows_writer_destroy(&writer);
            pthread_rwlock_unlock(&g_db_lock);
            free(clean_sql);
            return set_json_error(response, 500, exec_err[0] ? exec_err : "DB execution failed");
        }

        json_start = now_ns();
        ok = json_rows_writer_finish(&writer, &response->json_body, &response->json_len);
        api_stats_add_json_ns(now_ns() - json_start);
        if (!ok) {
            json_rows_writer_destroy(&writer);
            pthread_rwlock_unlock(&g_db_lock);
            free(clean_sql);
            return set_json_error(response, 500, "failed to serialize response");
        }

        response->ok = 1;
        response->http_status = 200;
        table_version = table_version_get(stmt.table_name);
    } else if (stmt.type == STMT_INSERT) {
        long inserted_id = 0;

        engine_start = now_ns();
        ok = execute_insert_result(&stmt, &inserted_id, exec_err, sizeof(exec_err));
        api_stats_add_engine_ns(now_ns() - engine_start);
        if (ok) table_version_bump(stmt.table_name);
    } else if (stmt.type == STMT_UPDATE) {
        int affected = -1;

        engine_start = now_ns();
        ok = execute_update_result(&stmt, &affected, exec_err, sizeof(exec_err));
        api_stats_add_engine_ns(now_ns() - engine_start);
        if (ok) table_version_bump(stmt.table_name);
    } else if (stmt.type == STMT_DELETE) {
        int affected = -1;

        engine_start = now_ns();
        ok = execute_delete_result(&stmt, &affected, exec_err, sizeof(exec_err));
        api_stats_add_engine_ns(now_ns() - engine_start);
        if (ok) table_version_bump(stmt.table_name);
    } else {
        ok = 0;
        snprintf(exec_err, sizeof(exec_err), "unsupported SQL statement");
    }

    pthread_rwlock_unlock(&g_db_lock);

    if (!ok) {
        free(clean_sql);
        return set_json_error(response, 500, exec_err[0] ? exec_err : "DB execution failed");
    }

    if (stmt.type != STMT_SELECT) {
        free(clean_sql);
        return set_json_ok_empty(response);
    }

    query_cache_store(clean_sql, table_version, response->json_body, response->json_len);
    free(clean_sql);
    return 1;
}
