#ifndef API_QUERY_CACHE_H
#define API_QUERY_CACHE_H

#include <stddef.h>

typedef enum {
    QUERY_CACHE_MISS_NONE = 0,
    QUERY_CACHE_MISS_NO_ENTRY = 1,
    QUERY_CACHE_MISS_TTL_EXPIRED = 2,
    QUERY_CACHE_MISS_VERSION_CHANGED = 3
} QueryCacheMissReason;

int query_cache_init(void);
void query_cache_destroy(void);
void query_cache_clear(void);
int query_cache_lookup(const char *key,
                       unsigned long long table_version,
                       char **body,
                       size_t *body_len,
                       QueryCacheMissReason *miss_reason);
void query_cache_store(const char *key,
                       unsigned long long table_version,
                       const char *body,
                       size_t body_len);

#endif
