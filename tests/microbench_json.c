#include "nostr-event.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static unsigned long long now_us(void){
    struct timeval tv; gettimeofday(&tv, NULL);
    return (unsigned long long)tv.tv_sec * 1000000ull + (unsigned long long)tv.tv_usec;
}

static void usage(const char *arg0){
    fprintf(stderr, "Usage: %s [parse|serialize|roundtrip] [iterations]\n", arg0);
}

int main(int argc, char **argv){
    const char *mode = argc > 1 ? argv[1] : "roundtrip";
    long iters = argc > 2 ? strtol(argv[2], NULL, 10) : 100000;
    if (iters <= 0) iters = 100000;

    if (strcmp(mode, "parse") != 0 && strcmp(mode, "serialize") != 0 && strcmp(mode, "roundtrip") != 0){
        usage(argv[0]);
        return 2;
    }

    nostr_json_init();

    // Representative event JSON with unicode and tags
    const char *json = "{\"id\":\"\",\"pubkey\":\"0279BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798\",\"created_at\":1700000000,\"kind\":1,\"tags\":[[\"p\",\"abcdef\"],[\"e\",\"123456\"]],\"content\":\"hello \\uD83D\\uDE03 world\\nline2\",\"sig\":\"\"}";

    // Prebuild a typical event for serialize path
    NostrEvent *ev = nostr_event_new();
    if (!ev){ fprintf(stderr, "alloc fail\n"); return 3; }
    nostr_event_set_pubkey(ev, "0279BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798");
    nostr_event_set_created_at(ev, 1700000000);
    nostr_event_set_kind(ev, 1);
    nostr_event_set_content(ev, "hello \xF0\x9F\x98\x83 world\nline2");

    unsigned long long t0 = now_us();

    if (strcmp(mode, "parse") == 0){
        for (long i = 0; i < iters; ++i){
            NostrEvent *tmp = nostr_event_new();
            if (!tmp){ fprintf(stderr, "alloc fail\n"); return 3; }
            int ok = nostr_event_deserialize(tmp, json);
            if (!ok){ fprintf(stderr, "deserialize fail at i=%ld\n", i); return 4; }
            nostr_event_free(tmp);
        }
    } else if (strcmp(mode, "serialize") == 0){
        for (long i = 0; i < iters; ++i){
            char *s = nostr_event_serialize_compact(ev);
            if (!s){ fprintf(stderr, "serialize fail at i=%ld\n", i); return 5; }
            free(s);
        }
    } else { // roundtrip
        for (long i = 0; i < iters; ++i){
            NostrEvent *tmp = nostr_event_new();
            if (!tmp){ fprintf(stderr, "alloc fail\n"); return 3; }
            int ok = nostr_event_deserialize(tmp, json);
            if (!ok){ fprintf(stderr, "deserialize fail at i=%ld\n", i); return 4; }
            char *s = nostr_event_serialize_compact(tmp);
            if (!s){ fprintf(stderr, "serialize fail at i=%ld\n", i); return 5; }
            free(s);
            nostr_event_free(tmp);
        }
    }

    unsigned long long t1 = now_us();
    double secs = (t1 - t0) / 1000000.0;
    double ops = (double)iters / (secs > 0 ? secs : 1e-9);
    printf("mode=%s iters=%ld time=%.3fs ops/sec=%.0f us/op=%.2f\n", mode, iters, secs, ops, (t1 - t0)/(double)iters);

    nostr_event_free(ev);
    nostr_json_cleanup();
    return 0;
}
