#ifndef API_STATS_H
#define API_STATS_H

typedef struct {
    unsigned long long uptime_ms;
    unsigned long long total_requests;
    unsigned long long inflight_requests;
    unsigned long long ok_responses;
    unsigned long long error_responses;
    unsigned long long status_503;
    unsigned long long keep_alive_reuse;
    unsigned long long cache_hits;
    unsigned long long cache_misses;
    unsigned long long parse_ns;
    unsigned long long lock_wait_ns;
    unsigned long long engine_ns;
    unsigned long long json_ns;
    unsigned long long send_ns;
} ApiStatsSnapshot;

void api_stats_init(void);
void api_stats_destroy(void);
void api_stats_note_request_started(void);
void api_stats_note_request_finished(int status_code);
void api_stats_note_immediate_503(void);
void api_stats_note_keep_alive_reuse(void);
void api_stats_note_cache_hit(void);
void api_stats_note_cache_miss(void);
void api_stats_add_parse_ns(unsigned long long ns);
void api_stats_add_lock_wait_ns(unsigned long long ns);
void api_stats_add_engine_ns(unsigned long long ns);
void api_stats_add_json_ns(unsigned long long ns);
void api_stats_add_send_ns(unsigned long long ns);
void api_stats_snapshot(ApiStatsSnapshot *snapshot);

#endif
