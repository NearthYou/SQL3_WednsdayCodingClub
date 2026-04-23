#include "stats.h"

#include <stdatomic.h>
#include <string.h>
#include <time.h>

static _Atomic unsigned long long g_started_ns = 0;
static _Atomic unsigned long long g_total_requests = 0;
static _Atomic unsigned long long g_inflight_requests = 0;
static _Atomic unsigned long long g_ok_responses = 0;
static _Atomic unsigned long long g_error_responses = 0;
static _Atomic unsigned long long g_status_503 = 0;
static _Atomic unsigned long long g_keep_alive_reuse = 0;
static _Atomic unsigned long long g_cache_hits = 0;
static _Atomic unsigned long long g_cache_misses = 0;
static _Atomic unsigned long long g_parse_ns = 0;
static _Atomic unsigned long long g_lock_wait_ns = 0;
static _Atomic unsigned long long g_engine_ns = 0;
static _Atomic unsigned long long g_json_ns = 0;
static _Atomic unsigned long long g_send_ns = 0;

static unsigned long long now_ns(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ULL + (unsigned long long)ts.tv_nsec;
}

void api_stats_init(void) {
    atomic_store(&g_started_ns, now_ns());
    atomic_store(&g_total_requests, 0);
    atomic_store(&g_inflight_requests, 0);
    atomic_store(&g_ok_responses, 0);
    atomic_store(&g_error_responses, 0);
    atomic_store(&g_status_503, 0);
    atomic_store(&g_keep_alive_reuse, 0);
    atomic_store(&g_cache_hits, 0);
    atomic_store(&g_cache_misses, 0);
    atomic_store(&g_parse_ns, 0);
    atomic_store(&g_lock_wait_ns, 0);
    atomic_store(&g_engine_ns, 0);
    atomic_store(&g_json_ns, 0);
    atomic_store(&g_send_ns, 0);
}

void api_stats_destroy(void) {
}

void api_stats_note_request_started(void) {
    atomic_fetch_add(&g_total_requests, 1);
    atomic_fetch_add(&g_inflight_requests, 1);
}

void api_stats_note_request_finished(int status_code) {
    atomic_fetch_sub(&g_inflight_requests, 1);
    if (status_code >= 200 && status_code < 400) atomic_fetch_add(&g_ok_responses, 1);
    else if (status_code == 503) atomic_fetch_add(&g_status_503, 1);
    else atomic_fetch_add(&g_error_responses, 1);
}

void api_stats_note_immediate_503(void) {
    atomic_fetch_add(&g_total_requests, 1);
    atomic_fetch_add(&g_status_503, 1);
}

void api_stats_note_keep_alive_reuse(void) {
    atomic_fetch_add(&g_keep_alive_reuse, 1);
}

void api_stats_note_cache_hit(void) {
    atomic_fetch_add(&g_cache_hits, 1);
}

void api_stats_note_cache_miss(void) {
    atomic_fetch_add(&g_cache_misses, 1);
}

void api_stats_add_parse_ns(unsigned long long ns) {
    atomic_fetch_add(&g_parse_ns, ns);
}

void api_stats_add_lock_wait_ns(unsigned long long ns) {
    atomic_fetch_add(&g_lock_wait_ns, ns);
}

void api_stats_add_engine_ns(unsigned long long ns) {
    atomic_fetch_add(&g_engine_ns, ns);
}

void api_stats_add_json_ns(unsigned long long ns) {
    atomic_fetch_add(&g_json_ns, ns);
}

void api_stats_add_send_ns(unsigned long long ns) {
    atomic_fetch_add(&g_send_ns, ns);
}

void api_stats_snapshot(ApiStatsSnapshot *snapshot) {
    unsigned long long started_ns;
    unsigned long long current_ns;

    if (!snapshot) return;
    memset(snapshot, 0, sizeof(*snapshot));
    started_ns = atomic_load(&g_started_ns);
    current_ns = now_ns();
    if (started_ns > 0 && current_ns > started_ns) {
        snapshot->uptime_ms = (current_ns - started_ns) / 1000000ULL;
    }
    snapshot->total_requests = atomic_load(&g_total_requests);
    snapshot->inflight_requests = atomic_load(&g_inflight_requests);
    snapshot->ok_responses = atomic_load(&g_ok_responses);
    snapshot->error_responses = atomic_load(&g_error_responses);
    snapshot->status_503 = atomic_load(&g_status_503);
    snapshot->keep_alive_reuse = atomic_load(&g_keep_alive_reuse);
    snapshot->cache_hits = atomic_load(&g_cache_hits);
    snapshot->cache_misses = atomic_load(&g_cache_misses);
    snapshot->parse_ns = atomic_load(&g_parse_ns);
    snapshot->lock_wait_ns = atomic_load(&g_lock_wait_ns);
    snapshot->engine_ns = atomic_load(&g_engine_ns);
    snapshot->json_ns = atomic_load(&g_json_ns);
    snapshot->send_ns = atomic_load(&g_send_ns);
}
