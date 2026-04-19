#include "nostr/nip61/nip61.h"
#include "nostr-event.h"
#include "nostr-tag.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- Internal tag helpers ---- */

static const char *tag_key(const NostrTag *t) {
    return nostr_tag_size(t) >= 1 ? nostr_tag_get(t, 0) : NULL;
}

static const char *tag_val(const NostrTag *t, size_t idx) {
    return nostr_tag_size(t) > idx ? nostr_tag_get(t, idx) : NULL;
}

/* ---- Preferences ---- */

int nostr_nip61_parse_prefs(const NostrEvent *ev,
                             NostrNip61Mint *mints, size_t max_mints,
                             const char **relays, size_t max_relays,
                             NostrNip61Prefs *out) {
    if (!ev || !out) return -EINVAL;
    if (nostr_event_get_kind(ev) != NOSTR_NIP61_KIND_NUTZAP_PREFS)
        return -EINVAL;

    memset(out, 0, sizeof(*out));
    out->mints = mints;
    out->relays = relays;

    const NostrTags *tags = nostr_event_get_tags(ev);
    size_t n = nostr_tags_size(tags);

    for (size_t i = 0; i < n; ++i) {
        const NostrTag *t = nostr_tags_get(tags, i);
        const char *k = tag_key(t);
        if (!k) continue;

        if (strcmp(k, "mint") == 0 && out->mint_count < max_mints) {
            const char *url = tag_val(t, 1);
            const char *unit = tag_val(t, 2);
            if (url && unit) {
                NostrNip61Mint *m = &mints[out->mint_count++];
                m->url = url;
                m->unit = unit;
                m->pubkey = tag_val(t, 3); /* nullable */
            }
        } else if (strcmp(k, "relay") == 0 && out->relay_count < max_relays) {
            const char *url = tag_val(t, 1);
            if (url && *url) {
                relays[out->relay_count++] = url;
            }
        } else if (strcmp(k, "p2pk") == 0) {
            out->require_p2pk = true;
        }
    }

    return 0;
}

int nostr_nip61_create_prefs(NostrEvent *ev,
                              const NostrNip61Mint *mints, size_t mint_count,
                              const char **relays, size_t relay_count,
                              bool require_p2pk) {
    if (!ev) return -EINVAL;

    nostr_event_set_kind(ev, NOSTR_NIP61_KIND_NUTZAP_PREFS);
    nostr_event_set_created_at(ev, (int64_t)time(NULL));
    nostr_event_set_content(ev, "");

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);

    /* mint tags: ["mint", url, unit] or ["mint", url, unit, pubkey] */
    for (size_t i = 0; i < mint_count; ++i) {
        if (!mints[i].url || !mints[i].unit) continue;
        NostrTag *t;
        if (mints[i].pubkey) {
            t = nostr_tag_new("mint", mints[i].url, mints[i].unit,
                              mints[i].pubkey, NULL);
        } else {
            t = nostr_tag_new("mint", mints[i].url, mints[i].unit, NULL);
        }
        nostr_tags_append(tags, t);
    }

    /* relay tags */
    for (size_t i = 0; i < relay_count; ++i) {
        if (!relays[i]) continue;
        nostr_tags_append(tags, nostr_tag_new("relay", relays[i], NULL));
    }

    /* p2pk tag */
    if (require_p2pk) {
        nostr_tags_append(tags, nostr_tag_new("p2pk", NULL));
    }

    return 0;
}

bool nostr_nip61_prefs_accepts_mint(const NostrNip61Prefs *prefs,
                                     const char *mint_url) {
    if (!prefs || !mint_url) return false;

    for (size_t i = 0; i < prefs->mint_count; ++i) {
        if (prefs->mints[i].url &&
            strcmp(prefs->mints[i].url, mint_url) == 0) {
            return true;
        }
    }
    return false;
}

/* ---- Nutzap event ---- */

int nostr_nip61_parse_nutzap(const NostrEvent *ev,
                              NostrNip61Nutzap *out) {
    if (!ev || !out) return -EINVAL;
    if (nostr_event_get_kind(ev) != NOSTR_NIP61_KIND_NUTZAP)
        return -EINVAL;

    memset(out, 0, sizeof(*out));

    const NostrTags *tags = nostr_event_get_tags(ev);
    size_t n = nostr_tags_size(tags);

    for (size_t i = 0; i < n; ++i) {
        const NostrTag *t = nostr_tags_get(tags, i);
        const char *k = tag_key(t);
        if (!k) continue;

        if (strcmp(k, "proofs") == 0) {
            out->proofs_json = tag_val(t, 1);
        } else if (strcmp(k, "u") == 0) {
            out->mint_url = tag_val(t, 1);
        } else if (strcmp(k, "p") == 0) {
            out->recipient_pubkey = tag_val(t, 1);
        } else if (strcmp(k, "e") == 0) {
            out->zapped_event_id = tag_val(t, 1);
            out->zapped_relay = tag_val(t, 2);
        } else if (strcmp(k, "a") == 0) {
            out->addressable_ref = tag_val(t, 1);
        }
    }

    /* proofs, mint URL, and recipient are required */
    if (!out->proofs_json || !out->mint_url || !out->recipient_pubkey)
        return -EINVAL;

    return 0;
}

int nostr_nip61_create_nutzap(NostrEvent *ev,
                               const char *proofs_json,
                               const char *mint_url,
                               const char *recipient_pubkey,
                               const char *zapped_event_id,
                               const char *zapped_relay,
                               const char *addressable_ref) {
    if (!ev || !proofs_json || !mint_url || !recipient_pubkey)
        return -EINVAL;

    nostr_event_set_kind(ev, NOSTR_NIP61_KIND_NUTZAP);
    nostr_event_set_created_at(ev, (int64_t)time(NULL));
    nostr_event_set_content(ev, "");

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);

    nostr_tags_append(tags, nostr_tag_new("proofs", proofs_json, NULL));
    nostr_tags_append(tags, nostr_tag_new("u", mint_url, NULL));
    nostr_tags_append(tags, nostr_tag_new("p", recipient_pubkey, NULL));

    if (zapped_event_id) {
        if (zapped_relay) {
            nostr_tags_append(tags,
                nostr_tag_new("e", zapped_event_id, zapped_relay, NULL));
        } else {
            nostr_tags_append(tags,
                nostr_tag_new("e", zapped_event_id, NULL));
        }
    }

    if (addressable_ref) {
        nostr_tags_append(tags,
            nostr_tag_new("a", addressable_ref, NULL));
    }

    return 0;
}

/* ---- Utilities ---- */

bool nostr_nip61_is_valid_mint_url(const char *url) {
    if (!url || !*url) return false;

    size_t len = strlen(url);
    if (len < 10 || len > 2048) return false;

    /* Must be https:// or http://localhost / http://127.0.0.1 */
    if (strncmp(url, "https://", 8) == 0) return true;
    if (strncmp(url, "http://localhost", 16) == 0) return true;
    if (strncmp(url, "http://127.0.0.1", 16) == 0) return true;

    return false;
}

int64_t nostr_nip61_proofs_total_amount(const char *proofs_json) {
    if (!proofs_json) return 0;

    int64_t total = 0;
    const char *p = proofs_json;

    /* Scan for "amount": followed by a number */
    while ((p = strstr(p, "\"amount\"")) != NULL) {
        p += 8; /* past "amount" */

        /* Skip whitespace and colon */
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
        if (*p != ':') continue;
        ++p;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;

        /* Parse integer */
        char *end = NULL;
        int64_t val = strtoll(p, &end, 10);
        if (end > p) {
            total += val;
        }
    }

    return total;
}

size_t nostr_nip61_proofs_count(const char *proofs_json) {
    if (!proofs_json) return 0;

    /* Count occurrences of "amount" keys */
    size_t count = 0;
    const char *p = proofs_json;
    while ((p = strstr(p, "\"amount\"")) != NULL) {
        ++count;
        p += 8;
    }
    return count;
}
