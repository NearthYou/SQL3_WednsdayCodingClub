#include "mvcc.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct Mvcc {
    pthread_mutex_t mu;
    unsigned long cur_ver;
    unsigned long *snaps;
    int snap_cnt;
    int snap_cap;
};

static void snap_min(struct Mvcc *mv, unsigned long *out) {
    int i;

    *out = mv->cur_ver;
    if (mv->snap_cnt == 0) return;
    *out = mv->snaps[0];
    for (i = 1; i < mv->snap_cnt; i++) {
        if (mv->snaps[i] < *out) *out = mv->snaps[i];
    }
}

Mvcc *mv_new(void) {
    Mvcc *mv;

    mv = (Mvcc *)calloc(1, sizeof(Mvcc));
    if (!mv) return NULL;
    pthread_mutex_init(&mv->mu, NULL);
    mv->cur_ver = 1;
    return mv;
}

void mv_del(Mvcc *mv) {
    if (!mv) return;
    pthread_mutex_destroy(&mv->mu);
    free(mv->snaps);
    free(mv);
}

unsigned long mv_snap(Mvcc *mv) {
    unsigned long snap;

    if (!mv) return 0;
    pthread_mutex_lock(&mv->mu);
    if (mv->snap_cnt == mv->snap_cap) {
        int cap = mv->snap_cap == 0 ? 8 : mv->snap_cap * 2;
        unsigned long *nsnaps = (unsigned long *)realloc(mv->snaps, (size_t)cap * sizeof(unsigned long));
        if (!nsnaps) {
            pthread_mutex_unlock(&mv->mu);
            return 0;
        }
        mv->snaps = nsnaps;
        mv->snap_cap = cap;
    }
    snap = mv->cur_ver;
    mv->snaps[mv->snap_cnt++] = snap;
    pthread_mutex_unlock(&mv->mu);
    return snap;
}

unsigned long mv_bump(Mvcc *mv) {
    unsigned long ver;

    if (!mv) return 0;
    pthread_mutex_lock(&mv->mu);
    ver = ++mv->cur_ver;
    pthread_mutex_unlock(&mv->mu);
    return ver;
}

void mv_gc(Mvcc *mv, unsigned long snap) {
    int i;

    if (!mv || snap == 0) return;
    pthread_mutex_lock(&mv->mu);
    for (i = 0; i < mv->snap_cnt; i++) {
        if (mv->snaps[i] != snap) continue;
        memmove(&mv->snaps[i], &mv->snaps[i + 1], (size_t)(mv->snap_cnt - i - 1) * sizeof(unsigned long));
        mv->snap_cnt--;
        break;
    }
    pthread_mutex_unlock(&mv->mu);
}

void mv_stat(Mvcc *mv, MvStat *out) {
    unsigned long minv = 0;

    if (!mv || !out) return;
    pthread_mutex_lock(&mv->mu);
    memset(out, 0, sizeof(*out));
    out->tx_live = (unsigned long)mv->snap_cnt;
    out->ver_now = mv->cur_ver;
    snap_min(mv, &minv);
    out->snap_min = minv;
    out->gc_wait = mv->snap_cnt > 0 ? (mv->cur_ver - minv) : 0;
    pthread_mutex_unlock(&mv->mu);
}
