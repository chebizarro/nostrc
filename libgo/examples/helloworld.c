#include <stdio.h>
#include "go.h"
#include "wait_group.h"

typedef struct { const char *msg; GoWaitGroup *wg; } MsgArgs;

void *print_message(void *arg) {
    MsgArgs *ma = (MsgArgs*)arg;
    printf("%s\n", ma->msg);
    go_wait_group_done(ma->wg);
    return NULL;
}

int main() {
    GoWaitGroup wg; go_wait_group_init(&wg);
    go_wait_group_add(&wg, 2);
    MsgArgs a = { .msg = "Hello from thread 1", .wg = &wg };
    MsgArgs b = { .msg = "Hello from thread 2", .wg = &wg };
    go(print_message, &a);
    go(print_message, &b);
    go_wait_group_wait(&wg);
    go_wait_group_destroy(&wg);
    return 0;
}
