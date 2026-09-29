#include "peer_judge.h"

#include <stdio.h>

/** 在 PC 上跑 peer_judge_selfcheck，不烧板。失败打印一行并返回 1。 */
int main(void)
{
    if (!peer_judge_selfcheck()) {
        fprintf(stderr, "peer_judge_selfcheck failed\n");
        return 1;
    }
    printf("ok\n");
    return 0;
}
