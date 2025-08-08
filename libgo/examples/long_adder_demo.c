#include <stdio.h>
#include "go.h"
#include "counter.h"
#include "wait_group.h"

typedef struct { LongAdder *ad; int n; GoWaitGroup *wg; } Args;

static void *worker(void *arg){
    Args *a = (Args*)arg;
    for(int i=0;i<a->n;i++) long_adder_increment(a->ad);
    go_wait_group_done(a->wg);
    free(a);
    return NULL;
}

int main(){
    LongAdder *ad = long_adder_create();
    GoWaitGroup wg; go_wait_group_init(&wg);

    int threads = 4; int per=10000;
    go_wait_group_add(&wg, threads);
    for(int i=0;i<threads;i++){
        Args *args = (Args*)malloc(sizeof(Args));
        args->ad = ad; args->n = per; args->wg = &wg;
        go(worker, args);
    }

    go_wait_group_wait(&wg);
    printf("sum=%lld expected=%d\n", long_adder_sum(ad), threads*per);
    long_adder_destroy(ad);
    return 0;
}
