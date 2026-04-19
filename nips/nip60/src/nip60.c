#include "nostr/nip60/nip60.h"
#include "nostr-event.h"
#include "nostr-tag.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- Kind checking ---- */

bool nostr_nip60_is_wallet_kind(int kind) {
    return kind == NOSTR_NIP60_KIND_WALLET;
}

bool nostr_nip60_is_token_kind(int kind) {
    return kind == NOSTR_NIP60_KIND_TOKEN;
}

bool nostr_nip60_is_history_kind(int kind) {
    return kind == NOSTR_NIP60_KIND_HISTORY;
}

/* ---- Direction ---- */

int nostr_nip60_direction_parse(const char *str) {
    if (!str) return -1;
    if (strcmp(str, "in") == 0) return NOSTR_NIP60_DIR_IN;
    if (strcmp(str, "out") == 0) return NOSTR_NIP60_DIR_OUT;
    return -1;
}

const char *nostr_nip60_direction_string(NostrNip60Direction dir) {
    switch (dir) {
        case NOSTR_NIP60_DIR_IN:  return "in";
        case NOSTR_NIP60_DIR_OUT: return "out";
        default: return NULL;
    }
}

/* ---- Unit ---- */

NostrNip60Unit nostr_nip60_unit_parse(const char *str) {
    if (!str) return NOSTR_NIP60_UNIT_UNKNOWN;
    if (strcmp(str, "sat") == 0)  return NOSTR_NIP60_UNIT_SAT;
    if (strcmp(str, "msat") == 0) return NOSTR_NIP60_UNIT_MSAT;
    if (strcmp(str, "usd") == 0)  return NOSTR_NIP60_UNIT_USD;
    if (strcmp(str, "eur") == 0)  return NOSTR_NIP60_UNIT_EUR;
    return NOSTR_NIP60_UNIT_UNKNOWN;
}

const char *nostr_nip60_unit_string(NostrNip60Unit unit) {
    switch (unit) {
        case NOSTR_NIP60_UNIT_SAT:  return "sat";
        case NOSTR_NIP60_UNIT_MSAT: return "msat";
        case NOSTR_NIP60_UNIT_USD:  return "usd";
        case NOSTR_NIP60_UNIT_EUR:  return "eur";
        default: return NULL;
    }
}

bool nostr_nip60_unit_is_valid(const char *str) {
    return nostr_nip60_unit_parse(str) != NOSTR_NIP60_UNIT_UNKNOWN;
}

/* ---- Amount formatting ---- */

char *nostr_nip60_format_amount(uint64_t amount, const char *unit) {
    if (!unit) return NULL;

    char buf[64];
    if (strcmp(unit, "msat") == 0) {
        /* Show as sats with msat remainder */
        uint64_t sats = amount / 1000;
        uint64_t msats = amount % 1000;
        if (msats > 0)
            snprintf(buf, sizeof(buf), "%llu.%03llu sat",
                     (unsigned long long)sats, (unsigned long long)msats);
        else
            snprintf(buf, sizeof(buf), "%llu sat", (unsigned long long)sats);
    } else if (strcmp(unit, "usd") == 0 || strcmp(unit, "eur") == 0) {
        /* Cents to dollars/euros */
        uint64_t whole = amount / 100;
        uint64_t cents = amount % 100;
        snprintf(buf, sizeof(buf), "%llu.%02llu %s",
                 (unsigned long long)whole, (unsigned long long)cents, unit);
    } else {
        snprintf(buf, sizeof(buf), "%llu %s",
                 (unsigned long long)amount, unit);
    }

    return strdup(buf);
}

/* ---- Token event helpers ---- */

static const char *find_tag_value(const NostrEvent *ev, const char *key) {
    const NostrTags *tags = nostr_event_get_tags(ev);
    size_t n = nostr_tags_size(tags);
    for (size_t i = 0; i < n; ++i) {
        const NostrTag *t = nostr_tags_get(tags, i);
        if (nostr_tag_size(t) >= 2 &&
            strcmp(nostr_tag_get(t, 0), key) == 0) {
            return nostr_tag_get(t, 1);
        }
    }
    return NULL;
}

int nostr_nip60_parse_token(const NostrEvent *ev, NostrNip60Token *out) {
    if (!ev || !out) return -EINVAL;
    if (nostr_event_get_kind(ev) != NOSTR_NIP60_KIND_TOKEN)
        return -EINVAL;

    memset(out, 0, sizeof(*out));
    out->mint = find_tag_value(ev, "mint");
    out->created_at = nostr_event_get_created_at(ev);
    return 0;
}

int nostr_nip60_create_token(NostrEvent *ev,
                              const char *encrypted_content,
                              const char *mint_url) {
    if (!ev || !encrypted_content) return -EINVAL;

    nostr_event_set_kind(ev, NOSTR_NIP60_KIND_TOKEN);
    nostr_event_set_created_at(ev, (int64_t)time(NULL));
    nostr_event_set_content(ev, encrypted_content);

    if (mint_url) {
        NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
        nostr_tags_append(tags, nostr_tag_new("mint", mint_url, NULL));
    }

    return 0;
}

/* ---- Simple JSON tag array parser ---- */

/*
 * Parse a JSON tags array like: [["direction","in"],["amount","100"]]
 * Extracts key-value pairs from inner arrays.
 * Returns tag values via callback.
 */
typedef void (*tag_visitor_fn)(const char *key, size_t klen,
                                const char *val, size_t vlen,
                                void *ctx);

static void parse_json_tags(const char *json, tag_visitor_fn visitor, void *ctx) {
    if (!json) return;

    const char *p = json;
    /* Find outer [ */
    while (*p && *p != '[') ++p;
    if (!*p) return;
    ++p;

    while (*p) {
        /* Skip whitespace and commas */
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',')
            ++p;
        if (*p == ']' || !*p) break;

        /* Expect inner [ */
        if (*p != '[') { ++p; continue; }
        ++p;

        /* Parse first string (key) */
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
        if (*p != '"') goto skip_inner;
        ++p;
        const char *key_start = p;
        while (*p && *p != '"') {
            if (*p == '\\' && *(p+1)) p += 2;
            else ++p;
        }
        size_t klen = (size_t)(p - key_start);
        if (*p == '"') ++p;

        /* Skip comma */
        while (*p == ' ' || *p == '\t' || *p == ',' || *p == '\n' || *p == '\r')
            ++p;

        /* Parse second string (value) */
        if (*p != '"') goto skip_inner;
        ++p;
        const char *val_start = p;
        while (*p && *p != '"') {
            if (*p == '\\' && *(p+1)) p += 2;
            else ++p;
        }
        size_t vlen = (size_t)(p - val_start);
        if (*p == '"') ++p;

        visitor(key_start, klen, val_start, vlen, ctx);

skip_inner:
        /* Skip to closing ] */
        while (*p && *p != ']') {
            if (*p == '"') {
                ++p;
                while (*p && *p != '"') {
                    if (*p == '\\' && *(p+1)) p += 2;
                    else ++p;
                }
                if (*p == '"') ++p;
            } else {
                ++p;
            }
        }
        if (*p == ']') ++p;
    }
}

/* ---- History parsing ---- */

typedef struct {
    NostrNip60HistoryEntry *entry;
    bool has_direction;
    bool has_amount;
} history_parse_ctx;

static void history_tag_visitor(const char *key, size_t klen,
                                 const char *val, size_t vlen,
                                 void *ctx) {
    history_parse_ctx *hctx = ctx;

    if (klen == 9 && strncmp(key, "direction", 9) == 0) {
        if (vlen == 2 && strncmp(val, "in", 2) == 0)
            hctx->entry->direction = NOSTR_NIP60_DIR_IN;
        else if (vlen == 3 && strncmp(val, "out", 3) == 0)
            hctx->entry->direction = NOSTR_NIP60_DIR_OUT;
        hctx->has_direction = true;
    } else if (klen == 6 && strncmp(key, "amount", 6) == 0) {
        /* Parse amount integer from val */
        char buf[32];
        size_t copy = vlen < sizeof(buf) - 1 ? vlen : sizeof(buf) - 1;
        memcpy(buf, val, copy);
        buf[copy] = '\0';
        hctx->entry->amount = strtoull(buf, NULL, 10);
        hctx->has_amount = true;
    } else if (klen == 4 && strncmp(key, "unit", 4) == 0) {
        /* We can't borrow into JSON since it may not be null-terminated
           at the right place, but unit is a well-known short string */
        /* Store the pointer - caller must be aware it points into tags_json */
        /* For safety, we check known values */
        if (vlen == 3 && strncmp(val, "sat", 3) == 0)
            hctx->entry->unit = "sat";
        else if (vlen == 4 && strncmp(val, "msat", 4) == 0)
            hctx->entry->unit = "msat";
        else if (vlen == 3 && strncmp(val, "usd", 3) == 0)
            hctx->entry->unit = "usd";
        else if (vlen == 3 && strncmp(val, "eur", 3) == 0)
            hctx->entry->unit = "eur";
    }
}

int nostr_nip60_parse_history_tags(const char *tags_json,
                                    NostrNip60HistoryEntry *out) {
    if (!tags_json || !out) return -EINVAL;
    memset(out, 0, sizeof(*out));

    history_parse_ctx ctx = { .entry = out, .has_direction = false, .has_amount = false };
    parse_json_tags(tags_json, history_tag_visitor, &ctx);

    if (!ctx.has_direction || !ctx.has_amount) return -EINVAL;
    return 0;
}

char *nostr_nip60_build_history_content(NostrNip60Direction direction,
                                         uint64_t amount,
                                         const char *unit) {
    const char *dir_str = nostr_nip60_direction_string(direction);
    if (!dir_str) return NULL;

    char amount_str[32];
    snprintf(amount_str, sizeof(amount_str), "%llu", (unsigned long long)amount);

    /* Build: [["direction","in"],["amount","100"]] or with optional unit */
    size_t needed = 64 + strlen(amount_str);
    if (unit) needed += 32 + strlen(unit);

    char *buf = malloc(needed);
    if (!buf) return NULL;

    if (unit) {
        snprintf(buf, needed,
                 "[[\"direction\",\"%s\"],[\"amount\",\"%s\"],[\"unit\",\"%s\"]]",
                 dir_str, amount_str, unit);
    } else {
        snprintf(buf, needed,
                 "[[\"direction\",\"%s\"],[\"amount\",\"%s\"]]",
                 dir_str, amount_str);
    }
    return buf;
}

int nostr_nip60_create_history(NostrEvent *ev,
                                const char *encrypted_content) {
    if (!ev || !encrypted_content) return -EINVAL;

    nostr_event_set_kind(ev, NOSTR_NIP60_KIND_HISTORY);
    nostr_event_set_created_at(ev, (int64_t)time(NULL));
    nostr_event_set_content(ev, encrypted_content);
    return 0;
}

/* ---- Wallet metadata helpers ---- */

typedef struct {
    char **mints;
    size_t max_mints;
    size_t count;
} mint_parse_ctx;

static void mint_tag_visitor(const char *key, size_t klen,
                              const char *val, size_t vlen,
                              void *ctx) {
    mint_parse_ctx *mctx = ctx;

    if (klen == 4 && strncmp(key, "mint", 4) == 0 &&
        mctx->count < mctx->max_mints) {
        char *mint = malloc(vlen + 1);
        if (mint) {
            memcpy(mint, val, vlen);
            mint[vlen] = '\0';
            mctx->mints[mctx->count++] = mint;
        }
    }
}

int nostr_nip60_parse_wallet_mints(const char *tags_json,
                                    char **mints,
                                    size_t max_mints,
                                    size_t *out_count) {
    if (!tags_json || !mints || !out_count) return -EINVAL;
    *out_count = 0;

    mint_parse_ctx ctx = { .mints = mints, .max_mints = max_mints, .count = 0 };
    parse_json_tags(tags_json, mint_tag_visitor, &ctx);
    *out_count = ctx.count;
    return 0;
}

char *nostr_nip60_build_wallet_content(const char **mint_urls,
                                        size_t n_mints) {
    if (!mint_urls && n_mints > 0) return NULL;

    /* Calculate size: [["mint","url1"],["mint","url2"]] */
    size_t needed = 3; /* "[]" + null */
    for (size_t i = 0; i < n_mints; ++i) {
        needed += 12 + strlen(mint_urls[i]); /* ["mint","..."], */
    }

    char *buf = malloc(needed);
    if (!buf) return NULL;

    char *p = buf;
    *p++ = '[';
    for (size_t i = 0; i < n_mints; ++i) {
        if (i > 0) *p++ = ',';
        int written = snprintf(p, needed - (size_t)(p - buf),
                                "[\"mint\",\"%s\"]", mint_urls[i]);
        p += written;
    }
    *p++ = ']';
    *p = '\0';
    return buf;
}

int nostr_nip60_create_wallet(NostrEvent *ev,
                               const char *encrypted_content) {
    if (!ev || !encrypted_content) return -EINVAL;

    nostr_event_set_kind(ev, NOSTR_NIP60_KIND_WALLET);
    nostr_event_set_created_at(ev, (int64_t)time(NULL));
    nostr_event_set_content(ev, encrypted_content);
    return 0;
}
