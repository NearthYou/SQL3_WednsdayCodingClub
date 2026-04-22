#ifndef API_QUERY_CACHE_H
#define API_QUERY_CACHE_H

#include <stddef.h>

int query_cache_init(void);
void query_cache_destroy(void);
void query_cache_clear(void);
int query_cache_lookup(const char *key,
                       unsigned long long table_version,
                       char **body,
                       size_t *body_len);
void query_cache_store(const char *key,
                       unsigned long long table_version,
                       const char *body,
                       size_t body_len);

#endif
