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
    GoChannel *ch;      // only the channel, never dereference Ticker in consumer
    _Atomic int count;  // how many ticks consumed
    int target;         // goal
    _Atomic int shutdown; // signal to exit without touching freed structures
} TickCounter;

void *consumer_thread(void *arg) {
    TickCounter *tc = (TickCounter *)arg;
    void *data = NULL;
    while (atomic_load_explicit(&tc->count, memory_order_acquire) < tc->target) {
#if defined(__has_feature)
#  if __has_feature(thread_sanitizer)
        // Under TSAN, use blocking receive to avoid missing ticks due to scheduling jitter
        if (go_channel_receive(tc->ch, &data) == 0) {
            atomic_fetch_add_explicit(&tc->count, 1, memory_order_acq_rel);
        } else {
            // Channel closed or shutdown signaled
            break;
        }
#  else
        if (go_channel_try_receive(tc->ch, &data) == 0) {
            atomic_fetch_add_explicit(&tc->count, 1, memory_order_acq_rel);
        } else {
            sleep_ms(1);
            if (atomic_load_explicit(&tc->shutdown, memory_order_acquire)) break;
        }
#  endif
#else
        if (go_channel_try_receive(tc->ch, &data) == 0) {
            atomic_fetch_add_explicit(&tc->count, 1, memory_order_acq_rel);
        } else {
            sleep_ms(1);
            if (atomic_load_explicit(&tc->shutdown, memory_order_acquire)) break;
        }
#endif
    }
    return NULL;
}

int main(void) {
    // Create a ticker with configurable interval (default 50ms)
    const int tick_ms =
#ifdef TICK_MS
        TICK_MS;
#else
        50;
#endif
    Ticker *t = create_ticker(tick_ms);
    if (!t) {
        fprintf(stderr, "failed to create ticker\n");
        return 1;
    }

    int target_ticks = 5;
#if defined(__has_feature)
#  if __has_feature(thread_sanitizer)
    target_ticks = 3;
#  endif
#endif
    TickCounter tc = { .ch = t->c, .count = 0, .target = target_ticks, .shutdown = 0 };
    pthread_t th;
    pthread_create(&th, NULL, consumer_thread, &tc);
    // Give the ticker goroutine a brief warmup to start emitting under heavy sanitizer overhead
    sleep_ms(tick_ms * 2);

    // Wait for ticks with a generous deadline under sanitizers; allow CMake to override via MAX_TICK_WAIT_MS
    const int max_ms =
#ifdef MAX_TICK_WAIT_MS
        MAX_TICK_WAIT_MS
#else
#  if defined(__has_feature)
#    if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer) || __has_feature(undefined_behavior_sanitizer)
         5000
#    else
         2000
#    endif
#  elif defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_UNDEFINED__)
         5000
#  else
         2000
#  endif
#endif
    ;
    int elapsed_ms = 0;
    while (atomic_load_explicit(&tc.count, memory_order_acquire) < tc.target && elapsed_ms < max_ms) {
        sleep_ms(50);
        elapsed_ms += 50;
    }

    // Ask consumer to stop and wait for it BEFORE freeing ticker resources
    atomic_store_explicit(&tc.shutdown, 1, memory_order_release);
    pthread_join(th, NULL);
    stop_ticker(t);

    int final = atomic_load_explicit(&tc.count, memory_order_acquire);
    if (final < tc.target) {
        fprintf(stderr, "ticker produced only %d ticks within deadline\n", final);
        return 2;
    }

    printf("received %d ticks\n", final);
    return 0;
}
