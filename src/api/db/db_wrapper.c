#include "db_wrapper.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../executor.h"
#include "../../parser.h"
#include "../../types.h"

static pthread_rwlock_t g_db_lock;
static int g_db_initialized = 0;

typedef struct {
    DbResult *result;
} RowCollector;

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

int db_wrapper_init(void) {
    if (!g_db_initialized) {
        if (pthread_rwlock_init(&g_db_lock, NULL) != 0) return 0;
        g_db_initialized = 1;
    }
    return 1;
}

void db_wrapper_destroy(void) {
    if (g_db_initialized) {
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
