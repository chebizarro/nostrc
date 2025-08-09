#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "json.h"
#include "event.h"
#include "tag.h"
#include "nostr-event.h"

static unsigned rnd(unsigned n) { return (unsigned)rand() % (n ? n : 1); }

typedef struct { char *buf; size_t len; size_t cap; } sb_t;
static void sb_init(sb_t *sb) { sb->buf = NULL; sb->len = 0; sb->cap = 0; }
static void sb_grow(sb_t *sb, size_t need) {
    if (sb->len + need + 1 > sb->cap) {
        size_t ncap = sb->cap ? sb->cap * 2 : 256;
        while (sb->len + need + 1 > ncap) ncap *= 2;
        sb->buf = (char*)realloc(sb->buf, ncap);
        sb->cap = ncap;
    }
}
static void sb_putch(sb_t *sb, char c) { sb_grow(sb, 1); sb->buf[sb->len++] = c; sb->buf[sb->len] = '\0'; }
static void sb_puts(sb_t *sb, const char *s) { size_t n = strlen(s); sb_grow(sb, n); memcpy(sb->buf + sb->len, s, n); sb->len += n; sb->buf[sb->len] = '\0'; }
static void sb_putu(sb_t *sb, unsigned v) { char tmp[32]; int n = snprintf(tmp, sizeof(tmp), "%u", v); (void)n; sb_puts(sb, tmp); }
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
    for (int i = 0; i < len; i++) s[i] = 'a' + (rnd(26));
    s[len] = '\0';
    return s;
}

static void emit_unknown_nested(sb_t *sb, int depth) {
    if (depth <= 0) { sb_putqstr(sb, "leaf"); sb_putch(sb, ':'); sb_putqstr(sb, "x"); return; }
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

static void build_random_event_json_string(char **out) {
    sb_t sb; sb_init(&sb);
    sb_putch(&sb, '{');
    int emitted = 0;
    #define COMMA() do { if (emitted++) sb_putch(&sb, ','); } while (0)
    #define KEY(k) do { COMMA(); sb_putqstr(&sb, k); sb_putch(&sb, ':'); } while (0)

    // kind: int or mistakenly string
    if (rnd(2)) { KEY("kind"); if (rnd(4)==0) { char t[8]; rand_ascii(t, 1 + rnd(3)); sb_putqstr(&sb, t); } else { sb_putu(&sb, (unsigned)rnd(50000)); } }
    // created_at
    if (rnd(2)) { KEY("created_at"); sb_putu(&sb, (unsigned)(1000000000 + rnd(1000000000))); }
    // pubkey
    if (rnd(2)) { KEY("pubkey"); char pk[65]; rand_ascii(pk, 32 + rnd(32)); sb_putqstr(&sb, pk); }
    // content (possibly very long)
    if (rnd(2)) { KEY("content"); if (rnd(6)==0) { char *ls = alloc_long_ascii(60000 + rnd(2000)); sb_putqstr(&sb, ls); free(ls);} else { char c[32]; rand_ascii(c, 1 + rnd(30)); sb_putqstr(&sb, c);} }

    // tags: array of arrays; inject some malformed elements sometimes
    if (rnd(2)) {
        KEY("tags"); sb_putch(&sb, '[');
        int n = rnd(10);
        for (int i = 0; i < n; i++) {
            if (i) sb_putch(&sb, ',');
            if (rnd(7)==0) { // malformed tag entry
                sb_putu(&sb, (unsigned)rnd(100));
            } else {
                sb_putch(&sb, '[');
                // name
                char name[2] = { (char)('a' + rnd(26)), '\0' };
                sb_putqstr(&sb, name);
                // value(s)
                int parts = 1 + rnd(2);
                for (int j = 0; j < parts; j++) { sb_putch(&sb, ','); char val[16]; rand_ascii(val, 1 + rnd(14)); sb_putqstr(&sb, val);} 
                sb_putch(&sb, ']');
            }
        }
        sb_putch(&sb, ']');
    }

    // id (randomly string or wrong type)
    if (rnd(2)) { KEY("id"); if (rnd(5)==0) sb_putu(&sb, (unsigned)rnd(1000)); else { char id[65]; rand_ascii(id, 32 + rnd(32)); sb_putqstr(&sb, id);} }

    // unknown deep-nested
    if (rnd(2)) { KEY("_unknown"); sb_putch(&sb, '{'); emit_unknown_nested(&sb, 3 + rnd(3)); sb_putch(&sb, '}'); }

    sb_putch(&sb, '}');
    *out = sb.buf;
}

int main(void) {
    srand((unsigned)time(NULL));
    extern NostrJsonInterface *jansson_impl;
    nostr_set_json_interface(jansson_impl);
    nostr_json_init();

    for (int i = 0; i < 800; i++) {
        char *s = NULL; build_random_event_json_string(&s);
        NostrEvent *e = nostr_event_new(); assert(e);
        (void)nostr_event_deserialize(e, s);
        nostr_event_free(e);
        free(s);
    }

    nostr_json_cleanup();
    printf("test_json_event_fuzzlite OK\n");
    return 0;
}
