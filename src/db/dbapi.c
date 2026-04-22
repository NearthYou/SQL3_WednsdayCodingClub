#include "dbapi.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mvcc.h"
#include "../legacy/bptree.h"
#include "../legacy/parser.h"
#include "../legacy/types.h"

typedef struct {
    char **vals;
} Row;

typedef struct TabVer {
    char *name;
    ColumnInfo cols[MAX_COLS];
    int col_cnt;
    int pk_idx;
    int uk_cnt;
    int uk_idx[MAX_UKS];
    Row *rows;
    int row_cnt;
    long next_id;
    unsigned long ver_id;
    BPlusTree *pk_tree;
    BPlusStringTree *uk_tree[MAX_UKS];
    struct TabVer *prev;
} TabVer;

typedef struct {
    char *name;
    TabVer *head;
} TabEnt;

typedef struct {
    TabEnt *ent;
    TabVer *base;
    TabVer *work;
    int dirty;
} TxTab;

struct Db {
    pthread_mutex_t mu;
    Mvcc *mv;
    TabEnt *tabs;
    int tab_cnt;
    int tab_cap;
    char root[PATH_MAX];
    int max_sql;
    int max_qry;
};

struct DbTx {
    Db *db;
    unsigned long snap_id;
    TxTab *tabs;
    int tab_cnt;
    int tab_cap;
    int done;
};

struct DbSnap {
    unsigned long snap_id;
};

typedef struct {
    DbRes *res;
    Statement *stmt;
    TabVer *tab;
    int *sel_idx;
    int sel_cnt;
} SelCtx;

static void tx_free(DbTx *tx);

static char *dup_s(const char *src) {
    size_t len;
    char *dst;

    if (!src) return NULL;
    len = strlen(src) + 1;
    dst = (char *)malloc(len);
    if (!dst) return NULL;
    memcpy(dst, src, len);
    return dst;
}

static void trim_uq(char *buf) {
    char *start;
    char *end;
    size_t len;

    if (!buf) return;
    start = buf;
    while (*start && isspace((unsigned char)*start)) start++;
    end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    if (*start == '\'' && end - start >= 2 && end[-1] == '\'') {
        start++;
        end--;
        *end = '\0';
    }
    if (start != buf) {
        len = strlen(start);
        memmove(buf, start, len + 1);
    }
}

static void norm_v(const char *src, char *dst, size_t dsz) {
    if (!dst || dsz == 0) return;
    strncpy(dst, src ? src : "", dsz - 1);
    dst[dsz - 1] = '\0';
    trim_uq(dst);
}

static int cmp_v(const char *lhs, const char *rhs) {
    char a[RECORD_SIZE];
    char b[RECORD_SIZE];

    norm_v(lhs, a, sizeof(a));
    norm_v(rhs, b, sizeof(b));
    return strcmp(a, b) == 0;
}

static int cmp_rng(const char *src, const char *a0, const char *a1) {
    char val[RECORD_SIZE];
    char lo[RECORD_SIZE];
    char hi[RECORD_SIZE];

    norm_v(src, val, sizeof(val));
    norm_v(a0, lo, sizeof(lo));
    norm_v(a1, hi, sizeof(hi));
    return strcmp(val, lo) >= 0 && strcmp(val, hi) <= 0;
}

static int parse_l(const char *src, long *out) {
    char buf[256];
    char *end;

    if (!src || !out) return 0;
    norm_v(src, buf, sizeof(buf));
    if (buf[0] == '\0') return 0;
    errno = 0;
    *out = strtol(buf, &end, 10);
    return errno == 0 && end != buf && *end == '\0';
}

static int split_csv(const char *row, char **vals, int maxv, char *buf, size_t bsz) {
    int idx = 0;
    int in_q = 0;
    char *ptr;

    if (!vals || maxv <= 0 || !buf || bsz == 0) return 0;
    memset(vals, 0, (size_t)maxv * sizeof(char *));
    strncpy(buf, row ? row : "", bsz - 1);
    buf[bsz - 1] = '\0';
    ptr = buf;
    vals[idx++] = ptr;
    while (*ptr && idx < maxv) {
        if (*ptr == '\'') in_q = !in_q;
        if (*ptr == ',' && !in_q) {
            *ptr = '\0';
            vals[idx++] = ptr + 1;
        }
        ptr++;
    }
    return idx;
}

static void free_row(Row *row, int col_cnt) {
    int i;

    if (!row || !row->vals) return;
    for (i = 0; i < col_cnt; i++) free(row->vals[i]);
    free(row->vals);
    row->vals = NULL;
}

static void free_rows(Row *rows, int row_cnt, int col_cnt) {
    int i;

    if (!rows) return;
    for (i = 0; i < row_cnt; i++) free_row(&rows[i], col_cnt);
    free(rows);
}

static void free_tab(TabVer *tab) {
    int i;

    if (!tab) return;
    free(tab->name);
    for (i = 0; i < tab->row_cnt; i++) free_row(&tab->rows[i], tab->col_cnt);
    free(tab->rows);
    if (tab->pk_tree) bptree_destroy(tab->pk_tree);
    for (i = 0; i < tab->uk_cnt; i++) {
        if (tab->uk_tree[i]) bptree_string_destroy(tab->uk_tree[i]);
    }
    free(tab);
}

static void free_chain(TabVer *head) {
    TabVer *cur;
    TabVer *nxt;

    cur = head;
    while (cur) {
        nxt = cur->prev;
        free_tab(cur);
        cur = nxt;
    }
}

static void res_err(DbRes *res, const char *code, const char *msg) {
    if (!res) return;
    strncpy(res->err.code, code ? code : "INT_ERR", sizeof(res->err.code) - 1);
    res->err.code[sizeof(res->err.code) - 1] = '\0';
    free(res->err.msg);
    res->err.msg = dup_s(msg ? msg : "");
}

static void res_clr(DbRes *res) {
    memset(res, 0, sizeof(*res));
}

void db_free(DbRes *res) {
    int i;
    int j;

    if (!res) return;
    for (i = 0; i < res->row_cnt; i++) {
        for (j = 0; j < res->rows[i].len; j++) {
            free(res->rows[i].cells[j].key);
            free(res->rows[i].cells[j].val);
        }
        free(res->rows[i].cells);
    }
    free(res->rows);
    free(res->err.msg);
    memset(res, 0, sizeof(*res));
}

static int col_idx(TabVer *tab, const char *name) {
    int i;

    if (!tab || !name) return -1;
    for (i = 0; i < tab->col_cnt; i++) {
        if (strcmp(tab->cols[i].name, name) == 0) return i;
    }
    return -1;
}

static int uk_slot(TabVer *tab, int idx) {
    int i;

    if (!tab) return -1;
    for (i = 0; i < tab->uk_cnt; i++) {
        if (tab->uk_idx[i] == idx) return i;
    }
    return -1;
}

static int row_match(TabVer *tab, Statement *stmt, Row *row) {
    int i;

    if (!tab || !stmt || !row) return 0;
    if (stmt->where_count == 0) return 1;
    for (i = 0; i < stmt->where_count; i++) {
        WhereCondition *cond = &stmt->where_conditions[i];
        int idx = col_idx(tab, cond->col);
        if (idx < 0) return 0;
        if (cond->type == WHERE_EQ) {
            if (!cmp_v(row->vals[idx], cond->val)) return 0;
        } else if (cond->type == WHERE_BETWEEN) {
            if (idx == tab->pk_idx) {
                long key;
                long a0;
                long a1;

                if (!parse_l(row->vals[idx], &key) ||
                    !parse_l(cond->val, &a0) ||
                    !parse_l(cond->end_val, &a1) ||
                    key < a0 || key > a1) {
                    return 0;
                }
            } else if (!cmp_rng(row->vals[idx], cond->val, cond->end_val)) {
                return 0;
            }
        } else {
            return 0;
        }
    }
    return 1;
}

static int res_add(DbRes *res, TabVer *tab, Row *row, int *sel_idx, int sel_cnt) {
    DbObj *nrows;
    DbObj *obj;
    int i;

    nrows = (DbObj *)realloc(res->rows, (size_t)(res->row_cnt + 1) * sizeof(DbObj));
    if (!nrows) return 0;
    res->rows = nrows;
    obj = &res->rows[res->row_cnt];
    memset(obj, 0, sizeof(*obj));
    obj->len = sel_cnt;
    obj->cells = (DbCell *)calloc((size_t)sel_cnt, sizeof(DbCell));
    if (!obj->cells) return 0;
    for (i = 0; i < sel_cnt; i++) {
        obj->cells[i].key = dup_s(tab->cols[sel_idx[i]].name);
        obj->cells[i].val = dup_s(row->vals[sel_idx[i]]);
        if (!obj->cells[i].key || !obj->cells[i].val) {
            int j;

            for (j = 0; j <= i; j++) {
                free(obj->cells[j].key);
                free(obj->cells[j].val);
            }
            free(obj->cells);
            obj->cells = NULL;
            return 0;
        }
    }
    res->row_cnt++;
    res->count = res->row_cnt;
    return 1;
}

static int sel_vis(long key, int row_idx, void *arg) {
    SelCtx *ctx = (SelCtx *)arg;
    Row *row;

    (void)key;
    if (!ctx || row_idx < 0 || row_idx >= ctx->tab->row_cnt) return 1;
    row = &ctx->tab->rows[row_idx];
    if (!row_match(ctx->tab, ctx->stmt, row)) return 1;
    return res_add(ctx->res, ctx->tab, row, ctx->sel_idx, ctx->sel_cnt);
}

static int sel_svis(const char *key, int row_idx, void *arg) {
    (void)key;
    return sel_vis(0, row_idx, arg);
}

static int pick_idx(TabVer *tab, Statement *stmt, int *cond_i, int *col_i) {
    int i;
    int best = -1;
    int best_s = 0;
    int best_c = -1;

    for (i = 0; i < stmt->where_count; i++) {
        int idx = col_idx(tab, stmt->where_conditions[i].col);
        int sc = 0;

        if (idx < 0) continue;
        if (stmt->where_conditions[i].type == WHERE_EQ && idx == tab->pk_idx) sc = 100;
        else if (stmt->where_conditions[i].type == WHERE_EQ && uk_slot(tab, idx) >= 0) sc = 90;
        else if (stmt->where_conditions[i].type == WHERE_BETWEEN && idx == tab->pk_idx) sc = 80;
        else if (stmt->where_conditions[i].type == WHERE_BETWEEN && uk_slot(tab, idx) >= 0) sc = 70;
        if (sc > best_s) {
            best_s = sc;
            best = i;
            best_c = idx;
        }
    }
    *cond_i = best;
    *col_i = best_c;
    return best >= 0;
}

static int sel_do(TabVer *tab, Statement *stmt, DbRes *res) {
    int sel_idx[MAX_COLS];
    int sel_cnt = 0;
    int i;
    int cond_i = -1;
    int col_i = -1;
    SelCtx ctx;

    if (stmt->select_all) {
        for (i = 0; i < tab->col_cnt; i++) sel_idx[sel_cnt++] = i;
    } else {
        for (i = 0; i < stmt->select_col_count; i++) {
            sel_idx[sel_cnt] = col_idx(tab, stmt->select_cols[i]);
            if (sel_idx[sel_cnt] < 0) {
                res_err(res, "BAD_SQL", "unknown SELECT column");
                return -1;
            }
            sel_cnt++;
        }
    }

    ctx.res = res;
    ctx.stmt = stmt;
    ctx.tab = tab;
    ctx.sel_idx = sel_idx;
    ctx.sel_cnt = sel_cnt;
    pick_idx(tab, stmt, &cond_i, &col_i);

    if (cond_i >= 0 && stmt->where_conditions[cond_i].type == WHERE_EQ && col_i == tab->pk_idx) {
        long key;
        int row_idx;

        if (!parse_l(stmt->where_conditions[cond_i].val, &key)) {
            res_err(res, "BAD_SQL", "PK filter must be integer");
            return -1;
        }
        if (bptree_search(tab->pk_tree, key, &row_idx) && !sel_vis(key, row_idx, &ctx)) {
            res_err(res, "OOM", "out of memory");
            return -1;
        }
        return 0;
    }

    if (cond_i >= 0 && stmt->where_conditions[cond_i].type == WHERE_EQ && uk_slot(tab, col_i) >= 0) {
        int row_idx;
        int slot = uk_slot(tab, col_i);
        char key[RECORD_SIZE];

        norm_v(stmt->where_conditions[cond_i].val, key, sizeof(key));
        if (bptree_string_search(tab->uk_tree[slot], key, &row_idx) && !sel_svis(key, row_idx, &ctx)) {
            res_err(res, "OOM", "out of memory");
            return -1;
        }
        return 0;
    }

    if (cond_i >= 0 && stmt->where_conditions[cond_i].type == WHERE_BETWEEN && col_i == tab->pk_idx) {
        long a0;
        long a1;

        if (!parse_l(stmt->where_conditions[cond_i].val, &a0) ||
            !parse_l(stmt->where_conditions[cond_i].end_val, &a1) ||
            !bptree_range_search(tab->pk_tree, a0, a1, sel_vis, &ctx)) {
            if (res->err.code[0] == '\0') res_err(res, "BAD_SQL", "bad PK BETWEEN");
            return res->err.code[0] ? -1 : 0;
        }
        return 0;
    }

    if (cond_i >= 0 && stmt->where_conditions[cond_i].type == WHERE_BETWEEN && uk_slot(tab, col_i) >= 0) {
        int slot = uk_slot(tab, col_i);
        char a0[RECORD_SIZE];
        char a1[RECORD_SIZE];

        norm_v(stmt->where_conditions[cond_i].val, a0, sizeof(a0));
        norm_v(stmt->where_conditions[cond_i].end_val, a1, sizeof(a1));
        if (!bptree_string_range_search(tab->uk_tree[slot], a0, a1, sel_svis, &ctx)) {
            res_err(res, "BAD_SQL", "bad UK BETWEEN");
            return -1;
        }
        return 0;
    }

    for (i = 0; i < tab->row_cnt; i++) {
        if (!row_match(tab, stmt, &tab->rows[i])) continue;
        if (!res_add(res, tab, &tab->rows[i], sel_idx, sel_cnt)) {
            res_err(res, "OOM", "out of memory");
            return -1;
        }
    }
    return 0;
}

static int chk_dup_pk(TabVer *tab, long key) {
    int row_idx;
    return tab->pk_idx >= 0 && bptree_search(tab->pk_tree, key, &row_idx);
}

static int chk_dup_uk(TabVer *tab, int idx, const char *val, int skip) {
    int row_idx;
    int slot;
    char key[RECORD_SIZE];

    slot = uk_slot(tab, idx);
    if (slot < 0 || !val || val[0] == '\0') return 0;
    norm_v(val, key, sizeof(key));
    if (!bptree_string_search(tab->uk_tree[slot], key, &row_idx)) return 0;
    return row_idx != skip;
}

static int row_copy(TabVer *tab, Row *dst, Row *src) {
    int i;

    dst->vals = (char **)calloc((size_t)tab->col_cnt, sizeof(char *));
    if (!dst->vals) return 0;
    for (i = 0; i < tab->col_cnt; i++) {
        dst->vals[i] = dup_s(src->vals[i]);
        if (!dst->vals[i]) {
            free_row(dst, tab->col_cnt);
            return 0;
        }
    }
    return 1;
}

static int row_new(TabVer *tab, Row *dst, char **vals, int val_cnt, long *new_id) {
    int i;
    int miss_pk = 0;
    int auto_pk = 0;
    char pk_buf[64];

    if (val_cnt != tab->col_cnt) {
        if (tab->pk_idx == 0 && val_cnt == tab->col_cnt - 1) miss_pk = 1;
        else return 0;
    }
    dst->vals = (char **)calloc((size_t)tab->col_cnt, sizeof(char *));
    if (!dst->vals) return 0;
    *new_id = 0;
    if (tab->pk_idx >= 0) {
        const char *pk_src = miss_pk ? NULL : vals[tab->pk_idx];

        if (!pk_src || pk_src[0] == '\0' || cmp_v(pk_src, "NULL")) {
            *new_id = tab->next_id++;
            auto_pk = 1;
        } else if (!parse_l(pk_src, new_id)) {
            free_row(dst, tab->col_cnt);
            return 0;
        } else if (*new_id >= tab->next_id) {
            tab->next_id = *new_id + 1;
        }
        snprintf(pk_buf, sizeof(pk_buf), "%ld", *new_id);
    }
    for (i = 0; i < tab->col_cnt; i++) {
        const char *src = "";

        if (tab->pk_idx >= 0 && i == tab->pk_idx && auto_pk) {
            src = pk_buf;
        } else {
            int src_i = miss_pk && i > tab->pk_idx ? i - 1 : i;

            if (src_i >= 0 && src_i < val_cnt) src = vals[src_i];
        }
        dst->vals[i] = dup_s(src ? src : "");
        if (!dst->vals[i]) {
            free_row(dst, tab->col_cnt);
            return 0;
        }
        trim_uq(dst->vals[i]);
        if (tab->cols[i].type == COL_NN && dst->vals[i][0] == '\0') {
            free_row(dst, tab->col_cnt);
            return 0;
        }
    }
    return 1;
}

static int idx_build(TabVer *tab) {
    int i;

    tab->pk_tree = bptree_create();
    if (!tab->pk_tree) return 0;
    for (i = 0; i < tab->uk_cnt; i++) {
        tab->uk_tree[i] = bptree_string_create();
        if (!tab->uk_tree[i]) return 0;
    }
    for (i = 0; i < tab->row_cnt; i++) {
        long key;
        int j;

        if (tab->pk_idx >= 0) {
            if (!parse_l(tab->rows[i].vals[tab->pk_idx], &key)) return 0;
            if (bptree_insert(tab->pk_tree, key, i) != 1) return 0;
        }
        for (j = 0; j < tab->uk_cnt; j++) {
            int col = tab->uk_idx[j];
            char txt[RECORD_SIZE];

            norm_v(tab->rows[i].vals[col], txt, sizeof(txt));
            if (txt[0] == '\0') continue;
            if (bptree_string_insert(tab->uk_tree[j], txt, i) != 1) return 0;
        }
    }
    return 1;
}

static TabVer *tab_clone(TabVer *src) {
    TabVer *dst;
    int i;

    dst = (TabVer *)calloc(1, sizeof(TabVer));
    if (!dst) return NULL;
    dst->name = dup_s(src->name);
    dst->col_cnt = src->col_cnt;
    dst->pk_idx = src->pk_idx;
    dst->uk_cnt = src->uk_cnt;
    dst->next_id = src->next_id;
    memcpy(dst->cols, src->cols, sizeof(src->cols));
    memcpy(dst->uk_idx, src->uk_idx, sizeof(src->uk_idx));
    dst->row_cnt = src->row_cnt;
    if (dst->row_cnt > 0) {
        dst->rows = (Row *)calloc((size_t)dst->row_cnt, sizeof(Row));
        if (!dst->rows) {
            free_tab(dst);
            return NULL;
        }
    }
    for (i = 0; i < dst->row_cnt; i++) {
        if (!row_copy(dst, &dst->rows[i], &src->rows[i])) {
            free_tab(dst);
            return NULL;
        }
    }
    if (!idx_build(dst)) {
        free_tab(dst);
        return NULL;
    }
    return dst;
}

static int path_csv(Db *db, const char *name, char *path, size_t psz) {
    return snprintf(path, psz, "%s/%s.csv", db->root, name) > 0;
}

static void rm_side(Db *db, const char *name) {
    char path[PATH_MAX];

    snprintf(path, sizeof(path), "%s/%s.delta", db->root, name);
    unlink(path);
    snprintf(path, sizeof(path), "%s/%s.idx", db->root, name);
    unlink(path);
}

static int load_hdr(TabVer *tab, const char *line) {
    char buf[RECORD_SIZE];
    char *vals[MAX_COLS];
    int i;
    int cnt;

    cnt = split_csv(line, vals, MAX_COLS, buf, sizeof(buf));
    for (i = 0; i < cnt; i++) {
        trim_uq(vals[i]);
        char *tag = strchr(vals[i], '(');
        if (tag) {
            size_t len = (size_t)(tag - vals[i]);
            if (len >= sizeof(tab->cols[i].name)) len = sizeof(tab->cols[i].name) - 1;
            memcpy(tab->cols[i].name, vals[i], len);
            tab->cols[i].name[len] = '\0';
            if (strstr(tag, "(PK)")) {
                tab->cols[i].type = COL_PK;
                tab->pk_idx = i;
            } else if (strstr(tag, "(UK)")) {
                tab->cols[i].type = COL_UK;
                tab->uk_idx[tab->uk_cnt++] = i;
            } else if (strstr(tag, "(NN)")) {
                tab->cols[i].type = COL_NN;
            } else {
                tab->cols[i].type = COL_NORMAL;
            }
        } else {
            strncpy(tab->cols[i].name, vals[i], sizeof(tab->cols[i].name) - 1);
            tab->cols[i].name[sizeof(tab->cols[i].name) - 1] = '\0';
            tab->cols[i].type = COL_NORMAL;
        }
    }
    tab->col_cnt = cnt;
    return cnt > 0;
}

static int load_row(TabVer *tab, const char *line) {
    char buf[RECORD_SIZE];
    char *vals[MAX_COLS];
    Row *nrows;
    int cnt;
    int i;

    cnt = split_csv(line, vals, MAX_COLS, buf, sizeof(buf));
    if (cnt != tab->col_cnt) return 0;
    nrows = (Row *)realloc(tab->rows, (size_t)(tab->row_cnt + 1) * sizeof(Row));
    if (!nrows) return 0;
    tab->rows = nrows;
    memset(&tab->rows[tab->row_cnt], 0, sizeof(Row));
    tab->rows[tab->row_cnt].vals = (char **)calloc((size_t)tab->col_cnt, sizeof(char *));
    if (!tab->rows[tab->row_cnt].vals) return 0;
    for (i = 0; i < tab->col_cnt; i++) {
        tab->rows[tab->row_cnt].vals[i] = dup_s(vals[i] ? vals[i] : "");
        if (!tab->rows[tab->row_cnt].vals[i]) {
            free_row(&tab->rows[tab->row_cnt], tab->col_cnt);
            return 0;
        }
        trim_uq(tab->rows[tab->row_cnt].vals[i]);
    }
    if (tab->pk_idx >= 0) {
        long key;
        if (!parse_l(tab->rows[tab->row_cnt].vals[tab->pk_idx], &key)) return 0;
        if (key >= tab->next_id) tab->next_id = key + 1;
    }
    tab->row_cnt++;
    return 1;
}

static TabVer *tab_load(Db *db, const char *name) {
    FILE *fp;
    char path[PATH_MAX];
    char line[RECORD_SIZE];
    TabVer *tab;

    path_csv(db, name, path, sizeof(path));
    fp = fopen(path, "r");
    if (!fp) return NULL;
    tab = (TabVer *)calloc(1, sizeof(TabVer));
    if (!tab) {
        fclose(fp);
        return NULL;
    }
    tab->name = dup_s(name);
    tab->pk_idx = -1;
    tab->next_id = 1;
    if (!fgets(line, sizeof(line), fp) || !load_hdr(tab, line)) {
        free_tab(tab);
        fclose(fp);
        return NULL;
    }
    while (fgets(line, sizeof(line), fp)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        if (line[0] == '\0') continue;
        if (!load_row(tab, line)) {
            free_tab(tab);
            fclose(fp);
            return NULL;
        }
    }
    fclose(fp);
    if (!idx_build(tab)) {
        free_tab(tab);
        return NULL;
    }
    tab->ver_id = 1;
    return tab;
}

static int tab_save(Db *db, TabVer *tab) {
    FILE *fp;
    char path[PATH_MAX];
    char tmp[PATH_MAX];
    int i;

    path_csv(db, tab->name, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    fp = fopen(tmp, "w");
    if (!fp) return 0;
    for (i = 0; i < tab->col_cnt; i++) {
        fprintf(fp, "%s", tab->cols[i].name);
        if (tab->cols[i].type == COL_PK) fprintf(fp, "(PK)");
        else if (tab->cols[i].type == COL_UK) fprintf(fp, "(UK)");
        else if (tab->cols[i].type == COL_NN) fprintf(fp, "(NN)");
        fprintf(fp, "%s", i == tab->col_cnt - 1 ? "\n" : ",");
    }
    for (i = 0; i < tab->row_cnt; i++) {
        int j;
        for (j = 0; j < tab->col_cnt; j++) {
            const char *src = tab->rows[i].vals[j];
            if (strchr(src, ',')) fprintf(fp, "'%s'", src);
            else fprintf(fp, "%s", src);
            fprintf(fp, "%s", j == tab->col_cnt - 1 ? "\n" : ",");
        }
    }
    if (fclose(fp) != 0) {
        unlink(tmp);
        return 0;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return 0;
    }
    rm_side(db, tab->name);
    return 1;
}

static TabEnt *get_ent(Db *db, const char *name) {
    int i;

    for (i = 0; i < db->tab_cnt; i++) {
        if (strcmp(db->tabs[i].name, name) == 0) return &db->tabs[i];
    }
    return NULL;
}

static TabEnt *add_ent(Db *db, const char *name, TabVer *tab) {
    TabEnt *ntabs;
    TabEnt *ent;

    if (db->tab_cnt == db->tab_cap) {
        int cap = db->tab_cap == 0 ? 8 : db->tab_cap * 2;
        ntabs = (TabEnt *)realloc(db->tabs, (size_t)cap * sizeof(TabEnt));
        if (!ntabs) return NULL;
        db->tabs = ntabs;
        db->tab_cap = cap;
    }
    ent = &db->tabs[db->tab_cnt++];
    memset(ent, 0, sizeof(*ent));
    ent->name = dup_s(name);
    ent->head = tab;
    return ent;
}

static TabEnt *need_ent(Db *db, const char *name) {
    TabEnt *ent;
    TabVer *tab;

    ent = get_ent(db, name);
    if (ent) return ent;
    tab = tab_load(db, name);
    if (!tab) return NULL;
    return add_ent(db, name, tab);
}

static TabVer *tab_vis(TabEnt *ent, unsigned long snap) {
    TabVer *cur;

    if (!ent) return NULL;
    cur = ent->head;
    while (cur && cur->ver_id > snap) cur = cur->prev;
    return cur;
}

static TxTab *tx_tab(DbTx *tx, const char *name) {
    int i;

    for (i = 0; i < tx->tab_cnt; i++) {
        if (strcmp(tx->tabs[i].ent->name, name) == 0) return &tx->tabs[i];
    }
    return NULL;
}

static TxTab *tx_view(DbTx *tx, const char *name) {
    TxTab *item;
    TabEnt *ent;
    TabVer *base;
    TxTab *ntabs;

    item = tx_tab(tx, name);
    if (item) return item;
    ent = need_ent(tx->db, name);
    if (!ent) return NULL;
    base = tab_vis(ent, tx->snap_id);
    if (!base) return NULL;
    if (tx->tab_cnt == tx->tab_cap) {
        int cap = tx->tab_cap == 0 ? 4 : tx->tab_cap * 2;
        ntabs = (TxTab *)realloc(tx->tabs, (size_t)cap * sizeof(TxTab));
        if (!ntabs) return NULL;
        tx->tabs = ntabs;
        tx->tab_cap = cap;
    }
    item = &tx->tabs[tx->tab_cnt++];
    memset(item, 0, sizeof(*item));
    item->ent = ent;
    item->base = base;
    return item;
}

static TxTab *tx_touch(DbTx *tx, const char *name) {
    TxTab *item;

    item = tx_view(tx, name);
    if (!item) return NULL;
    if (!item->work) {
        item->work = tab_clone(item->base);
        if (!item->work) return NULL;
    }
    item->dirty = 1;
    return item;
}

static int stmt_is_wr(Statement *stmt) {
    return stmt->type == STMT_INSERT ||
           stmt->type == STMT_UPDATE ||
           stmt->type == STMT_DELETE;
}

static int ins_do(TabVer *tab, Statement *stmt, DbRes *res) {
    char buf[RECORD_SIZE];
    char *vals[MAX_COLS];
    Row *nrows;
    long new_id = 0;
    int val_cnt;
    int i;

    val_cnt = split_csv(stmt->row_data, vals, MAX_COLS, buf, sizeof(buf));
    if (tab->row_cnt >= MAX_RECORDS) {
        res_err(res, "BAD_REQ", "table row limit reached");
        return -1;
    }
    nrows = (Row *)realloc(tab->rows, (size_t)(tab->row_cnt + 1) * sizeof(Row));
    if (!nrows) {
        res_err(res, "OOM", "out of memory");
        return -1;
    }
    tab->rows = nrows;
    memset(&tab->rows[tab->row_cnt], 0, sizeof(Row));
    if (!row_new(tab, &tab->rows[tab->row_cnt], vals, val_cnt, &new_id)) {
        res_err(res, "BAD_SQL", "invalid INSERT values");
        return -1;
    }
    if (tab->pk_idx >= 0 && chk_dup_pk(tab, new_id)) {
        free_row(&tab->rows[tab->row_cnt], tab->col_cnt);
        res_err(res, "BAD_SQL", "duplicate PK");
        return -1;
    }
    for (i = 0; i < tab->uk_cnt; i++) {
        int idx = tab->uk_idx[i];
        if (chk_dup_uk(tab, idx, tab->rows[tab->row_cnt].vals[idx], -1)) {
            free_row(&tab->rows[tab->row_cnt], tab->col_cnt);
            res_err(res, "BAD_SQL", "duplicate UK");
            return -1;
        }
    }
    tab->row_cnt++;
    if (tab->pk_tree && bptree_insert(tab->pk_tree, new_id, tab->row_cnt - 1) != 1) {
        res_err(res, "INT_ERR", "PK index update failed");
        return -1;
    }
    for (i = 0; i < tab->uk_cnt; i++) {
        char key[RECORD_SIZE];
        int idx = tab->uk_idx[i];

        norm_v(tab->rows[tab->row_cnt - 1].vals[idx], key, sizeof(key));
        if (key[0] == '\0') continue;
        if (bptree_string_insert(tab->uk_tree[i], key, tab->row_cnt - 1) != 1) {
            res_err(res, "INT_ERR", "UK index update failed");
            return -1;
        }
    }
    res->count = 1;
    res->last_id = new_id;
    return 0;
}

static int upd_do(TabVer *tab, Statement *stmt, DbRes *res) {
    int set_idx;
    int i;
    char set_val[256];

    if (stmt->where_count == 0) {
        res_err(res, "BAD_SQL", "UPDATE requires WHERE");
        return -1;
    }
    set_idx = col_idx(tab, stmt->set_col);
    if (set_idx < 0) {
        res_err(res, "BAD_SQL", "unknown SET column");
        return -1;
    }
    if (set_idx == tab->pk_idx) {
        res_err(res, "BAD_SQL", "PK update not allowed");
        return -1;
    }
    norm_v(stmt->set_val, set_val, sizeof(set_val));
    if (tab->cols[set_idx].type == COL_NN && set_val[0] == '\0') {
        res_err(res, "BAD_SQL", "NN violation");
        return -1;
    }
    for (i = 0; i < tab->row_cnt; i++) {
        if (!row_match(tab, stmt, &tab->rows[i])) continue;
        if (tab->cols[set_idx].type == COL_UK && chk_dup_uk(tab, set_idx, set_val, i)) {
            res_err(res, "BAD_SQL", "duplicate UK");
            return -1;
        }
    }
    for (i = 0; i < tab->row_cnt; i++) {
        char *oldv;

        if (!row_match(tab, stmt, &tab->rows[i])) continue;
        oldv = tab->rows[i].vals[set_idx];
        tab->rows[i].vals[set_idx] = dup_s(set_val);
        free(oldv);
        res->count++;
    }
    if (res->count == 0) return 0;
    if (tab->pk_tree) bptree_destroy(tab->pk_tree);
    tab->pk_tree = NULL;
    for (i = 0; i < tab->uk_cnt; i++) {
        if (tab->uk_tree[i]) bptree_string_destroy(tab->uk_tree[i]);
        tab->uk_tree[i] = NULL;
    }
    if (!idx_build(tab)) {
        res_err(res, "INT_ERR", "index rebuild failed");
        return -1;
    }
    return 0;
}

static int del_do(TabVer *tab, Statement *stmt, DbRes *res) {
    Row *rows;
    int row_cnt = 0;
    int i;

    if (stmt->where_count == 0) {
        res_err(res, "BAD_SQL", "DELETE requires WHERE");
        return -1;
    }
    rows = (Row *)calloc((size_t)tab->row_cnt, sizeof(Row));
    if (!rows) {
        res_err(res, "OOM", "out of memory");
        return -1;
    }
    for (i = 0; i < tab->row_cnt; i++) {
        if (row_match(tab, stmt, &tab->rows[i])) {
            res->count++;
            continue;
        }
        if (!row_copy(tab, &rows[row_cnt], &tab->rows[i])) {
            free_rows(rows, row_cnt, tab->col_cnt);
            res_err(res, "OOM", "out of memory");
            return -1;
        }
        row_cnt++;
    }
    for (i = 0; i < tab->row_cnt; i++) free_row(&tab->rows[i], tab->col_cnt);
    free(tab->rows);
    tab->rows = rows;
    tab->row_cnt = row_cnt;
    if (tab->pk_tree) bptree_destroy(tab->pk_tree);
    tab->pk_tree = NULL;
    for (i = 0; i < tab->uk_cnt; i++) {
        if (tab->uk_tree[i]) bptree_string_destroy(tab->uk_tree[i]);
        tab->uk_tree[i] = NULL;
    }
    if (!idx_build(tab)) {
        res_err(res, "INT_ERR", "index rebuild failed");
        return -1;
    }
    return 0;
}

static int exec_stmt(TabVer *tab, Statement *stmt, DbRes *res) {
    if (stmt->type == STMT_SELECT) return sel_do(tab, stmt, res);
    if (stmt->type == STMT_INSERT) return ins_do(tab, stmt, res);
    if (stmt->type == STMT_UPDATE) return upd_do(tab, stmt, res);
    if (stmt->type == STMT_DELETE) return del_do(tab, stmt, res);
    res_err(res, "BAD_SQL", "unsupported SQL");
    return -1;
}

static int parse_ok(Db *db, const char *sql, Statement *stmt, DbRes *res) {
    size_t len;

    len = strlen(sql);
    if (db->max_sql > 0 && (int)len > db->max_sql) {
        res_err(res, "TOO_BIG", "SQL too long");
        return 0;
    }
    if (!parse_statement(sql, stmt)) {
        res_err(res, "BAD_SQL", "parse failed");
        return 0;
    }
    return 1;
}

static int do_read(Db *db, unsigned long snap, const char *sql, DbRes *res) {
    Statement stmt;
    TabEnt *ent;
    TabVer *tab;
    int rc;

    res_clr(res);
    if (!parse_ok(db, sql, &stmt, res)) return -1;
    if (stmt.type != STMT_SELECT) {
        res_err(res, "BAD_SQL", "read path accepts SELECT only");
        return -1;
    }
    pthread_mutex_lock(&db->mu);
    ent = need_ent(db, stmt.table_name);
    tab = ent ? tab_vis(ent, snap) : NULL;
    pthread_mutex_unlock(&db->mu);
    if (!tab) {
        res_err(res, "BAD_SQL", "unknown table");
        return -1;
    }
    res->snap_id = snap;
    rc = exec_stmt(tab, &stmt, res);
    return rc;
}

static void prune_tab(TabEnt *ent, unsigned long keep_id) {
    TabVer *keep;

    if (!ent || !ent->head) return;
    keep = ent->head;
    if (keep_id == 0) {
        if (keep->prev) {
            free_chain(keep->prev);
            keep->prev = NULL;
        }
        return;
    }
    while (keep && keep->ver_id > keep_id) keep = keep->prev;
    if (!keep) return;
    if (keep->prev) {
        free_chain(keep->prev);
        keep->prev = NULL;
    }
}

static void gc_tabs(Db *db) {
    MvStat mst;
    unsigned long keep_id;
    int i;

    if (!db) return;
    mv_stat(db->mv, &mst);
    keep_id = mst.tx_live > 0 ? mst.snap_min : 0;
    pthread_mutex_lock(&db->mu);
    for (i = 0; i < db->tab_cnt; i++) prune_tab(&db->tabs[i], keep_id);
    pthread_mutex_unlock(&db->mu);
}

Db *db_open(const DbCfg *cfg) {
    Db *db;

    db = (Db *)calloc(1, sizeof(Db));
    if (!db) return NULL;
    pthread_mutex_init(&db->mu, NULL);
    db->mv = mv_new();
    if (!db->mv) {
        free(db);
        return NULL;
    }
    strncpy(db->root, (cfg && cfg->root) ? cfg->root : ".", sizeof(db->root) - 1);
    db->root[sizeof(db->root) - 1] = '\0';
    db->max_sql = cfg ? cfg->max_sql : 4096;
    db->max_qry = cfg ? cfg->max_qry : 32;
    return db;
}

void db_close(Db *db) {
    int i;

    if (!db) return;
    for (i = 0; i < db->tab_cnt; i++) {
        free(db->tabs[i].name);
        free_chain(db->tabs[i].head);
    }
    free(db->tabs);
    mv_del(db->mv);
    pthread_mutex_destroy(&db->mu);
    free(db);
}

DbSnap *db_snap(Db *db) {
    DbSnap *snap;

    if (!db) return NULL;
    snap = (DbSnap *)calloc(1, sizeof(DbSnap));
    if (!snap) return NULL;
    snap->snap_id = mv_snap(db->mv);
    if (snap->snap_id == 0) {
        free(snap);
        return NULL;
    }
    return snap;
}

void db_done(Db *db, DbSnap *snap) {
    if (!db || !snap) return;
    mv_gc(db->mv, snap->snap_id);
    gc_tabs(db);
    free(snap);
}

int db_read(Db *db, DbSnap *snap, const char *sql, DbRes *res) {
    if (!db || !snap || !sql || !res) return -1;
    return do_read(db, snap->snap_id, sql, res);
}

int db_begin(Db *db, DbTx **out) {
    DbTx *tx;

    if (!db || !out) return -1;
    tx = (DbTx *)calloc(1, sizeof(DbTx));
    if (!tx) return -1;
    tx->db = db;
    tx->snap_id = mv_snap(db->mv);
    if (tx->snap_id == 0) {
        free(tx);
        return -1;
    }
    *out = tx;
    return 0;
}

int db_commit(Db *db, DbTx *tx) {
    unsigned long ver;
    int i;

    if (!db || !tx || tx->done) return -1;
    pthread_mutex_lock(&db->mu);
    for (i = 0; i < tx->tab_cnt; i++) {
        if (!tx->tabs[i].dirty) continue;
        if (tx->tabs[i].ent->head != tx->tabs[i].base) {
            pthread_mutex_unlock(&db->mu);
            return -1;
        }
    }
    ver = mv_bump(db->mv);
    for (i = 0; i < tx->tab_cnt; i++) {
        if (!tx->tabs[i].dirty) continue;
        tx->tabs[i].work->ver_id = ver;
        tx->tabs[i].work->prev = tx->tabs[i].ent->head;
        tx->tabs[i].ent->head = tx->tabs[i].work;
        if (!tab_save(db, tx->tabs[i].work)) {
            pthread_mutex_unlock(&db->mu);
            return -1;
        }
        tx->tabs[i].work = NULL;
    }
    pthread_mutex_unlock(&db->mu);
    tx->done = 1;
    mv_gc(db->mv, tx->snap_id);
    gc_tabs(db);
    tx_free(tx);
    return 0;
}

int db_abort(Db *db, DbTx *tx) {
    int i;

    (void)db;
    if (!tx || tx->done) return -1;
    for (i = 0; i < tx->tab_cnt; i++) {
        free_tab(tx->tabs[i].work);
        tx->tabs[i].work = NULL;
    }
    tx->done = 1;
    mv_gc(tx->db->mv, tx->snap_id);
    gc_tabs(tx->db);
    tx_free(tx);
    return 0;
}

static void tx_free(DbTx *tx) {
    free(tx->tabs);
    free(tx);
}

int db_txdo(DbTx *tx, const char *sql, DbRes *res) {
    Statement stmt;
    TxTab *item;
    TabVer *tab;

    if (!tx || !sql || !res || tx->done) return -1;
    res_clr(res);
    if (!parse_ok(tx->db, sql, &stmt, res)) return -1;
    pthread_mutex_lock(&tx->db->mu);
    if (stmt_is_wr(&stmt)) item = tx_touch(tx, stmt.table_name);
    else item = tx_view(tx, stmt.table_name);
    pthread_mutex_unlock(&tx->db->mu);
    tab = item ? (item->work ? item->work : item->base) : NULL;
    if (!tab) {
        res_err(res, "BAD_SQL", "unknown table");
        return -1;
    }
    res->snap_id = tx->snap_id;
    return exec_stmt(tab, &stmt, res);
}

int db_exec(Db *db, const char *sql, DbRes *res) {
    Statement stmt;
    DbSnap *snap;
    DbTx *tx;
    TxTab *item;
    int rc;

    if (!db || !sql || !res) return -1;
    res_clr(res);
    if (!parse_ok(db, sql, &stmt, res)) return -1;
    if (stmt.type == STMT_SELECT) {
        snap = db_snap(db);
        if (!snap) {
            res_err(res, "OOM", "out of memory");
            return -1;
        }
        rc = db_read(db, snap, sql, res);
        db_done(db, snap);
        return rc;
    }
    if (db_begin(db, &tx) != 0) {
        res_err(res, "INT_ERR", "tx begin failed");
        return -1;
    }
    pthread_mutex_lock(&db->mu);
    item = tx_touch(tx, stmt.table_name);
    pthread_mutex_unlock(&db->mu);
    if (!item) {
        db_abort(db, tx);
        tx_free(tx);
        res_err(res, "BAD_SQL", "unknown table");
        return -1;
    }
    rc = exec_stmt(item->work, &stmt, res);
    if (rc == 0 && db_commit(db, tx) == 0) return 0;
    if (res->err.code[0] == '\0') res_err(res, "TX_ABORT", "write conflict or commit failure");
    db_abort(db, tx);
    return -1;
}

int db_batch(Db *db, const DbBat *bat, DbRes **resv, int *rcnt) {
    DbRes *out;
    int i;

    if (!db || !bat || !resv || !rcnt || bat->sqln < 0) return -1;
    *resv = NULL;
    *rcnt = 0;
    if (bat->sqln == 0) return 0;
    if (bat->sqln > db->max_qry) return -1;
    out = (DbRes *)calloc((size_t)bat->sqln, sizeof(DbRes));
    if (!out) return -1;
    if (bat->tx) {
        DbTx *tx;

        if (db_begin(db, &tx) != 0) {
            free(out);
            return -1;
        }
        for (i = 0; i < bat->sqln; i++) {
            if (db_txdo(tx, bat->sqlv[i], &out[i]) != 0) {
                char msg[256];

                strncpy(msg, out[i].err.msg ? out[i].err.msg : "query failed", sizeof(msg) - 1);
                msg[sizeof(msg) - 1] = '\0';
                res_err(&out[i], "TX_ABORT", msg);
                db_abort(db, tx);
                *resv = out;
                *rcnt = bat->sqln;
                return -1;
            }
        }
        if (db_commit(db, tx) != 0) {
            res_err(&out[bat->sqln - 1], "TX_ABORT", "commit failed");
            db_abort(db, tx);
            *resv = out;
            *rcnt = bat->sqln;
            return -1;
        }
        *resv = out;
        *rcnt = bat->sqln;
        return 0;
    }
    for (i = 0; i < bat->sqln; i++) {
        if (db_exec(db, bat->sqlv[i], &out[i]) != 0 && out[i].err.code[0] == '\0') {
            res_err(&out[i], "BAD_SQL", "query failed");
        }
    }
    *resv = out;
    *rcnt = bat->sqln;
    return 0;
}

int db_mst(Db *db, DbMst *mst) {

    if (!db || !mst) return -1;
    mv_stat(db->mv, (MvStat *)mst);
    return 0;
}
