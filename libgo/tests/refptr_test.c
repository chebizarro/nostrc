#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "refptr.h"
#include "go.h"
#include "wait_group.h"

static atomic_int dtor_calls = 0;
static void dtor(void *p){
    atomic_fetch_add(&dtor_calls, 1);
    free(p);
}

typedef struct { GoRefPtr *r; GoWaitGroup *wg; } Args;

static void *retainer(void *arg){
    Args *a = (Args*)arg;
    // retain and release a few times
    for(int i=0;i<1000;i++){
        go_refptr_retain(a->r);
        go_refptr_release(a->r);
    }
    go_wait_group_done(a->wg);
    free(a);
    return NULL;
}

int main(void){
    // make a refptr and share across threads
    go_auto(GoRefPtr) r = make_go_refptr(malloc(64), dtor);
    strcpy((char*)r.ptr, "refptr");

    GoWaitGroup wg; go_wait_group_init(&wg);
    int threads = 4;
    go_wait_group_add(&wg, threads);
    for(int i=0;i<threads;i++){
        Args *a = (Args*)malloc(sizeof(Args));
        a->r = &r; a->wg = &wg;
        go(retainer, a);
    }
    go_wait_group_wait(&wg);

    // Drop our own reference; destructor must run exactly once
    go_refptr_release(&r);

    int calls = atomic_load(&dtor_calls);
    if (calls != 1){
        fprintf(stderr, "expected dtor once, got %d\n", calls);
        return 1;
    }

    // go_autostr should auto free; avoid strdup under -Werror/-std=c11
    char *tmp = (char*)malloc(5);
    if (tmp) { strcpy(tmp, "auto"); }
    go_autostr s = tmp;
    (void)s;

    return 0;
}
