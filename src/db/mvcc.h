#ifndef MVCC_H
#define MVCC_H

#include <stddef.h>

typedef struct Mvcc Mvcc;

typedef struct {
    unsigned long tx_live;
    unsigned long snap_min;
    unsigned long ver_now;
    unsigned long gc_wait;
} MvStat;

Mvcc *mv_new(void);
void mv_del(Mvcc *mv);
unsigned long mv_snap(Mvcc *mv);
unsigned long mv_bump(Mvcc *mv);
void mv_gc(Mvcc *mv, unsigned long snap);
void mv_stat(Mvcc *mv, MvStat *out);

#endif
