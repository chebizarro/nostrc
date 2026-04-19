#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nostr-event.h"
#include "nostr-tag.h"
#include "nostr/nip61/nip61.h"

/* ---- Preferences ---- */

static void test_parse_prefs(void) {
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, NOSTR_NIP61_KIND_NUTZAP_PREFS);
    nostr_event_set_content(ev, "");

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags,
        nostr_tag_new("mint", "https://mint.example.com", "sat", NULL));
    nostr_tags_append(tags,
        nostr_tag_new("mint", "https://mint2.example.com", "usd", "pk123", NULL));
    nostr_tags_append(tags,
        nostr_tag_new("relay", "wss://relay.example.com", NULL));
    nostr_tags_append(tags,
        nostr_tag_new("p2pk", NULL));

    NostrNip61Mint mints[5];
    const char *relays[5];
    NostrNip61Prefs prefs;

    int rc = nostr_nip61_parse_prefs(ev, mints, 5, relays, 5, &prefs);
    assert(rc == 0);
    assert(prefs.mint_count == 2);
    assert(strcmp(prefs.mints[0].url, "https://mint.example.com") == 0);
    assert(strcmp(prefs.mints[0].unit, "sat") == 0);
    assert(prefs.mints[0].pubkey == NULL);
    assert(strcmp(prefs.mints[1].url, "https://mint2.example.com") == 0);
    assert(strcmp(prefs.mints[1].unit, "usd") == 0);
    assert(strcmp(prefs.mints[1].pubkey, "pk123") == 0);
    assert(prefs.relay_count == 1);
    assert(strcmp(prefs.relays[0], "wss://relay.example.com") == 0);
    assert(prefs.require_p2pk == true);

    nostr_event_free(ev);
}

static void test_parse_prefs_wrong_kind(void) {
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, 1);

    NostrNip61Mint mints[5];
    const char *relays[5];
    NostrNip61Prefs prefs;
    assert(nostr_nip61_parse_prefs(ev, mints, 5, relays, 5, &prefs) == -EINVAL);

    nostr_event_free(ev);
}

static void test_parse_prefs_null_inputs(void) {
    NostrNip61Mint mints[5];
    const char *relays[5];
    NostrNip61Prefs prefs;
    assert(nostr_nip61_parse_prefs(NULL, mints, 5, relays, 5, &prefs) == -EINVAL);
}

static void test_create_prefs(void) {
    NostrEvent *ev = nostr_event_new();

    NostrNip61Mint mints[] = {
        { .url = "https://mint.com", .unit = "sat", .pubkey = NULL },
        { .url = "https://mint2.com", .unit = "usd", .pubkey = "pk456" },
    };
    const char *relays[] = { "wss://relay.com" };

    int rc = nostr_nip61_create_prefs(ev, mints, 2, relays, 1, true);
    assert(rc == 0);
    assert(nostr_event_get_kind(ev) == NOSTR_NIP61_KIND_NUTZAP_PREFS);

    /* Verify tags roundtrip */
    NostrNip61Mint out_mints[5];
    const char *out_relays[5];
    NostrNip61Prefs prefs;
    rc = nostr_nip61_parse_prefs(ev, out_mints, 5, out_relays, 5, &prefs);
    assert(rc == 0);
    assert(prefs.mint_count == 2);
    assert(strcmp(prefs.mints[0].url, "https://mint.com") == 0);
    assert(strcmp(prefs.mints[1].pubkey, "pk456") == 0);
    assert(prefs.relay_count == 1);
    assert(prefs.require_p2pk == true);

    nostr_event_free(ev);
}

static void test_prefs_accepts_mint(void) {
    NostrNip61Mint mints[] = {
        { .url = "https://mint.com", .unit = "sat", .pubkey = NULL },
    };
    NostrNip61Prefs prefs = {
        .mints = mints, .mint_count = 1,
        .relays = NULL, .relay_count = 0,
        .require_p2pk = false
    };

    assert(nostr_nip61_prefs_accepts_mint(&prefs, "https://mint.com") == true);
    assert(nostr_nip61_prefs_accepts_mint(&prefs, "https://other.com") == false);
    assert(nostr_nip61_prefs_accepts_mint(&prefs, NULL) == false);
    assert(nostr_nip61_prefs_accepts_mint(NULL, "x") == false);
}

/* ---- Nutzap event ---- */

static void test_parse_nutzap(void) {
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, NOSTR_NIP61_KIND_NUTZAP);
    nostr_event_set_content(ev, "");

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags,
        nostr_tag_new("proofs", "[{\"amount\":64}]", NULL));
    nostr_tags_append(tags,
        nostr_tag_new("u", "https://mint.example.com", NULL));
    nostr_tags_append(tags,
        nostr_tag_new("p", "recipient_pubkey_hex", NULL));
    nostr_tags_append(tags,
        nostr_tag_new("e", "event123", "wss://relay.com", NULL));
    nostr_tags_append(tags,
        nostr_tag_new("a", "30023:pk:slug", NULL));

    NostrNip61Nutzap nz;
    int rc = nostr_nip61_parse_nutzap(ev, &nz);
    assert(rc == 0);
    assert(nz.proofs_json != NULL);
    assert(strstr(nz.proofs_json, "64") != NULL);
    assert(strcmp(nz.mint_url, "https://mint.example.com") == 0);
    assert(strcmp(nz.recipient_pubkey, "recipient_pubkey_hex") == 0);
    assert(strcmp(nz.zapped_event_id, "event123") == 0);
    assert(strcmp(nz.zapped_relay, "wss://relay.com") == 0);
    assert(strcmp(nz.addressable_ref, "30023:pk:slug") == 0);

    nostr_event_free(ev);
}

static void test_parse_nutzap_minimal(void) {
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, NOSTR_NIP61_KIND_NUTZAP);

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags,
        nostr_tag_new("proofs", "[]", NULL));
    nostr_tags_append(tags,
        nostr_tag_new("u", "https://mint.com", NULL));
    nostr_tags_append(tags,
        nostr_tag_new("p", "pk", NULL));

    NostrNip61Nutzap nz;
    int rc = nostr_nip61_parse_nutzap(ev, &nz);
    assert(rc == 0);
    assert(nz.zapped_event_id == NULL);
    assert(nz.zapped_relay == NULL);
    assert(nz.addressable_ref == NULL);

    nostr_event_free(ev);
}

static void test_parse_nutzap_missing_required(void) {
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, NOSTR_NIP61_KIND_NUTZAP);

    /* No proofs tag */
    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags,
        nostr_tag_new("u", "https://mint.com", NULL));
    nostr_tags_append(tags,
        nostr_tag_new("p", "pk", NULL));

    NostrNip61Nutzap nz;
    assert(nostr_nip61_parse_nutzap(ev, &nz) == -EINVAL);

    nostr_event_free(ev);
}

static void test_parse_nutzap_wrong_kind(void) {
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, 1);

    NostrNip61Nutzap nz;
    assert(nostr_nip61_parse_nutzap(ev, &nz) == -EINVAL);

    nostr_event_free(ev);
}

static void test_create_nutzap(void) {
    NostrEvent *ev = nostr_event_new();

    int rc = nostr_nip61_create_nutzap(ev,
        "[{\"amount\":32},{\"amount\":16}]",
        "https://mint.com",
        "recipient_pk",
        "event_id_123",
        "wss://relay.com",
        "30023:pk:d");
    assert(rc == 0);
    assert(nostr_event_get_kind(ev) == NOSTR_NIP61_KIND_NUTZAP);

    /* Verify tags roundtrip */
    NostrNip61Nutzap nz;
    rc = nostr_nip61_parse_nutzap(ev, &nz);
    assert(rc == 0);
    assert(strcmp(nz.mint_url, "https://mint.com") == 0);
    assert(strcmp(nz.recipient_pubkey, "recipient_pk") == 0);
    assert(strcmp(nz.zapped_event_id, "event_id_123") == 0);
    assert(strcmp(nz.addressable_ref, "30023:pk:d") == 0);

    nostr_event_free(ev);
}

static void test_create_nutzap_minimal(void) {
    NostrEvent *ev = nostr_event_new();

    int rc = nostr_nip61_create_nutzap(ev,
        "[]", "https://mint.com", "pk",
        NULL, NULL, NULL);
    assert(rc == 0);

    const NostrTags *tags = nostr_event_get_tags(ev);
    /* Should have exactly 3 tags: proofs, u, p */
    assert(nostr_tags_size(tags) == 3);

    nostr_event_free(ev);
}

static void test_create_nutzap_null_required(void) {
    NostrEvent *ev = nostr_event_new();
    assert(nostr_nip61_create_nutzap(ev, NULL, "u", "p", NULL, NULL, NULL) == -EINVAL);
    assert(nostr_nip61_create_nutzap(ev, "[]", NULL, "p", NULL, NULL, NULL) == -EINVAL);
    assert(nostr_nip61_create_nutzap(ev, "[]", "u", NULL, NULL, NULL, NULL) == -EINVAL);
    nostr_event_free(ev);
}

/* ---- Utilities ---- */

static void test_valid_mint_url(void) {
    assert(nostr_nip61_is_valid_mint_url("https://mint.example.com"));
    assert(nostr_nip61_is_valid_mint_url("http://localhost:3338"));
    assert(nostr_nip61_is_valid_mint_url("http://127.0.0.1:3338"));
    assert(!nostr_nip61_is_valid_mint_url("http://other.com"));
    assert(!nostr_nip61_is_valid_mint_url(""));
    assert(!nostr_nip61_is_valid_mint_url(NULL));
    assert(!nostr_nip61_is_valid_mint_url("ftp://x"));
}

static void test_proofs_total_amount(void) {
    const char *json = "[{\"amount\":32,\"id\":\"x\"},{\"amount\":16,\"id\":\"y\"},{\"amount\":64,\"id\":\"z\"}]";
    assert(nostr_nip61_proofs_total_amount(json) == 112);
}

static void test_proofs_total_amount_empty(void) {
    assert(nostr_nip61_proofs_total_amount("[]") == 0);
    assert(nostr_nip61_proofs_total_amount(NULL) == 0);
}

static void test_proofs_count(void) {
    const char *json = "[{\"amount\":32},{\"amount\":16}]";
    assert(nostr_nip61_proofs_count(json) == 2);
    assert(nostr_nip61_proofs_count("[]") == 0);
    assert(nostr_nip61_proofs_count(NULL) == 0);
}

int main(void) {
    test_parse_prefs();
    test_parse_prefs_wrong_kind();
    test_parse_prefs_null_inputs();
    test_create_prefs();
    test_prefs_accepts_mint();
    test_parse_nutzap();
    test_parse_nutzap_minimal();
    test_parse_nutzap_missing_required();
    test_parse_nutzap_wrong_kind();
    test_create_nutzap();
    test_create_nutzap_minimal();
    test_create_nutzap_null_required();
    test_valid_mint_url();
    test_proofs_total_amount();
    test_proofs_total_amount_empty();
    test_proofs_count();
    printf("nip61 ok\n");
    return 0;
}
