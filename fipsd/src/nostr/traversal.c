#include "nostr/traversal.h"
#include "nostr/internal.h"
#include <fips/identity/encoding.h>
#include <nostr/nip17/nip17.h>
#include <nostr/nip59/nip59.h>
#include <nostr/nip44/nip44.h>
#include <nostr/nip40/nip40.h>
#include <sodium.h>
#include <stdlib.h>
#include <string.h>

static void address_json(fn_writer *w, const fipsd_nostr_address *a)
{
    fn_raw(w, "{\"protocol\":"); fn_quote(w, a->protocol, sizeof(a->protocol));
    fn_raw(w, ",\"ip\":"); fn_quote(w, a->ip, sizeof(a->ip));
    fn_raw(w, ",\"port\":"); fn_uint(w, a->port); fn_raw(w, "}");
}
char *fipsd_nostr_traversal_content(const fipsd_nostr_traversal *p)
{
    if (!p || p->local_count > FIPSD_NOSTR_CANDIDATES_MAX) return NULL;
    fn_writer w = {.ok = true};
    fn_raw(&w, "{\"type\":"); fn_quote(&w, p->answer ? "answer" : "offer", 7);
    fn_raw(&w, ",\"sessionId\":"); fn_quote(&w, p->session_id, sizeof(p->session_id));
    fn_raw(&w, ",\"issuedAt\":"); fn_uint(&w, p->issued_at);
    fn_raw(&w, ",\"expiresAt\":"); fn_uint(&w, p->expires_at);
    fn_raw(&w, ",\"nonce\":"); fn_quote(&w, p->nonce, sizeof(p->nonce));
    fn_raw(&w, ",\"senderNpub\":"); fn_quote(&w, p->sender_npub, sizeof(p->sender_npub));
    fn_raw(&w, ",\"recipientNpub\":"); fn_quote(&w, p->recipient_npub, sizeof(p->recipient_npub));
    if (p->answer) {
        fn_raw(&w, ",\"inReplyTo\":"); fn_quote(&w, p->in_reply_to, sizeof(p->in_reply_to));
        fn_raw(&w, p->accepted ? ",\"accepted\":true" : ",\"accepted\":false");
    }
    fn_raw(&w, ",\"reflexiveAddress\":");
    if (p->has_reflexive) address_json(&w, &p->reflexive); else fn_raw(&w, "null");
    fn_raw(&w, ",\"localAddresses\":[");
    for (size_t i = 0; i < p->local_count; ++i) { if (i) fn_raw(&w, ","); address_json(&w, &p->local[i]); }
    fn_raw(&w, "],\"stunServer\":");
    if (p->has_stun) fn_quote(&w, p->stun, sizeof(p->stun)); else fn_raw(&w, "null");
    if (p->answer) {
        fn_raw(&w, ",\"punch\":");
        if (p->has_punch) {
            fn_raw(&w, "{\"startAtMs\":"); fn_uint(&w, p->punch.start_at_ms);
            fn_raw(&w, ",\"intervalMs\":"); fn_uint(&w, p->punch.interval_ms);
            fn_raw(&w, ",\"durationMs\":"); fn_uint(&w, p->punch.duration_ms); fn_raw(&w, "}");
        } else fn_raw(&w, "null");
        fn_raw(&w, ",\"reason\":");
        if (p->has_reason) fn_quote(&w, p->reason, sizeof(p->reason)); else fn_raw(&w, "null");
        if (p->has_offer_received_at) { fn_raw(&w, ",\"offerReceivedAt\":"); fn_uint(&w, p->offer_received_at); }
    }
    fn_raw(&w, "}"); return fn_finish(&w);
}
static bool address(const fn_json *j, unsigned t, fipsd_nostr_address *a)
{
    uint64_t port;
    if (!fn_string(j, fn_field(j, t, "protocol"), a->protocol, sizeof(a->protocol)) ||
        !fn_string(j, fn_field(j, t, "ip"), a->ip, sizeof(a->ip)) ||
        !fn_u64(j, fn_field(j, t, "port"), &port) || port > UINT16_MAX) return false;
    a->port = (uint16_t)port; return true;
}
fipsd_nostr_status fipsd_nostr_traversal_parse_content(const char *text, size_t len, fipsd_nostr_traversal *out)
{
    if (!out || len > FIPSD_NOSTR_BODY_MAX) return FIPSD_NOSTR_LIMIT;
    fn_json j;
    if (!fn_parse(&j, text, len, FIPSD_NOSTR_BODY_MAX)) return FIPSD_NOSTR_INVALID;
    fipsd_nostr_traversal p = {0};
    unsigned type = fn_field(&j, 1, "type");
    if (fn_eq(&j, type, "answer")) p.answer = true;
    else if (!fn_eq(&j, type, "offer")) return FIPSD_NOSTR_INVALID;
#define STR(key, field) if (!fn_string(&j, fn_field(&j, 1, key), p.field, sizeof(p.field))) return FIPSD_NOSTR_INVALID
#define NUM(key, field) if (!fn_u64(&j, fn_field(&j, 1, key), &p.field)) return FIPSD_NOSTR_INVALID
    STR("sessionId", session_id); NUM("issuedAt", issued_at); NUM("expiresAt", expires_at);
    STR("nonce", nonce); STR("senderNpub", sender_npub); STR("recipientNpub", recipient_npub);
    if (strlen(p.sender_npub) != 63 || strlen(p.recipient_npub) != 63 ||
        strncmp(p.sender_npub, "npub1", 5) || strncmp(p.recipient_npub, "npub1", 5)) return FIPSD_NOSTR_INVALID;
    unsigned ref = fn_field(&j, 1, "reflexiveAddress");
    p.has_reflexive = !fn_null(&j, ref);
    if (p.has_reflexive && !address(&j, ref, &p.reflexive)) return FIPSD_NOSTR_INVALID;
    unsigned local = fn_field(&j, 1, "localAddresses");
    if (!local || j.t[local].type != '[' || j.t[local].count > FIPSD_NOSTR_CANDIDATES_MAX) return FIPSD_NOSTR_LIMIT;
    for (unsigned k = local + 1; k < j.t[local].next; k = j.t[k].next)
        if (!address(&j, k, &p.local[p.local_count++])) return FIPSD_NOSTR_INVALID;
    p.has_stun = !fn_null(&j, fn_field(&j, 1, "stunServer"));
    if (p.has_stun) { STR("stunServer", stun); }
    if (p.answer) {
        STR("inReplyTo", in_reply_to);
        if (!fn_bool(&j, fn_field(&j, 1, "accepted"), &p.accepted)) return FIPSD_NOSTR_INVALID;
        unsigned punch = fn_field(&j, 1, "punch");
        p.has_punch = !fn_null(&j, punch);
        if (p.has_punch && (!fn_u64(&j, fn_field(&j, punch, "startAtMs"), &p.punch.start_at_ms) ||
            !fn_u64(&j, fn_field(&j, punch, "intervalMs"), &p.punch.interval_ms) ||
            !fn_u64(&j, fn_field(&j, punch, "durationMs"), &p.punch.duration_ms))) return FIPSD_NOSTR_INVALID;
        p.has_reason = !fn_null(&j, fn_field(&j, 1, "reason"));
        if (p.has_reason) { STR("reason", reason); }
        p.has_offer_received_at = !fn_null(&j, fn_field(&j, 1, "offerReceivedAt"));
        if (p.has_offer_received_at) { NUM("offerReceivedAt", offer_received_at); }
    }
#undef STR
#undef NUM
    *out = p; return FIPSD_NOSTR_OK;
}
fipsd_nostr_status fipsd_nostr_traversal_freshness(uint64_t issued, uint64_t expires,
    uint64_t now, uint64_t ttl, fipsd_nostr_freshness *out)
{
    uint64_t expiry = fn_add(issued, ttl); if (expires < expiry) expiry = expires;
    uint64_t ahead = issued > now ? issued - now : 0, age = now > issued ? now - issued : 0;
    if (!ahead && expiry > now && age <= ttl) { if (out) *out = FIPSD_NOSTR_FRESH; return FIPSD_NOSTR_OK; }
    if (ahead > FIPSD_NOSTR_SKEW_MS) return FIPSD_NOSTR_FUTURE;
    if (fn_add(expiry, FIPSD_NOSTR_SKEW_MS) <= now || age > fn_add(ttl, FIPSD_NOSTR_SKEW_MS)) return FIPSD_NOSTR_STALE;
    if (out) *out = FIPSD_NOSTR_SKEW;
    return FIPSD_NOSTR_OK;
}
fipsd_nostr_status fipsd_nostr_traversal_answer_for_offer(const fipsd_nostr_traversal *offer,
    const fipsd_nostr_traversal *answer, uint64_t now, uint64_t ttl, fipsd_nostr_freshness *out)
{
    if (!offer || !answer || offer->answer || !answer->answer ||
        strcmp(offer->session_id, answer->session_id) || strcmp(offer->nonce, answer->in_reply_to) ||
        strcmp(offer->sender_npub, answer->recipient_npub) || strcmp(offer->recipient_npub, answer->sender_npub) ||
        (answer->accepted && !answer->has_reflexive && !answer->local_count) ||
        (!answer->accepted && (!answer->has_reason || !answer->reason[0]))) return FIPSD_NOSTR_INVALID;
    fipsd_nostr_freshness a, b;
    fipsd_nostr_status rc = fipsd_nostr_traversal_freshness(offer->issued_at, offer->expires_at, now, ttl, &a);
    if (rc != FIPSD_NOSTR_OK) return rc;
    rc = fipsd_nostr_traversal_freshness(answer->issued_at, answer->expires_at, now, ttl, &b);
    if (rc == FIPSD_NOSTR_OK && out) *out = a == FIPSD_NOSTR_FRESH && b == FIPSD_NOSTR_FRESH ? FIPSD_NOSTR_FRESH : FIPSD_NOSTR_SKEW;
    return rc;
}
static bool identities(const fips_identity *id, const char *peer_hex, const fipsd_nostr_traversal *p, bool sending)
{
    char own[64], peer[64]; fips_xonly_pubkey key; size_t n;
    if (!peer_hex || strlen(peer_hex) != 64 || sodium_hex2bin(key.bytes, 32, peer_hex, 64, NULL, &n, NULL) || n != 32 ||
        fips_identity_npub(id, own, sizeof(own)) != FIPS_IDENTITY_OK ||
        fips_encode_npub(&key, peer, sizeof(peer)) != FIPS_IDENTITY_OK) return false;
    return !strcmp(p->sender_npub, sending ? own : peer) && !strcmp(p->recipient_npub, sending ? peer : own);
}
fipsd_nostr_status fipsd_nostr_traversal_build(const fips_identity *id, const char *to,
    const fipsd_nostr_traversal *p, uint64_t at, uint64_t ttl, NostrEvent **out)
{
    if (!id || !out || !p || at > INT64_MAX || ttl > (uint64_t)INT64_MAX - at) return FIPSD_NOSTR_INVALID;
    char *body = fipsd_nostr_traversal_content(p);
    fipsd_nostr_traversal checked;
    if (!body || fipsd_nostr_traversal_parse_content(body, strlen(body), &checked) != FIPSD_NOSTR_OK || !identities(id, to, &checked, true)) {
        free(body); return FIPSD_NOSTR_INVALID;
    }
    char sk[65], pub[65]; fn_secret(id, sk);
    fips_xonly_pubkey pk = fips_identity_pubkey(id); sodium_bin2hex(pub, sizeof(pub), pk.bytes, 32);
    NostrEvent *rumor = nostr_nip17_create_rumor(pub, to, body, (int64_t)at); free(body);
    if (rumor) nostr_event_set_created_at(rumor, (int64_t)at); /* zero is an explicit epoch here. */
    NostrEvent *seal = rumor ? nostr_nip17_create_seal(rumor, sk, to) : NULL;
    nostr_event_free(rumor);
    int64_t backdated;
    if (!seal || nostr_nip59_randomize_timestamp((int64_t)at, 0, &backdated) ||
        (nostr_event_set_created_at(seal, backdated), nostr_event_sign(seal, sk))) {
        sodium_memzero(sk, sizeof(sk)); nostr_event_free(seal); return FIPSD_NOSTR_CRYPTO;
    }
    sodium_memzero(sk, sizeof(sk));
    char *esk = NULL, *epk = NULL;
    if (nostr_nip59_create_ephemeral_key(&esk, &epk)) { nostr_event_free(seal); return FIPSD_NOSTR_CRYPTO; }
    NostrEvent *wrap = nostr_nip59_wrap(seal, to, esk); nostr_event_free(seal); free(epk);
    bool ok = false;
    if (wrap) {
        nostr_event_set_kind(wrap, NOSTR_KIND_FIPS_TRAVERSAL);
        /* Upstream custom wrap is at the explicit send time; only its seal is
         * backdated. Keep p then expiration tag order from build_signal_event. */
        nostr_event_set_created_at(wrap, (int64_t)at);
        ok = nostr_nip40_set_expiration(wrap, (int64_t)(at + ttl)) == 0 && nostr_event_sign(wrap, esk) == 0;
    }
    sodium_memzero(esk, strlen(esk)); free(esk);
    if (!ok) { nostr_event_free(wrap); return FIPSD_NOSTR_CRYPTO; }
    *out = wrap; return FIPSD_NOSTR_OK;
}
static NostrEvent *decrypt(const uint8_t sk[32], const char *peer_hex, const char *cipher, bool signed_event)
{
    uint8_t peer[32], *plain = NULL; size_t n = 0, len = 0;
    if (!peer_hex || strlen(peer_hex) != 64 || !cipher || strnlen(cipher, 32769) > 32768 ||
        sodium_hex2bin(peer, 32, peer_hex, 64, NULL, &n, NULL) || n != 32 ||
        nostr_nip44_decrypt_v2(sk, peer, cipher, &plain, &len)) return NULL;
    fn_json j;
    if (!fn_parse(&j, (const char *)plain, len, 32768) || !fn_event_shape(&j, signed_event)) { sodium_memzero(plain, len); free(plain); return NULL; }
    /* The decrypted buffer is not assumed NUL-terminated by NIP-44. */
    char *text = malloc(len + 1);
    if (!text) { free(plain); return NULL; }
    memcpy(text, plain, len); text[len] = 0; sodium_memzero(plain, len); free(plain);
    NostrEvent *e = nostr_event_new();
    NostrEventValidationStatus status = NOSTR_EVENT_VALIDATION_NULL;
    if (e) status = signed_event ? nostr_event_deserialize_signed(e, text, NULL) : nostr_event_deserialize_unsigned(e, text, NULL);
    sodium_memzero(text, len); free(text);
    if (status != NOSTR_EVENT_VALIDATION_OK || !fn_tags_bounded(e) ||
        (signed_event ? nostr_event_validate(e, NULL) : nostr_event_validate_id(e, NULL)) != NOSTR_EVENT_VALIDATION_OK) {
        nostr_event_free(e); return NULL;
    }
    return e;
}
fipsd_nostr_status fipsd_nostr_traversal_open(const fips_identity *recipient, const NostrEvent *e,
    uint64_t now, uint64_t ttl, fipsd_nostr_traversal *out, fipsd_nostr_freshness *fresh)
{
    if (!recipient || !out || !fn_tags_bounded(e) || e->kind != NOSTR_KIND_FIPS_TRAVERSAL ||
        !nostr_kind_is_ephemeral(e->kind)) return FIPSD_NOSTR_INVALID;
    fips_xonly_pubkey pk = fips_identity_pubkey(recipient); char pub[65];
    sodium_bin2hex(pub, sizeof(pub), pk.bytes, 32);
    const char *to = fn_tag(e, "p"); int64_t expiry;
    if (!to || strcmp(to, pub) || nostr_nip40_get_expiration(e, &expiry) || expiry < 0) return FIPSD_NOSTR_INVALID;
    if (fn_mul1000((uint64_t)expiry) <= now) return FIPSD_NOSTR_STALE;
    if (nostr_event_validate(e, NULL) != NOSTR_EVENT_VALIDATION_OK) return FIPSD_NOSTR_CRYPTO;
    uint8_t sk[32]; fips_identity_export_secret(recipient, sk);
    NostrEvent *seal = decrypt(sk, e->pubkey, e->content, true);
    NostrEvent *rumor = seal && seal->kind == NOSTR_KIND_SEAL && seal->tags->count == 0 ? decrypt(sk, seal->pubkey, seal->content, false) : NULL;
    sodium_memzero(sk, sizeof(sk));
    fipsd_nostr_status rc = FIPSD_NOSTR_CRYPTO;
    fipsd_nostr_traversal p; fipsd_nostr_freshness f;
    if (rumor && rumor->kind == NOSTR_KIND_DIRECT_MESSAGE && !strcmp(rumor->pubkey, seal->pubkey) &&
        fn_tag(rumor, "p") && !strcmp(fn_tag(rumor, "p"), pub)) {
        rc = fipsd_nostr_traversal_parse_content(rumor->content, strlen(rumor->content), &p);
        if (rc == FIPSD_NOSTR_OK && !identities(recipient, seal->pubkey, &p, false)) rc = FIPSD_NOSTR_INVALID;
        if (rc == FIPSD_NOSTR_OK) rc = fipsd_nostr_traversal_freshness(p.issued_at, p.expires_at, now, ttl, &f);
        if (rc == FIPSD_NOSTR_OK) { *out = p; if (fresh) *fresh = f; }
    }
    nostr_event_free(rumor); nostr_event_free(seal); return rc;
}
