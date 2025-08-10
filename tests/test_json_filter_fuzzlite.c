#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include "json.h"
#include "nostr_jansson.h"
#include "nostr-filter.h"

static int rnd(int n) { return rand() % n; }

typedef struct {
    char *buf; size_t len; size_t cap;
} sb_t;

static void sb_init(sb_t *sb) { sb->buf = NULL; sb->len = 0; sb->cap = 0; }
static void sb_grow(sb_t *sb, size_t need) {
    if (sb->len + need + 1 > sb->cap) {
        size_t ncap = (sb->cap ? sb->cap * 2 : 256);
        while (sb->len + need + 1 > ncap) ncap *= 2;
        sb->buf = (char*)realloc(sb->buf, ncap);
        sb->cap = ncap;
    }
}
static void sb_putch(sb_t *sb, char c) { sb_grow(sb, 1); sb->buf[sb->len++] = c; sb->buf[sb->len] = '\0'; }
static void sb_puts(sb_t *sb, const char *s) { size_t n = strlen(s); sb_grow(sb, n); memcpy(sb->buf + sb->len, s, n); sb->len += n; sb->buf[sb->len] = '\0'; }
static void sb_putu(sb_t *sb, unsigned v) { char tmp[32]; int n = snprintf(tmp, sizeof(tmp), "%u", v); sb_puts(sb, tmp); }
static void sb_putqstr(sb_t *sb, const char *s) { sb_putch(sb, '"'); sb_puts(sb, s); sb_putch(sb, '"'); }

static void rand_ascii(char *out, int len) {
    for (int i = 0; i < len; i++) {
        int r = rnd(36);
        out[i] = (r < 10) ? ('0' + r) : ('a' + (r - 10));
    }
    out[len] = '\0';
}

static char *alloc_long_ascii(int len) {
    char *s = (char*)malloc((size_t)len + 1);
    for (int i = 0; i < len; i++) {
        int r = rnd(26);
        s[i] = 'a' + r;
    }
    s[len] = '\0';
    return s;
}

static void emit_unknown_nested(sb_t *sb, int depth) {
    if (depth <= 0) {
        sb_putqstr(sb, "leaf"); sb_putch(sb, ':'); sb_putqstr(sb, "x");
        return;
    }
    sb_putqstr(sb, depth % 2 ? "obj" : "arr"); sb_putch(sb, ':');
    if (depth % 2) {
        sb_putch(sb, '{');
        sb_putqstr(sb, "k"); sb_putch(sb, ':'); sb_putu(sb, (unsigned)rnd(1000)); sb_putch(sb, ',');
        emit_unknown_nested(sb, depth - 1);
        sb_putch(sb, '}');
    } else {
        sb_putch(sb, '[');
        sb_putu(sb, (unsigned)rnd(100)); sb_putch(sb, ',');
        sb_putqstr(sb, "y"); sb_putch(sb, ',');
        sb_putch(sb, '{'); emit_unknown_nested(sb, depth - 1); sb_putch(sb, '}');
        sb_putch(sb, ']');
    }
}

static void build_random_filter_json_string(char **out) {
    sb_t sb; sb_init(&sb);
    int allow_bad = rnd(2);
    sb_putch(&sb, '{');

    int emitted = 0;
    // helper lambdas in C style
    #define COMMA() do { if (emitted++) sb_putch(&sb, ','); } while (0)
    #define KEY(k) do { COMMA(); sb_putqstr(&sb, k); sb_putch(&sb, ':'); } while (0)

    if (rnd(2)) {
        KEY("ids"); sb_putch(&sb, '[');
        int n = rnd(10);
        for (int i = 0; i < n; i++) {
            if (i) sb_putch(&sb, ',');
            if (allow_bad && rnd(5) == 0) { sb_putu(&sb, (unsigned)rnd(1000)); }
            else { char tmp[16]; rand_ascii(tmp, 1 + rnd(14)); sb_putqstr(&sb, tmp); }
        }
        sb_putch(&sb, ']');
    }

    if (rnd(2)) {
        KEY("authors"); sb_putch(&sb, '[');
        int n = rnd(10);
        for (int i = 0; i < n; i++) {
            if (i) sb_putch(&sb, ',');
            if (allow_bad && rnd(5) == 0) { sb_putu(&sb, (unsigned)rnd(1000)); }
            else { char tmp[16]; rand_ascii(tmp, 1 + rnd(14)); sb_putqstr(&sb, tmp); }
        }
        sb_putch(&sb, ']');
    }

    if (rnd(2)) {
        KEY("kinds"); sb_putch(&sb, '[');
        int n = rnd(10);
        for (int i = 0; i < n; i++) {
            if (i) sb_putch(&sb, ',');
            if (allow_bad && rnd(5) == 0) { char tmp[16]; rand_ascii(tmp, 1 + rnd(14)); sb_putqstr(&sb, tmp); }
            else { sb_putu(&sb, (unsigned)rnd(50000)); }
        }
        sb_putch(&sb, ']');
    }

    if (rnd(2)) { KEY("since"); sb_putu(&sb, (unsigned)rnd(2000000000)); }
    if (rnd(2)) { KEY("until"); sb_putu(&sb, (unsigned)rnd(2000000000)); }
    if (rnd(2)) { KEY("limit"); sb_putu(&sb, (unsigned)rnd(1000)); }
    if (rnd(2)) { KEY("search"); char tmp[16]; rand_ascii(tmp, 1 + rnd(14)); sb_putqstr(&sb, tmp); }
    // occasionally stress with very long search strings
    if (rnd(6) == 0) { KEY("search"); char *ls = alloc_long_ascii(60000 + rnd(1000)); sb_putqstr(&sb, ls); free(ls); }

    // dynamic tag keys
    const char letters[] = "epabcd";
    int tags = rnd(4);
    for (int i = 0; i < tags; i++) {
        char key[3] = {'#', letters[rnd((int)sizeof(letters)-1)], '\0'};
        KEY(key); sb_putch(&sb, '[');
        int m = rnd(5);
        for (int j = 0; j < m; j++) {
            if (j) sb_putch(&sb, ',');
            if (allow_bad && rnd(6) == 0) { sb_putu(&sb, (unsigned)rnd(1000)); }
            else { char tmp[16]; rand_ascii(tmp, 1 + rnd(14)); sb_putqstr(&sb, tmp); }
        }
        sb_putch(&sb, ']');
    }

    // sometimes add legacy tags array-of-arrays
    if (rnd(3) == 0) {
        KEY("tags"); sb_putch(&sb, '[');
        int n = rnd(5);
        for (int i = 0; i < n; i++) {
            if (i) sb_putch(&sb, ',');
            sb_putch(&sb, '[');
            char t0[8]; char t1[16]; rand_ascii(t0, 1); rand_ascii(t1, 1 + rnd(14));
            sb_putqstr(&sb, t0); sb_putch(&sb, ','); sb_putqstr(&sb, t1);
            sb_putch(&sb, ']');
        }
        sb_putch(&sb, ']');
    }

    // sometimes add unknown deep-nested keys to ensure robust skipping
    if (rnd(2)) {
        KEY("_unknown"); sb_putch(&sb, '{');
        emit_unknown_nested(&sb, 3 + rnd(3));
        sb_putch(&sb, '}');
    }

    sb_putch(&sb, '}');
    *out = sb.buf;
}

int main(void) {
    srand((unsigned)time(NULL));
    nostr_json_init();

    for (int i = 0; i < 1000; i++) {
        char *s = NULL;
        build_random_filter_json_string(&s);
        NostrFilter *f = nostr_filter_new();
        assert(f);
        // We don't assert success; just ensure no crashes/leaks under sanitizers.
        (void)nostr_filter_deserialize(f, s);
        nostr_filter_free(f);
        free(s);
    }

    nostr_json_cleanup();
    printf("test_json_filter_fuzzlite OK\n");
    return 0;
}
