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

static char *dup_string(const char *s) {
    size_t len;
    char *out;
    if (!s) return NULL;
    len = strlen(s);
    out = (char *)malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, s, len + 1);
    return out;
}

static unsigned long hash_key(const char *s) {
    unsigned long hash = 1469598103934665603UL;
    const unsigned char *p = (const unsigned char *)(s ? s : "");

    while (*p) {
        hash ^= (unsigned long)(*p++);
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

static void unlink_from_bucket(int index) {
    unsigned long bucket;
    int cur;
    int prev = -1;

    if (index < 0 || index >= QUERY_CACHE_MAX_ENTRIES || !g_entries[index].in_use) return;
    bucket = g_entries[index].hash % QUERY_CACHE_BUCKETS;
    cur = g_bucket_heads[bucket];
    while (cur != -1) {
        if (cur == index) {
            if (prev == -1) g_bucket_heads[bucket] = g_entries[cur].next_hash;
            else g_entries[prev].next_hash = g_entries[cur].next_hash;
            g_entries[cur].next_hash = -1;
            return;
        }
        prev = cur;
        cur = g_entries[cur].next_hash;
    }
}

static void unlink_from_lru(int index) {
    int prev;
    int next;

    if (index < 0 || index >= QUERY_CACHE_MAX_ENTRIES || !g_entries[index].in_use) return;
    prev = g_entries[index].prev_lru;
    next = g_entries[index].next_lru;
    if (prev != -1) g_entries[prev].next_lru = next;
    else g_lru_head = next;
    if (next != -1) g_entries[next].prev_lru = prev;
    else g_lru_tail = prev;
    g_entries[index].prev_lru = -1;
    g_entries[index].next_lru = -1;
}

static void touch_lru_head(int index) {
    if (index < 0 || index >= QUERY_CACHE_MAX_ENTRIES || !g_entries[index].in_use) return;
    unlink_from_lru(index);
    g_entries[index].prev_lru = -1;
    g_entries[index].next_lru = g_lru_head;
    if (g_lru_head != -1) g_entries[g_lru_head].prev_lru = index;
    g_lru_head = index;
    if (g_lru_tail == -1) g_lru_tail = index;
}

static void remove_entry(int index) {
    if (index < 0 || index >= QUERY_CACHE_MAX_ENTRIES || !g_entries[index].in_use) return;
    unlink_from_bucket(index);
    unlink_from_lru(index);
    free_entry(&g_entries[index]);
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
    int cur;
    unsigned long bucket;

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
    int index;
    char *copy = NULL;

    if (body) *body = NULL;
    if (body_len) *body_len = 0;
    if (!key || key[0] == '\0') return 0;
    query_cache_init();

    hash = hash_key(key);
    pthread_mutex_lock(&g_query_cache_mutex);
    index = find_entry_index(key, hash);
    if (index == -1) {
        pthread_mutex_unlock(&g_query_cache_mutex);
        return 0;
    }
    if (g_entries[index].expire_at_ns <= now_ns() ||
        g_entries[index].table_version != table_version) {
        remove_entry(index);
        pthread_mutex_unlock(&g_query_cache_mutex);
        return 0;
    }
    copy = (char *)malloc(g_entries[index].body_len + 1);
    if (!copy) {
        pthread_mutex_unlock(&g_query_cache_mutex);
        return 0;
    }
    memcpy(copy, g_entries[index].body, g_entries[index].body_len);
    copy[g_entries[index].body_len] = '\0';
    touch_lru_head(index);
    if (body) *body = copy;
    else free(copy);
    if (body_len) *body_len = g_entries[index].body_len;
    pthread_mutex_unlock(&g_query_cache_mutex);
    return 1;
}

void query_cache_store(const char *key,
                       unsigned long long table_version,
                       const char *body,
                       size_t body_len) {
    unsigned long hash;
    int index;
    unsigned long bucket;

    if (!key || key[0] == '\0' || !body) return;
    if (body_len > QUERY_CACHE_MAX_BODY_BYTES) return;
    query_cache_init();

    hash = hash_key(key);
    pthread_mutex_lock(&g_query_cache_mutex);
    index = find_entry_index(key, hash);
    if (index == -1) {
        index = allocate_entry_index();
        if (index == -1) {
            pthread_mutex_unlock(&g_query_cache_mutex);
            return;
        }
        g_entries[index].in_use = 1;
        g_entries[index].hash = hash;
        g_entries[index].key = dup_string(key);
        if (!g_entries[index].key) {
            free_entry(&g_entries[index]);
            pthread_mutex_unlock(&g_query_cache_mutex);
            return;
        }
        bucket = hash % QUERY_CACHE_BUCKETS;
        g_entries[index].next_hash = g_bucket_heads[bucket];
        g_bucket_heads[bucket] = index;
        g_entry_count++;
    } else {
        free(g_entries[index].body);
        g_entries[index].body = NULL;
    }

    g_entries[index].body = (char *)malloc(body_len + 1);
    if (!g_entries[index].body) {
        remove_entry(index);
        pthread_mutex_unlock(&g_query_cache_mutex);
        return;
    }
    memcpy(g_entries[index].body, body, body_len);
    g_entries[index].body[body_len] = '\0';
    g_entries[index].body_len = body_len;
    g_entries[index].table_version = table_version;
    g_entries[index].expire_at_ns = now_ns() + QUERY_CACHE_TTL_NS;
    touch_lru_head(index);
    pthread_mutex_unlock(&g_query_cache_mutex);
}
