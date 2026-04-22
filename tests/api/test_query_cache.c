#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/api/cache/query_cache.h"

int main(void) {
    char *body = NULL;
    size_t body_len = 0;
    const char *cached = "{\"status\":\"ok\",\"rows\":[]}";

    assert(query_cache_init() == 1);
    query_cache_clear();

    query_cache_store("SELECT * FROM users WHERE id = 1", 0, cached, strlen(cached));
    assert(query_cache_lookup("SELECT * FROM users WHERE id = 1", 0, &body, &body_len) == 1);
    assert(body != NULL);
    assert(body_len == strlen(cached));
    assert(strcmp(body, cached) == 0);
    free(body);

    assert(query_cache_lookup("SELECT * FROM users WHERE id = 1", 1, &body, &body_len) == 0);
    assert(body == NULL);

    query_cache_clear();
    assert(query_cache_lookup("SELECT * FROM users WHERE id = 1", 0, &body, &body_len) == 0);

    query_cache_destroy();
    printf("test_query_cache: OK\n");
    return 0;
}
