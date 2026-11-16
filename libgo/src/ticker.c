#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include <time.h>
#include "ticker.h"
#include <pthread.h>
#include <nsync_mu_wait.h>
#include <nsync_time.h>

static int ticker_stopped(const void *arg) {
    const Ticker *ticker = arg;
    return atomic_load_explicit(&ticker->stop, memory_order_acquire);
}

void *ticker_thread_func(void *arg) {
    Ticker *ticker = (Ticker *)arg;

    while (true) {
        if (atomic_load_explicit(&ticker->stop, memory_order_acquire)) {
            break;
        }

        nsync_mu_lock(&ticker->mutex);
        nsync_time deadline = nsync_time_add(nsync_time_now(), nsync_time_ms(ticker->interval_ms));
        (void)nsync_mu_wait_with_deadline(&ticker->mutex, ticker_stopped, ticker,
            NULL, deadline, NULL);
        bool stopped = ticker_stopped(ticker);
        nsync_mu_unlock(&ticker->mutex);
        if (stopped) break;

        // Non-blocking send; drop tick if channel is full
        (void)go_channel_try_send(ticker->c, NULL);
    }

    return NULL;
}

Ticker *create_ticker(size_t interval_ms) {
    /* nostrc-val0v: every step is checked and unwound. The old code ignored
     * malloc/channel/pthread_create failures, returning a ticker whose
     * thread never started; stop_ticker then joined an indeterminate
     * pthread_t. */
    Ticker *ticker = (Ticker *)malloc(sizeof(Ticker));
    if (!ticker) return NULL;
    ticker->interval_ms = interval_ms;
    ticker->c = go_channel_create(1); // Channel with capacity 1 for ticks
    if (!ticker->c) { free(ticker); return NULL; }
    atomic_store_explicit(&ticker->stop, false, memory_order_relaxed);
    nsync_mu_init(&ticker->mutex); // retained for struct compatibility

    if (pthread_create(&ticker->thread, NULL, ticker_thread_func, ticker) != 0) {
        go_channel_free(ticker->c);
        free(ticker);
        return NULL;
    }
    return ticker;
}

void stop_ticker(Ticker *ticker) {
    if (ticker == NULL) {
        return; // Ensure ticker is not NULL
    }

    nsync_mu_lock(&ticker->mutex);
    atomic_store_explicit(&ticker->stop, true, memory_order_release);
    nsync_mu_unlock(&ticker->mutex);

    if (ticker->thread) {
        (void)pthread_join(ticker->thread, NULL);
    }

    if (ticker->c) {
        go_channel_free(ticker->c);  // Ensure ticker channel is freed only if valid
    }
    free(ticker); // Free the ticker structure
}
