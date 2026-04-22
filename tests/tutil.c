#include "tutil.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void t_die(const char *file, int line, const char *expr) {
    fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
    fflush(stderr);
    exit(1);
}

static int wr_txt(const char *path, const char *txt) {
    FILE *fp = fopen(path, "w");

    if (!fp) return 0;
    if (fputs(txt, fp) == EOF) {
        fclose(fp);
        return 0;
    }
    return fclose(fp) == 0;
}

int t_mktmp(char *buf, size_t bsz) {
    char tmp[] = "/tmp/sql3dbXXXXXX";
    char *dir;

    dir = mkdtemp(tmp);
    if (!dir || !buf || bsz == 0) return 0;
    strncpy(buf, dir, bsz - 1);
    buf[bsz - 1] = '\0';
    return 1;
}

int t_seed(const char *root) {
    char path[PATH_MAX];

    if (!root) return 0;

    snprintf(path, sizeof(path), "%s/users.csv", root);
    if (!wr_txt(path,
                "id(PK),email(UK),name(NN),age\n"
                "1,admin@test.com,Admin,30\n"
                "2,user1@test.com,UserOne,22\n"
                "3,user2@test.com,UserTwo,25\n")) return 0;

    snprintf(path, sizeof(path), "%s/restaurants.csv", root);
    if (!wr_txt(path,
                "id(PK),name(UK),zone(NN),kind,status\n"
                "1,HanBowl,seoul_east,korean,open\n"
                "2,TacoLoop,seoul_east,mexican,open\n"
                "3,NoodlePort,default,asian,open\n"
                "4,SaladDock,default,healthy,closed\n")) return 0;

    snprintf(path, sizeof(path), "%s/orders.csv", root);
    if (!wr_txt(path,
                "id(PK),user_id(NN),status(NN),total\n"
                "1,1,delivering,18000\n"
                "2,2,ready,9500\n"
                "3,3,done,22000\n")) return 0;

    snprintf(path, sizeof(path), "%s/coupons.csv", root);
    if (!wr_txt(path,
                "id(PK),code(UK),user_id(NN),status\n"
                "1,WELCOME10,1,ready\n"
                "2,SPRING15,1,used\n"
                "3,LUNCH5,2,ready\n")) return 0;

    snprintf(path, sizeof(path), "%s/cart.csv", root);
    if (!wr_txt(path,
                "id(PK),user_id(UK),item_count(NN),total\n"
                "1,1,2,21000\n"
                "2,2,1,9500\n"
                "3,3,3,33000\n")) return 0;

    return 1;
}
