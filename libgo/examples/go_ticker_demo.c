#include <stdio.h>
#include <unistd.h>
#include "go.h"
#include "ticker.h"
#include "wait_group.h"

typedef struct { Ticker *t; GoWaitGroup *wg; int max_ticks; } Args;

void *tick_consumer(void *arg){
    Args *a = (Args*)arg;
    void *tick;
    int count = 0;
    while (count < a->max_ticks) {
        if (go_channel_receive(a->t->c, &tick) == 0) {
            printf("tick %d\n", ++count);
        }
    }
    go_wait_group_done(a->wg);
    return NULL;
}

int main(void){
    // Create a ticker that ticks every 50ms
    Ticker *t = create_ticker(50);

    // Consume 10 ticks on a goroutine
    GoWaitGroup wg; go_wait_group_init(&wg);
    go_wait_group_add(&wg, 1);
    Args a = { .t = t, .wg = &wg, .max_ticks = 10 };
    go(tick_consumer, &a);

    // Wait then stop ticker and exit
    go_wait_group_wait(&wg);
    stop_ticker(t);
    return 0;
}
