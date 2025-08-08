#include "relay.h"
#include "go.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

int main(void) {
    // Ensure deterministic, no-network run when possible
    setenv("NOSTR_TEST_MODE", "1", 1);

    GoContext *bg = go_context_background();
    Error *err = NULL;
    Relay *r = new_relay(bg, "wss://example.invalid", &err);
    if (!r) {
        fprintf(stderr, "new_relay failed: %s\n", err ? err->message : "unknown");
        return 1;
    }

    // Do not connect; just ensure free_relay returns promptly
    long long t0 = now_ms();
    free_relay(r);
    long long t1 = now_ms();

    long long dur = t1 - t0;
    if (dur > 2000) { // 2s threshold
        fprintf(stderr, "free_relay took too long: %lld ms\n", dur);
        return 2;
    }

    return 0;
}
