/* Regression for nostrc-3ulf2: deliberately link libgo before go_fiber. */
#define _POSIX_C_SOURCE 200809L
#include "go.h"
#include "libgo/fiber.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

static GoChannel *channel;
static atomic_int receiving;
static atomic_int progressed;
static int message;

static void receiver(void *arg) {
    (void)arg;
    assert(gof_in_fiber());
    atomic_store(&receiving, 1);
    void *value = NULL;
    assert(go_channel_receive(channel, &value) == 0);
    assert(value == &message);
    assert(atomic_load(&progressed) == 1);
}

static void sender(void *arg) {
    (void)arg;
    assert(gof_in_fiber());
    /* The receiver ran first. Reaching here proves it released the sole
     * worker rather than entering the OS-thread condition-variable wait. */
    assert(atomic_load(&receiving) == 1);
    atomic_store(&progressed, 1);
    assert(go_channel_send(channel, &message) == 0);
}

int main(void) {
    assert(setenv("GOF_NWORKERS", "1", 1) == 0);
    gof_init(0);
    channel = go_channel_create(1);
    assert(channel);
    /* Also verify the existing spawn registration remains functional. */
    assert(go_fiber(receiver, NULL, 0) == 0);
    assert(go_fiber(sender, NULL, 0) == 0);
    gof_run();
    assert(atomic_load(&progressed) == 1);
    go_channel_free(channel);
    puts("libgo before go_fiber: receiver parked, single worker progressed");
    return 0;
}
