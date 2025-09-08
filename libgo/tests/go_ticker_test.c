#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <stdatomic.h>
#include "go.h"
#include "ticker.h"

static inline void sleep_ms(int ms){ struct timespec ts={ ms/1000, (long)(ms%1000)*1000000L }; nanosleep(&ts, NULL); }

typedef struct {
    Ticker *ticker;
    int target;
    _Atomic int count;
} TickCounter;

void *consumer_thread(void *arg) {
    TickCounter *tc = (TickCounter *)arg;
    void *data = NULL;
    while (atomic_load_explicit(&tc->count, memory_order_acquire) < tc->target) {
        if (go_channel_try_receive(tc->ticker->c, &data) == 0) {
            atomic_fetch_add_explicit(&tc->count, 1, memory_order_acq_rel);
        } else {
            // avoid busy spin
            sleep_ms(5);
            // If ticker has been stopped and channel is closed, exit to allow join
            if (atomic_load_explicit(&tc->ticker->stop, memory_order_acquire) &&
                go_channel_is_closed(tc->ticker->c)) {
                break;
            }
        }
    }
    return NULL;
}

int main(void) {
    // Create a ticker that ticks every 50ms
    Ticker *t = create_ticker(50);
    if (!t) {
        fprintf(stderr, "failed to create ticker\n");
        return 1;
    }

    TickCounter tc = { .ticker = t, .target = 5, .count = 0 };
    pthread_t th;
    pthread_create(&th, NULL, consumer_thread, &tc);

    // Wait for ticks with a generous deadline under sanitizers
    const int max_ms =
#if defined(__has_feature)
#  if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer) || __has_feature(undefined_behavior_sanitizer)
       5000
#  else
       2000
#  endif
#elif defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_UNDEFINED__)
       5000
#else
       2000
#endif
    ;
    int elapsed_ms = 0;
    while (atomic_load_explicit(&tc.count, memory_order_acquire) < tc.target && elapsed_ms < max_ms) {
        sleep_ms(50);
        elapsed_ms += 50;
    }

    stop_ticker(t);
    pthread_join(th, NULL);

    int final = atomic_load_explicit(&tc.count, memory_order_acquire);
    if (final < tc.target) {
        fprintf(stderr, "ticker produced only %d ticks within deadline\n", final);
        return 2;
    }

    printf("received %d ticks\n", final);
    return 0;
}
