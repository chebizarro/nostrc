#include "nostr/advert.h"
#include "nostr/inbox.h"
#include "nostr/traversal.h"
#include "nostr/internal.h"
#include "fakes/host.h"
#include <nostr/nip40/nip40.h>
#include <jansson.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define OK(x) CHECK((x) == FIPSD_NOSTR_OK)
static unsigned checks;
static fips_secp *secp;
static fips_fixed_entropy fixed = {0x5a};
static fips_identity *identity(const char *secret)
{
    fips_identity *id = NULL;
    CHECK(fips_identity_from_secret_str(secp, fips_fixed_entropy_adapter(&fixed), secret, strlen(secret), &id) == FIPS_IDENTITY_OK);
    return id;
}
static char *load(const char *name, size_t *len)
{
    char path[4096]; CHECK(snprintf(path, sizeof(path), "%s/%s", FIPSD_NOSTR_VECTORS, name) > 0);
    FILE *f = fopen(path, "rb"); CHECK(f);
    CHECK(fseek(f, 0, SEEK_END) == 0); long n = ftell(f); CHECK(n >= 0 && n < 1048576);
    rewind(f); char *text = malloc((size_t)n + 1); CHECK(text);
    CHECK(fread(text, 1, (size_t)n, f) == (size_t)n); CHECK(fclose(f) == 0);
    text[n] = 0; if (len) *len = (size_t)n; return text;
}
static const char *sval(const json_t *o, const char *k)
{
    const char *s = json_string_value(json_object_get(o, k)); CHECK(s); return s;
}
static void projection(const NostrEvent *held, const NostrEvent *built)
{
    char *a = fipsd_nostr_event_json(held, false), *b = fipsd_nostr_event_json(built, false);
    CHECK(a && b); CHECK(!strcmp(a, b)); free(a); free(b);
    CHECK(nostr_event_validate(built, NULL) == NOSTR_EVENT_VALIDATION_OK); ++checks;
}
static fipsd_nostr_traversal offer_fixture, answer_fixture;
static void fixture(const json_t *c)
{
    const char *name = sval(c, "name"); size_t len;
    char *raw = load(sval(json_object_get(c, "event"), "file"), &len);
    NostrEvent *event = NULL; OK(fipsd_nostr_event_parse(raw, len, &event));
    CHECK(nostr_event_check_signature(event));
    char *serialized = fipsd_nostr_event_json(event, true); CHECK(serialized && !strcmp(serialized, raw)); free(serialized);
    uint64_t now = (uint64_t)json_integer_value(json_object_get(c, "capture_unix_ms"));
    fips_identity *from = identity(sval(c, "signing_key_hex"));
    NostrEvent *rebuilt = NULL;
    if (event->kind == NOSTR_KIND_FIPS_OVERLAY_ADVERT) {
        fipsd_nostr_advert a; uint64_t until;
        OK(fipsd_nostr_advert_parse(event, FIPSD_NOSTR_IDENTIFIER, now, 3600000, &a, &until));
        CHECK(a.endpoint_count == 2 && a.endpoints[0].transport == FIPSD_NOSTR_UDP && !strcmp(a.endpoints[0].addr, "nat"));
        CHECK(a.endpoints[1].transport == FIPSD_NOSTR_TCP && !strcmp(a.endpoints[1].addr, "fips.example.com:8443"));
        char *body = fipsd_nostr_advert_content(&a), *expected = load(sval(json_object_get(c, "content"), "file"), NULL);
        CHECK(body && !strcmp(body, expected) && !strcmp(body, event->content)); free(body); free(expected);
        CHECK(until == (uint64_t)(event->created_at + 3600) * 1000);
        OK(fipsd_nostr_advert_build(from, &a, FIPSD_NOSTR_IDENTIFIER, (uint64_t)event->created_at, 3600, &rebuilt));
        projection(event, rebuilt);
        CHECK(fipsd_nostr_advert_parse(event, "other-app", now, 3600000, &a, &until) == FIPSD_NOSTR_INVALID);
        CHECK(fipsd_nostr_advert_parse(event, FIPSD_NOSTR_IDENTIFIER, now + 3600000, 3600000, &a, &until) == FIPSD_NOSTR_STALE);
    } else if (event->kind == NOSTR_KIND_DM_RELAY_LIST) {
        fipsd_nostr_inbox inbox; OK(fipsd_nostr_inbox_parse(event, &inbox));
        const json_t *relays = json_object_get(json_object_get(c, "decoded"), "relays");
        CHECK(inbox.count == json_array_size(relays));
        json_t *normalization = json_object_get(c, "relay_normalization");
        for (size_t k = 0; k < json_array_size(normalization); ++k) {
            const json_t *entry = json_array_get(normalization, k);
            char normalized[FIPSD_NOSTR_URL_MAX + 1];
            const char *expected = json_string_value(json_object_get(entry, "expected"));
            bool valid = fn_relay_normalize(sval(entry, "input"), normalized);
            CHECK(valid == (expected != NULL)); if (valid) CHECK(!strcmp(normalized, expected));
        }
        for (size_t i = 0; i < inbox.count; ++i) CHECK(!strcmp(inbox.relays[i], json_string_value(json_array_get(relays, i))));
        OK(fipsd_nostr_inbox_build(from, &inbox, (uint64_t)event->created_at, &rebuilt)); projection(event, rebuilt);
    } else {
        CHECK(event->kind == NOSTR_KIND_FIPS_TRAVERSAL && nostr_kind_is_ephemeral(event->kind));
        fips_identity *to = identity(sval(c, "recipient_key_hex"));
        fipsd_nostr_traversal p, opened; fipsd_nostr_freshness f;
        OK(fipsd_nostr_traversal_open(to, event, now, 120000, &p, &f)); CHECK(f == FIPSD_NOSTR_FRESH);
        char *body = fipsd_nostr_traversal_content(&p), *expected = load(sval(json_object_get(c, "content"), "file"), NULL);
        CHECK(body && !strcmp(body, expected));
        json_t *decoded = json_loads(body, 0, NULL); CHECK(decoded && json_equal(decoded, json_object_get(c, "decoded"))); json_decref(decoded);
        fips_xonly_pubkey to_pk = fips_identity_pubkey(to); char to_hex[65]; sodium_bin2hex(to_hex, sizeof(to_hex), to_pk.bytes, 32);
        OK(fipsd_nostr_traversal_build(from, to_hex, &p, (uint64_t)event->created_at, 120, &rebuilt));
        CHECK(nostr_event_check_signature(rebuilt)); CHECK(strcmp(rebuilt->pubkey, event->pubkey));
        CHECK(!strcmp(fn_tag(rebuilt, "p"), fn_tag(event, "p")) && !strcmp(fn_tag(rebuilt, "expiration"), fn_tag(event, "expiration")));
        OK(fipsd_nostr_traversal_open(to, rebuilt, now, 120000, &opened, &f));
        char *again = fipsd_nostr_traversal_content(&opened); CHECK(again && !strcmp(again, expected)); free(again);
        CHECK(fipsd_nostr_traversal_open(from, event, now, 120000, &opened, &f) == FIPSD_NOSTR_INVALID);
        CHECK(fipsd_nostr_traversal_open(to, event, now + 120000, 120000, &opened, &f) == FIPSD_NOSTR_STALE);
        if (p.answer) answer_fixture = p; else offer_fixture = p;
        free(expected); free(body); fips_identity_destroy(to); ++checks;
    }
    uint8_t id[32], storage[2][32]; CHECK(fipsd_nostr_event_id(event, id));
    fipsd_nostr_seen seen = {.ids = storage, .capacity = 2};
    CHECK(!fipsd_nostr_seen_contains(&seen, id)); CHECK(fipsd_nostr_seen_insert(&seen, id));
    CHECK(fipsd_nostr_seen_contains(&seen, id)); CHECK(!fipsd_nostr_seen_insert(&seen, id));
    id[0] ^= 1; CHECK(fipsd_nostr_seen_insert(&seen, id)); id[0] ^= 2; CHECK(!fipsd_nostr_seen_insert(&seen, id));
    /* Same event from a second relay is still the same id; no timers involved. */
    NostrEvent *sentinel = event;
    raw[len - 3] ^= 1; CHECK(fipsd_nostr_event_parse(raw, len, &sentinel) != FIPSD_NOSTR_OK && sentinel == event);
    nostr_event_free(rebuilt); nostr_event_free(event); free(raw); fips_identity_destroy(from);
    printf("fixture %s: parse, decoded fields, rebuild, signature, dedup PASS\n", name);
}
static void freshness(void)
{
    fipsd_nostr_freshness f;
    OK(fipsd_nostr_traversal_freshness(100000, 220000, 100000, 120000, &f)); CHECK(f == FIPSD_NOSTR_FRESH);
    OK(fipsd_nostr_traversal_freshness(100000, 220000, 220000, 120000, &f)); CHECK(f == FIPSD_NOSTR_SKEW);
    OK(fipsd_nostr_traversal_freshness(100000, 220000, 279999, 120000, &f));
    CHECK(fipsd_nostr_traversal_freshness(100000, UINT64_MAX, 280000, 120000, &f) == FIPSD_NOSTR_STALE);
    OK(fipsd_nostr_traversal_freshness(160000, 280000, 100000, 120000, &f)); CHECK(f == FIPSD_NOSTR_SKEW);
    CHECK(fipsd_nostr_traversal_freshness(160001, 280001, 100000, 120000, &f) == FIPSD_NOSTR_FUTURE);
    CHECK(fipsd_nostr_traversal_freshness(100000, 90000, 150000, 120000, &f) == FIPSD_NOSTR_STALE);
    OK(fipsd_nostr_traversal_freshness(UINT64_MAX, UINT64_MAX, UINT64_MAX - 1, UINT64_MAX, &f));
    OK(fipsd_nostr_traversal_answer_for_offer(&offer_fixture, &answer_fixture, offer_fixture.issued_at, 120000, &f));
    fipsd_nostr_traversal bad = answer_fixture; bad.in_reply_to[0] ^= 1;
    CHECK(fipsd_nostr_traversal_answer_for_offer(&offer_fixture, &bad, offer_fixture.issued_at, 120000, &f) == FIPSD_NOSTR_INVALID);
    bad = answer_fixture; bad.has_reflexive = false;
    CHECK(fipsd_nostr_traversal_answer_for_offer(&offer_fixture, &bad, offer_fixture.issued_at, 120000, &f) == FIPSD_NOSTR_INVALID);
    bad.accepted = false; CHECK(fipsd_nostr_traversal_answer_for_offer(&offer_fixture, &bad, offer_fixture.issued_at, 120000, &f) == FIPSD_NOSTR_INVALID);
    ++checks;
}
static void invalid_body(json_t *value)
{
    char *s = json_dumps(value, JSON_COMPACT); CHECK(s); fipsd_nostr_traversal p = offer_fixture;
    CHECK(fipsd_nostr_traversal_parse_content(s, strlen(s), &p) != FIPSD_NOSTR_OK);
    CHECK(!memcmp(&p, &offer_fixture, sizeof(p))); free(s); ++checks;
}
static void bounds(void)
{
    char *body = fipsd_nostr_traversal_content(&offer_fixture); CHECK(body);
    fipsd_nostr_traversal p;
    for (size_t n = 0; n < strlen(body); ++n) CHECK(fipsd_nostr_traversal_parse_content(body, n, &p) != FIPSD_NOSTR_OK);
    const char *fields[] = {"sessionId", "nonce", "senderNpub", "recipientNpub", "stunServer"};
    const size_t limits[] = {FIPSD_NOSTR_SESSION_MAX, FIPSD_NOSTR_SESSION_MAX, 63, 63, FIPSD_NOSTR_URL_MAX};
    char huge[1026]; memset(huge, 'x', sizeof(huge));
    for (size_t i = 0; i < 5; ++i) {
        json_t *v = json_loads(body, 0, NULL); CHECK(v);
        CHECK(json_object_set_new(v, fields[i], json_stringn(huge, limits[i] + 1)) == 0); invalid_body(v); json_decref(v);
    }
    json_t *v = json_loads(body, 0, NULL), *a = json_array(); CHECK(v && a);
    for (unsigned i = 0; i <= FIPSD_NOSTR_CANDIDATES_MAX; ++i)
        CHECK(json_array_append(a, json_object_get(v, "reflexiveAddress")) == 0);
    CHECK(json_object_set_new(v, "localAddresses", a) == 0); invalid_body(v); json_decref(v);
    v = json_loads(body, 0, NULL);
    CHECK(json_object_set_new(json_object_get(v, "reflexiveAddress"), "port", json_integer(65536)) == 0); invalid_body(v); json_decref(v);
    const char *bad[] = {"{\"x\":1,\"x\":2}", "{\"x\":1,\"\\u0078\":2}", "{\"x\":\"\\u0000\"}",
        "{\"x\":\"\\ud800\"}", "{\"x\":\"\\udfff\"}", "{\"x\":01}", "{\"x\":true,}", "{\"x\":[1,]}",
        "{\"x\":\"\xc0\xaf\"}", "{\"x\":[[[[[[[[[0]]]]]]]]]}", "{} garbage"};
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) { fn_json j; CHECK(!fn_parse(&j, bad[i], strlen(bad[i]), FIPSD_NOSTR_BODY_MAX)); }
    const char *overflow = "{\"v\":18446744073709551616}"; fn_json j; uint64_t n;
    CHECK(fn_parse(&j, overflow, strlen(overflow), FIPSD_NOSTR_BODY_MAX)); CHECK(!fn_u64(&j, fn_field(&j, 1, "v"), &n));
    char oversized[FIPSD_NOSTR_BODY_MAX + 1]; memset(oversized, ' ', sizeof(oversized));
    CHECK(fipsd_nostr_traversal_parse_content(oversized, sizeof(oversized), &p) == FIPSD_NOSTR_LIMIT);
    NostrEvent *e = NULL; CHECK(fipsd_nostr_event_parse("{}", FIPSD_NOSTR_EVENT_MAX + 1, &e) == FIPSD_NOSTR_LIMIT && !e);
    free(body); ++checks;
}
static void advert_inbox_bounds(void)
{
    fips_identity *id = identity("0000000000000000000000000000000000000000000000000000000000000003");
    fipsd_nostr_advert a = { .endpoint_count = 1 };
    a.endpoints[0].transport = FIPSD_NOSTR_TCP; strcpy(a.endpoints[0].addr, "example.com:8443");
    NostrEvent *e = NULL; OK(fipsd_nostr_advert_build(id, &a, "private-app", 2000000000, 3600, &e));
    fipsd_nostr_advert parsed; uint64_t until;
    OK(fipsd_nostr_advert_parse(e, "private-app", UINT64_C(1900000000000), 3600000, &parsed, &until));
    CHECK(until == UINT64_C(1900000000000) + FIPSD_NOSTR_SKEW_MS + 3600000);
    char *body = strdup(e->content); CHECK(body);
    const char *fields[] = {"endpoints", "signalRelays", "stunServers"};
    const size_t counts[] = {FIPSD_NOSTR_ENDPOINTS_MAX, FIPSD_NOSTR_RELAYS_MAX, FIPSD_NOSTR_STUN_MAX};
    for (size_t i = 0; i < 3; ++i) {
        json_t *v = json_loads(body, 0, NULL), *array = json_array(); CHECK(v && array);
        for (size_t k = 0; k <= counts[i]; ++k) {
            json_t *entry = i ? json_string("test") : json_deep_copy(json_array_get(json_object_get(v, "endpoints"), 0));
            CHECK(json_array_append_new(array, entry) == 0);
        }
        CHECK(json_object_set_new(v, fields[i], array) == 0);
        char *text = json_dumps(v, JSON_COMPACT); CHECK(text); nostr_event_set_content(e, text); free(text); json_decref(v);
        OK(fn_sign(e, id));
        CHECK(fipsd_nostr_advert_parse(e, "private-app", UINT64_C(2000000000000), 3600000, &parsed, &until) != FIPSD_NOSTR_OK);
    }
    /* Version, NAT dependencies, unknown transport dropping, private endpoint
     * dropping and absent expiration's max-age fallback are upstream rules. */
    json_t *v = json_loads(body, 0, NULL); CHECK(v);
    CHECK(json_object_set_new(v, "version", json_integer(2)) == 0);
    char *text = json_dumps(v, JSON_COMPACT); nostr_event_set_content(e, text); free(text); json_decref(v); OK(fn_sign(e, id));
    CHECK(fipsd_nostr_advert_parse(e, "private-app", UINT64_C(2000000000000), 3600000, &parsed, &until) == FIPSD_NOSTR_INVALID);
    const char *unusable[] = {"127.0.0.1:1", "10.1.1.1:1", "100.64.0.1:1", "169.254.0.1:1", "192.0.2.1:1", "203.0.113.1:1", "[::1]:1", "[fc00::1]:1", "localhost:1", "example.com:0", "nat"};
    for (size_t i = 0; i < sizeof(unusable)/sizeof(unusable[0]); ++i) {
        strcpy(a.endpoints[0].addr, unusable[i]);
        char *b = fipsd_nostr_advert_content(&a); CHECK(b); nostr_event_set_content(e, b); free(b); OK(fn_sign(e, id));
        CHECK(fipsd_nostr_advert_parse(e, "private-app", UINT64_C(2000000000000), 3600000, &parsed, &until) == FIPSD_NOSTR_INVALID);
    }
    a.endpoints[0].transport = FIPSD_NOSTR_UDP; strcpy(a.endpoints[0].addr, "nat");
    CHECK(!fipsd_nostr_advert_content(&a));
    a.endpoint_count = FIPSD_NOSTR_ENDPOINTS_MAX + 1; CHECK(!fipsd_nostr_advert_content(&a));
    a.endpoint_count = 1; a.relay_count = FIPSD_NOSTR_RELAYS_MAX + 1; CHECK(!fipsd_nostr_advert_content(&a));
    nostr_event_free(e); free(body);
    fipsd_nostr_inbox inbox = {.count = 1}; strcpy(inbox.relays[0], "wss://EXAMPLE.com:443");
    OK(fipsd_nostr_inbox_build(id, &inbox, 2000000000, &e));
    nostr_tags_append(e->tags, nostr_tag_new("relay", "wss://example.com", NULL));
    nostr_tags_append(e->tags, nostr_tag_new("relay", "wss://example.com/", NULL));
    nostr_tags_append(e->tags, nostr_tag_new("r", "wss://write.example/", "write", NULL));
    nostr_tags_append(e->tags, nostr_tag_new("relay", "https://not-a-relay.example/", NULL));
    OK(fn_sign(e, id)); OK(fipsd_nostr_inbox_parse(e, &inbox));
    CHECK(inbox.count == 1 && !strcmp(inbox.relays[0], "wss://example.com"));
    for (unsigned i = 0; i < FIPSD_NOSTR_RELAYS_MAX; ++i) {
        char url[80]; snprintf(url, sizeof(url), "wss://relay%u.example/", i);
        nostr_tags_append(e->tags, nostr_tag_new("r", url, NULL));
    }
    OK(fn_sign(e, id)); CHECK(fipsd_nostr_inbox_parse(e, &inbox) == FIPSD_NOSTR_LIMIT);
    inbox.count = FIPSD_NOSTR_RELAYS_MAX + 1; NostrEvent *none = NULL;
    CHECK(fipsd_nostr_inbox_build(id, &inbox, 2000000000, &none) == FIPSD_NOSTR_LIMIT && !none);
    nostr_event_free(e);
    inbox.count = 0; e = NULL; OK(fipsd_nostr_inbox_build(id, &inbox, 2000000000, &e));
    CHECK(nostr_event_check_signature(e)); OK(fipsd_nostr_inbox_parse(e, &inbox)); CHECK(inbox.count == 0);
    nostr_event_free(e); fips_identity_destroy(id); ++checks;
}
/* Local advert assembly (nostrc-r4aeo) against upstream: content strings are
 * src/nostr/tests/mod.rs's (serde field order; signalRelays/stunServers only
 * beside udp:nat), the filter cases its validate_overlay_advert tests, the
 * source rows driver.rs build_overlay_advert's precedence and the external
 * rows config/transport.rs parse_external_advert_addr / parse_bind_port. */
#define BODY(e) "{\"identifier\":\"fips-overlay-v1\",\"version\":1,\"endpoints\":[" e "]"
static void meta(fipsd_nostr_advert *a)
{
    a->has_relays = a->has_stun = true; a->relay_count = a->stun_count = 1;
    strcpy(a->relays[0], "wss://relay.example"); strcpy(a->stun[0], "stun:stun.example.org:3478");
}
static void expect_body(const fipsd_nostr_advert *a, const char *want)
{
    char *body = fipsd_nostr_advert_content(a); CHECK(body);
    if (strcmp(body, want)) { fprintf(stderr, "got  %s\nwant %s\n", body, want); CHECK(false); }
    free(body); ++checks;
}
static void local_adverts(void)
{
    const char *tcp8 = BODY("{\"transport\":\"tcp\",\"addr\":\"8.8.8.8:443\"}") "}";
    fipsd_nostr_advert a = {.endpoint_count = 1, .endpoints = {{FIPSD_NOSTR_TCP, "8.8.8.8:443"}}};
    expect_body(&a, tcp8); /* signed_overlay_advert_event_from */
    a = (fipsd_nostr_advert){.endpoint_count = 2, .endpoints = {{FIPSD_NOSTR_TCP, "203.0.113.10:443"},
        {FIPSD_NOSTR_TOR, "exampleonion.onion:1234"}}};
    expect_body(&a, BODY("{\"transport\":\"tcp\",\"addr\":\"203.0.113.10:443\"},"
        "{\"transport\":\"tor\",\"addr\":\"exampleonion.onion:1234\"}") "}"); /* ..._without_nat_metadata */
    meta(&a); CHECK(fipsd_nostr_advert_publishable(&a)); /* documentation range never published */
    CHECK(a.endpoint_count == 1 && !a.has_relays && !a.has_stun);
    expect_body(&a, BODY("{\"transport\":\"tor\",\"addr\":\"exampleonion.onion:1234\"}") "}");
    const char *nat = BODY("{\"transport\":\"udp\",\"addr\":\"nat\"}") ",\"signalRelays\":[\"wss://relay.example\"],"
        "\"stunServers\":[\"stun:stun.example.org:3478\"]}";
    a = (fipsd_nostr_advert){.endpoint_count = 1, .endpoints = {{FIPSD_NOSTR_UDP, "nat"}}}; meta(&a);
    expect_body(&a, nat); /* serializes_nat_overlay_advert_with_metadata */
    CHECK(fipsd_nostr_advert_publishable(&a)); expect_body(&a, nat);
    a.stun_count = 0; CHECK(!fipsd_nostr_advert_publishable(&a)); /* udp:nat requires non-empty stunServers */
    a = (fipsd_nostr_advert){.endpoint_count = 1, .endpoints = {{FIPSD_NOSTR_UDP, "nat"}}}; meta(&a); a.relay_count = 0;
    CHECK(!fipsd_nostr_advert_publishable(&a)); /* ... and signalRelays */
    a = (fipsd_nostr_advert){.endpoint_count = 3, .endpoints = {{FIPSD_NOSTR_TCP, "192.168.1.10:443"},
        {FIPSD_NOSTR_UDP, "100.64.1.2:2121"}, {FIPSD_NOSTR_TCP, "8.8.8.8:443"}}}; meta(&a);
    CHECK(fipsd_nostr_advert_publishable(&a)); expect_body(&a, tcp8); /* ..._filters_unroutable_direct_endpoints */
    a = (fipsd_nostr_advert){.endpoint_count = 2, .endpoints = {{FIPSD_NOSTR_TCP, "127.0.0.1:443"}, {FIPSD_NOSTR_UDP, "10.0.0.2:2121"}}};
    CHECK(!fipsd_nostr_advert_publishable(&a) && !a.endpoint_count); /* ..._rejects_only_unroutable_direct_endpoints */
    a = (fipsd_nostr_advert){.endpoint_count = 2, .endpoints = {{FIPSD_NOSTR_UDP, "nat"}, {FIPSD_NOSTR_TCP, "8.8.8.8:443"}}}; meta(&a);
    CHECK(fipsd_nostr_advert_publishable(&a));
    expect_body(&a, BODY("{\"transport\":\"udp\",\"addr\":\"nat\"},{\"transport\":\"tcp\",\"addr\":\"8.8.8.8:443\"}")
        ",\"signalRelays\":[\"wss://relay.example\"],\"stunServers\":[\"stun:stun.example.org:3478\"]}");
    const fipsd_nostr_transport U = FIPSD_NOSTR_UDP, T = FIPSD_NOSTR_TCP, O = FIPSD_NOSTR_TOR;
    const struct { fipsd_nostr_advert_source s; const char *want; const char *why; } rows[] = {
        {{.transport = U, .advertise = true, .local = "0.0.0.0:2121"}, "nat", NULL},
        {{.transport = U, .advertise = false, .local = "0.0.0.0:2121"}, NULL, NULL},
        {{.transport = U, .advertise = true, .is_public = true, .external = "198.51.100.9:2121", .local = "1.2.3.4:2121"}, "198.51.100.9:2121", NULL},
        {{.transport = U, .advertise = true, .is_public = true, .local = "1.2.3.4:2121", .reflexive = "5.6.7.8"}, "1.2.3.4:2121", NULL},
        {{.transport = U, .advertise = true, .is_public = true, .local = "0.0.0.0:2121", .reflexive = "198.18.0.7"}, "198.18.0.7:2121", NULL},
        {{.transport = U, .advertise = true, .is_public = true, .local = "[::]:2121", .reflexive = "2001:db8:0:0:0:0:0:5"}, "[2001:db8::5]:2121", NULL},
        {{.transport = U, .advertise = true, .is_public = true, .local = "0.0.0.0:2121"}, NULL, "STUN observation failed"},
        {{.transport = U, .advertise = true, .is_public = true}, NULL, NULL},
        {{.transport = T, .advertise = true, .external = "203.0.113.45:8443", .local = "0.0.0.0:8443"}, "203.0.113.45:8443", NULL},
        {{.transport = T, .advertise = true, .local = "8.8.8.8:443"}, "8.8.8.8:443", NULL},
        {{.transport = T, .advertise = true, .local = "[2001:db8:0:0::1]:443"}, "[2001:db8::1]:443", NULL},
        {{.transport = T, .advertise = true, .local = "0.0.0.0:8443", .reflexive = "5.6.7.8"}, NULL, "no transports.tcp.external_addr"},
        {{.transport = T, .advertise = true}, NULL, NULL},
        {{.transport = T, .advertise = false, .external = "8.8.8.8:443"}, NULL, NULL},
        {{.transport = O, .advertise = true, .onion = "exampleonion.onion", .advertised_port = 1234}, "exampleonion.onion:1234", NULL},
        {{.transport = O, .advertise = true, .advertised_port = 443}, NULL, NULL},
        {{.transport = O, .advertise = false, .onion = "exampleonion.onion", .advertised_port = 443}, NULL, NULL},
    };
    for (size_t i = 0; i < sizeof rows / sizeof *rows; ++i) {
        fipsd_nostr_endpoint e; const char *why = "unset";
        bool got = fipsd_nostr_advert_source_endpoint(&rows[i].s, &e, &why);
        if (got != (rows[i].want != NULL) || (got && (e.transport != rows[i].s.transport || strcmp(e.addr, rows[i].want))) ||
            (rows[i].why ? !why || !strstr(why, rows[i].why) : why != NULL)) {
            fprintf(stderr, "source row %zu: got=%d addr=%s why=%s\n", i, got, got ? e.addr : "-", why ? why : "(null)"); CHECK(false);
        }
        ++checks;
    }
    const struct { const char *raw, *bind, *want; } external[] = {
        {"203.0.113.45", "0.0.0.0:8443", "203.0.113.45:8443"}, {"203.0.113.45:9000", "0.0.0.0:8443", "203.0.113.45:9000"},
        {"1.2.3.4", "0.0.0.0:2121", "1.2.3.4:2121"}, {"2001:db8::1", "[::]:443", "[2001:db8::1]:443"},
        {"[2001:DB8:0::1]:80", "0.0.0.0:1", "[2001:db8::1]:80"}, {"::ffff:1.2.3.4", "0.0.0.0:7", "[::ffff:1.2.3.4]:7"},
        {"1.2.3.4:080", "0.0.0.0:1", "1.2.3.4:80"}, {"1.2.3.4", NULL, NULL}, {"1.2.3.4:5", NULL, NULL},
        {"1.2.3.4", "not-a-socket-addr", NULL}, {"1.2.3.4:5", "localhost:443", NULL}, {"example.com:443", "0.0.0.0:1", NULL},
        {"01.2.3.4", "0.0.0.0:1", NULL}, {"1.2.3.4:65536", "0.0.0.0:1", NULL}, {"[::1]", "0.0.0.0:1", NULL},
        {"1.2.3.4:", "0.0.0.0:1", NULL}, {"", "0.0.0.0:1", NULL}, {NULL, "0.0.0.0:1", NULL},
    };
    for (size_t i = 0; i < sizeof external / sizeof *external; ++i) {
        char out[FIPSD_NOSTR_ADDR_MAX + 1] = "";
        bool got = fipsd_nostr_advert_external(external[i].raw, external[i].bind, out);
        if (got != (external[i].want != NULL) || (got && strcmp(out, external[i].want))) {
            fprintf(stderr, "external row %zu (%s, %s): got=%d %s\n", i, external[i].raw ? external[i].raw : "(null)",
                external[i].bind ? external[i].bind : "(null)", got, out); CHECK(false);
        }
        ++checks;
    }
    /* What the builder publishes is what a peer parses: tcp + tor, no NAT lists. */
    fips_identity *id = identity("0000000000000000000000000000000000000000000000000000000000000004");
    a = (fipsd_nostr_advert){.endpoint_count = 2, .endpoints = {{FIPSD_NOSTR_TCP, "8.8.8.8:443"},
        {FIPSD_NOSTR_TOR, "exampleonion.onion:443"}}}; meta(&a);
    CHECK(fipsd_nostr_advert_publishable(&a));
    NostrEvent *e = NULL; OK(fipsd_nostr_advert_build(id, &a, FIPSD_NOSTR_IDENTIFIER, 2000000000, 3600, &e));
    fipsd_nostr_advert parsed; uint64_t until;
    OK(fipsd_nostr_advert_parse(e, FIPSD_NOSTR_IDENTIFIER, UINT64_C(2000000000000), 3600000, &parsed, &until));
    CHECK(parsed.endpoint_count == 2 && parsed.endpoints[1].transport == FIPSD_NOSTR_TOR &&
        !strcmp(parsed.endpoints[1].addr, "exampleonion.onion:443") && !parsed.has_relays && !parsed.has_stun);
    nostr_event_free(e); fips_identity_destroy(id); ++checks;
}
static void properties(void)
{
    fips_seeded_entropy rng = {0x0123456789abcdef};
    for (unsigned i = 0; i < 256; ++i) {
        fipsd_nostr_traversal p = i & 1 ? answer_fixture : offer_fixture;
        p.issued_at = fips_splitmix64_next(&rng); p.expires_at = fips_splitmix64_next(&rng);
        p.offer_received_at = fips_splitmix64_next(&rng); p.has_offer_received_at = i & 2;
        p.has_punch = i & 4; p.has_reflexive = i & 8; p.has_reason = i & 16; p.has_stun = i & 32;
        p.accepted = i & 64; p.local_count = i % (FIPSD_NOSTR_CANDIDATES_MAX + 1);
        p.punch = (fipsd_nostr_punch){UINT64_MAX, fips_splitmix64_next(&rng), fips_splitmix64_next(&rng)};
        strcpy(p.reason, "rejected: \"busy\"\n\t\\ unicode \xe2\x98\x83");
        for (size_t k = 0; k < p.local_count; ++k) {
            strcpy(p.local[k].protocol, "udp"); strcpy(p.local[k].ip, "2001:db8::1"); p.local[k].port = (uint16_t)fips_splitmix64_next(&rng);
        }
        char *text = fipsd_nostr_traversal_content(&p); CHECK(text);
        fipsd_nostr_traversal decoded; OK(fipsd_nostr_traversal_parse_content(text, strlen(text), &decoded));
        char *again = fipsd_nostr_traversal_content(&decoded); CHECK(again && !strcmp(text, again)); free(text); free(again); ++checks;
    }
}
int main(void)
{
    CHECK(sodium_init() >= 0);
    CHECK(fips_identity_init_secp(fips_fixed_entropy_adapter(&fixed), &secp) == FIPS_IDENTITY_OK);
    char *text = load("nostr.json", NULL); json_t *doc = json_loads(text, 0, NULL); CHECK(doc); free(text);
    CHECK(!strcmp(sval(doc, "pin"), "64f5b4d482cfe14abad108ffc2d92128f7658311"));
    json_t *cases = json_object_get(doc, "cases"); CHECK(json_array_size(cases) == 4);
    for (size_t i = 0; i < json_array_size(cases); ++i) fixture(json_array_get(cases, i));
    freshness(); bounds(); advert_inbox_bounds(); local_adverts(); properties();
    json_decref(doc); fips_identity_destroy_secp(secp);
    printf("Nostr: 4 fixtures, 256 seeded payload round trips, %u checks passed\n", checks); return 0;
}
