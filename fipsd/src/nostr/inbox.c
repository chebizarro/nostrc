#include "nostr/inbox.h"
#include "nostr/internal.h"
#include <nostr/nip17/nip17.h>
#include <sodium.h>
#include <stdlib.h>
#include <string.h>

static void equality_key(const char *url, char out[FIPSD_NOSTR_URL_MAX + 1])
{
    memcpy(out, url, strlen(url) + 1);
    char *scheme = strstr(out, "://");
    if (!scheme) return;
    char *path = strchr(scheme + 3, '/');
    /* Rust RelayUrl preserves display's explicit root slash, but Eq/Hash use
     * the underlying URL, where an empty path and '/' are identical. */
    if (path && (!path[1] || path[1] == '?')) memmove(path, path + 1, strlen(path));
}
fipsd_nostr_status fipsd_nostr_inbox_build(const fips_identity *id,
    const fipsd_nostr_inbox *inbox, uint64_t at, NostrEvent **out)
{
    if (!id || !inbox || !out || at > INT64_MAX ||
        inbox->count > FIPSD_NOSTR_RELAYS_MAX) return FIPSD_NOSTR_LIMIT;
    const char *relays[FIPSD_NOSTR_RELAYS_MAX + 1] = {0};
    fipsd_nostr_inbox normalized = { .count = inbox->count };
    for (size_t i = 0; i < inbox->count; ++i) {
        if (!fn_relay_normalize(inbox->relays[i], normalized.relays[i])) return FIPSD_NOSTR_INVALID;
        relays[i] = normalized.relays[i];
    }
    char sk[65]; fn_secret(id, sk);
    NostrEvent *e = inbox->count ? nostr_nip17_create_dm_relay_list(relays, sk) : nostr_event_new();
    /* The shared NIP-17 helper refuses zero relays. An empty replacement is
     * nevertheless a valid upstream inbox list (withdraw all preferences). */
    if (e && !inbox->count) {
        nostr_event_set_kind(e, NOSTR_KIND_DM_RELAY_LIST);
        nostr_event_set_content(e, "");
        nostr_event_set_tags(e, nostr_tags_new(0));
    }
    sodium_memzero(sk, sizeof(sk));
    if (!e) return FIPSD_NOSTR_CRYPTO;
    /* nip17 emits the standard relay form; deployed FIPS emits bare r. */
    for (size_t i = 0; i < e->tags->count; ++i) nostr_tag_set(e->tags->data[i], 0, "r");
    nostr_event_set_created_at(e, (int64_t)at);
    fipsd_nostr_status rc = fn_sign(e, id);
    if (rc != FIPSD_NOSTR_OK) { nostr_event_free(e); return rc; }
    *out = e; return FIPSD_NOSTR_OK;
}
fipsd_nostr_status fipsd_nostr_inbox_parse(const NostrEvent *e, fipsd_nostr_inbox *out)
{
    if (!out || !fn_tags_bounded(e) || e->kind != NOSTR_KIND_DM_RELAY_LIST) return FIPSD_NOSTR_INVALID;
    fipsd_nostr_inbox inbox = {0};
    for (size_t i = 0; i < e->tags->count; ++i) {
        const NostrTag *t = e->tags->data[i];
        const char *key = nostr_tag_get(t, 0), *url = nostr_tag_get(t, 1);
        if (!key || nostr_tag_size(t) != 2 || (strcmp(key, "relay") && strcmp(key, "r"))) continue;
        char normalized_url[FIPSD_NOSTR_URL_MAX + 1];
        if (!fn_relay_normalize(url, normalized_url)) continue;
        bool seen = false;
        char incoming[FIPSD_NOSTR_URL_MAX + 1]; equality_key(normalized_url, incoming);
        for (size_t k = 0; k < inbox.count; ++k) {
            char held[FIPSD_NOSTR_URL_MAX + 1]; equality_key(inbox.relays[k], held);
            if (!strcmp(held, incoming)) seen = true;
        }
        if (seen) continue;
        if (inbox.count == FIPSD_NOSTR_RELAYS_MAX) return FIPSD_NOSTR_LIMIT;
        memcpy(inbox.relays[inbox.count++], normalized_url, strlen(normalized_url) + 1);
    }
    if (nostr_event_validate(e, NULL) != NOSTR_EVENT_VALIDATION_OK) return FIPSD_NOSTR_CRYPTO;
    /* Feed the existing NIP-17 parser a bounded standardized tag view. */
    if (inbox.count) {
        NostrEvent view = { .kind = NOSTR_KIND_DM_RELAY_LIST, .tags = nostr_tags_new(0) };
        if (!view.tags) return FIPSD_NOSTR_LIMIT;
        for (size_t i = 0; i < inbox.count; ++i)
            nostr_tags_append(view.tags, nostr_tag_new("relay", inbox.relays[i], NULL));
        NostrDmRelayList *parsed = nostr_nip17_parse_dm_relay_list(&view);
        nostr_tags_free(view.tags);
        bool ok = parsed && parsed->count == inbox.count;
        nostr_nip17_free_dm_relay_list(parsed);
        if (!ok) return FIPSD_NOSTR_LIMIT;
    }
    *out = inbox; return FIPSD_NOSTR_OK;
}
