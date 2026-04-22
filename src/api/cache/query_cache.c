#include "query_cache.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define QUERY_CACHE_BUCKETS 2048
#define QUERY_CACHE_MAX_ENTRIES 1024
#define QUERY_CACHE_TTL_NS 1000000000ULL
#define QUERY_CACHE_MAX_BODY_BYTES (256U * 1024U)

typedef struct {
    int in_use;
    unsigned long hash;
    char *key;
    char *body;
    size_t body_len;
    unsigned long long table_version;
    unsigned long long expire_at_ns;
    int next_hash;
    int prev_lru;
    int next_lru;
} QueryCacheEntry;

static pthread_mutex_t g_query_cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_query_cache_initialized = 0;
static int g_bucket_heads[QUERY_CACHE_BUCKETS];
static QueryCacheEntry g_entries[QUERY_CACHE_MAX_ENTRIES];
static int g_lru_head = -1;
static int g_lru_tail = -1;
static int g_entry_count = 0;

static unsigned long long now_ns(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ULL + (unsigned long long)ts.tv_nsec;
}

static char *dup_string(const char *src) {
    char *dst;
    size_t len;

    if (!src) return NULL;
    len = strlen(src);
    dst = (char *)malloc(len + 1);
    if (!dst) return NULL;
    memcpy(dst, src, len + 1);
    return dst;
}

static unsigned long hash_key(const char *src) {
    unsigned long hash = 1469598103934665603UL;
    const unsigned char *ptr = (const unsigned char *)(src ? src : "");

    while (*ptr) {
        hash ^= (unsigned long)(*ptr++);
        hash *= 1099511628211UL;
    }
    return hash;
}

static void free_entry(QueryCacheEntry *entry) {
    if (!entry) return;
    free(entry->key);
    free(entry->body);
    memset(entry, 0, sizeof(*entry));
    entry->prev_lru = -1;
    entry->next_lru = -1;
    entry->next_hash = -1;
}

static void unlink_from_bucket(int idx) {
    unsigned long bucket;
    int cur;
    int prev = -1;

    if (idx < 0 || idx >= QUERY_CACHE_MAX_ENTRIES || !g_entries[idx].in_use) return;
    bucket = g_entries[idx].hash % QUERY_CACHE_BUCKETS;
    cur = g_bucket_heads[bucket];
    while (cur != -1) {
        if (cur == idx) {
            if (prev == -1) g_bucket_heads[bucket] = g_entries[cur].next_hash;
            else g_entries[prev].next_hash = g_entries[cur].next_hash;
            g_entries[cur].next_hash = -1;
            return;
        }
        prev = cur;
        cur = g_entries[cur].next_hash;
    }
}

static void unlink_from_lru(int idx) {
    int prev;
    int next;

    if (idx < 0 || idx >= QUERY_CACHE_MAX_ENTRIES || !g_entries[idx].in_use) return;
    prev = g_entries[idx].prev_lru;
    next = g_entries[idx].next_lru;
    if (prev != -1) g_entries[prev].next_lru = next;
    else g_lru_head = next;
    if (next != -1) g_entries[next].prev_lru = prev;
    else g_lru_tail = prev;
    g_entries[idx].prev_lru = -1;
    g_entries[idx].next_lru = -1;
}

static void touch_lru_head(int idx) {
    if (idx < 0 || idx >= QUERY_CACHE_MAX_ENTRIES || !g_entries[idx].in_use) return;
    unlink_from_lru(idx);
    g_entries[idx].prev_lru = -1;
    g_entries[idx].next_lru = g_lru_head;
    if (g_lru_head != -1) g_entries[g_lru_head].prev_lru = idx;
    g_lru_head = idx;
    if (g_lru_tail == -1) g_lru_tail = idx;
}

static void remove_entry(int idx) {
    if (idx < 0 || idx >= QUERY_CACHE_MAX_ENTRIES || !g_entries[idx].in_use) return;
    unlink_from_bucket(idx);
    unlink_from_lru(idx);
    free_entry(&g_entries[idx]);
    if (g_entry_count > 0) g_entry_count--;
}

static int allocate_entry_index(void) {
    int i;

    if (g_entry_count < QUERY_CACHE_MAX_ENTRIES) {
        for (i = 0; i < QUERY_CACHE_MAX_ENTRIES; i++) {
            if (!g_entries[i].in_use) return i;
        }
    }
    if (g_lru_tail != -1) {
        remove_entry(g_lru_tail);
        for (i = 0; i < QUERY_CACHE_MAX_ENTRIES; i++) {
            if (!g_entries[i].in_use) return i;
        }
    }
    return -1;
}

static int find_entry_index(const char *key, unsigned long hash) {
    unsigned long bucket;
    int cur;

    if (!key) return -1;
    bucket = hash % QUERY_CACHE_BUCKETS;
    cur = g_bucket_heads[bucket];
    while (cur != -1) {
        if (g_entries[cur].in_use &&
            g_entries[cur].hash == hash &&
            g_entries[cur].key &&
            strcmp(g_entries[cur].key, key) == 0) {
            return cur;
        }
        cur = g_entries[cur].next_hash;
    }
    return -1;
}

int query_cache_init(void) {
    int i;

    pthread_mutex_lock(&g_query_cache_mutex);
    if (!g_query_cache_initialized) {
        for (i = 0; i < QUERY_CACHE_BUCKETS; i++) g_bucket_heads[i] = -1;
        for (i = 0; i < QUERY_CACHE_MAX_ENTRIES; i++) {
            g_entries[i].prev_lru = -1;
            g_entries[i].next_lru = -1;
            g_entries[i].next_hash = -1;
        }
        g_lru_head = -1;
        g_lru_tail = -1;
        g_entry_count = 0;
        g_query_cache_initialized = 1;
    }
    pthread_mutex_unlock(&g_query_cache_mutex);
    return 1;
}

void query_cache_clear(void) {
    int i;

    pthread_mutex_lock(&g_query_cache_mutex);
    for (i = 0; i < QUERY_CACHE_MAX_ENTRIES; i++) {
        if (g_entries[i].in_use) free_entry(&g_entries[i]);
    }
    for (i = 0; i < QUERY_CACHE_BUCKETS; i++) g_bucket_heads[i] = -1;
    g_lru_head = -1;
    g_lru_tail = -1;
    g_entry_count = 0;
    pthread_mutex_unlock(&g_query_cache_mutex);
}

void query_cache_destroy(void) {
    query_cache_clear();
    pthread_mutex_lock(&g_query_cache_mutex);
    g_query_cache_initialized = 0;
    pthread_mutex_unlock(&g_query_cache_mutex);
}

int query_cache_lookup(const char *key,
                       unsigned long long table_version,
                       char **body,
                       size_t *body_len) {
    unsigned long hash;
    int idx;
    char *copy = NULL;

    if (body) *body = NULL;
    if (body_len) *body_len = 0;
    if (!key || key[0] == '\0') return 0;
    query_cache_init();

    hash = hash_key(key);
    pthread_mutex_lock(&g_query_cache_mutex);
    idx = find_entry_index(key, hash);
    if (idx == -1) {
        pthread_mutex_unlock(&g_query_cache_mutex);
        return 0;
    }
    if (g_entries[idx].expire_at_ns <= now_ns() || g_entries[idx].table_version != table_version) {
        remove_entry(idx);
        pthread_mutex_unlock(&g_query_cache_mutex);
        return 0;
    }
    copy = (char *)malloc(g_entries[idx].body_len + 1);
    if (!copy) {
        pthread_mutex_unlock(&g_query_cache_mutex);
        return 0;
    }
    memcpy(copy, g_entries[idx].body, g_entries[idx].body_len);
    copy[g_entries[idx].body_len] = '\0';
    touch_lru_head(idx);
    if (body) *body = copy;
    else free(copy);
    if (body_len) *body_len = g_entries[idx].body_len;
    pthread_mutex_unlock(&g_query_cache_mutex);
    return 1;
}

void query_cache_store(const char *key,
                       unsigned long long table_version,
                       const char *body,
                       size_t body_len) {
    unsigned long hash;
    unsigned long bucket;
    int idx;

    if (!key || key[0] == '\0' || !body) return;
    if (body_len > QUERY_CACHE_MAX_BODY_BYTES) return;
    query_cache_init();

    hash = hash_key(key);
    pthread_mutex_lock(&g_query_cache_mutex);
    idx = find_entry_index(key, hash);
    if (idx == -1) {
        idx = allocate_entry_index();
        if (idx == -1) {
            pthread_mutex_unlock(&g_query_cache_mutex);
            return;
        }
        g_entries[idx].in_use = 1;
        g_entries[idx].hash = hash;
        g_entries[idx].key = dup_string(key);
        if (!g_entries[idx].key) {
            free_entry(&g_entries[idx]);
            pthread_mutex_unlock(&g_query_cache_mutex);
            return;
        }
        bucket = hash % QUERY_CACHE_BUCKETS;
        g_entries[idx].next_hash = g_bucket_heads[bucket];
        g_bucket_heads[bucket] = idx;
        g_entry_count++;
    } else {
        free(g_entries[idx].body);
        g_entries[idx].body = NULL;
    }

    g_entries[idx].body = (char *)malloc(body_len + 1);
    if (!g_entries[idx].body) {
        remove_entry(idx);
        pthread_mutex_unlock(&g_query_cache_mutex);
        return;
    }
    memcpy(g_entries[idx].body, body, body_len);
    g_entries[idx].body[body_len] = '\0';
    g_entries[idx].body_len = body_len;
    g_entries[idx].table_version = table_version;
    g_entries[idx].expire_at_ns = now_ns() + QUERY_CACHE_TTL_NS;
    touch_lru_head(idx);
    pthread_mutex_unlock(&g_query_cache_mutex);
}
