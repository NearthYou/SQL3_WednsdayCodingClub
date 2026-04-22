#ifndef TUTIL_H
#define TUTIL_H

#include <stddef.h>

void t_die(const char *file, int line, const char *expr);
int t_mktmp(char *buf, size_t bsz);
int t_seed(const char *root);

#define T_OK(expr) do { \
    if (!(expr)) t_die(__FILE__, __LINE__, #expr); \
} while (0)

#endif
