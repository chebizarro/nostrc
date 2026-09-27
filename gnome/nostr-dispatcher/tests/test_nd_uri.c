/* NIP-21 parsing, TLV kind extraction, relay-hint filter, canonical form. */
#include "nd-error.h"
#include "nd-uri.h"

#include <stdlib.h>
#include <string.h>

#include "nostr/nip19/nip19.h"

static const char *ID = "d1b3f0c1e5a2d4f60718293a4b5c6d7e8f9011223344556677889900aabbccdd";
static const char *PK = "3bf0c63fcb93463407af97a5e5ee64fa883d107ef9e558472c4eb9aaaefa459d";

static char *make_nevent(int kind, gboolean with_kind, const char *relay) {
  /* Build the TLV by hand so kind 0 can be encoded (the library encoder
   * omits kind <= 0). */
  GByteArray *b = g_byte_array_new();
  guint8 id[32];
  for (int i = 0; i < 32; i++) id[i] = (guint8)strtoul((char[]){ID[2*i], ID[2*i+1], 0}, NULL, 16);
  guint8 h0[2] = {0, 32};
  g_byte_array_append(b, h0, 2); g_byte_array_append(b, id, 32);
  if (relay) {
    guint8 h1[2] = {1, (guint8)strlen(relay)};
    g_byte_array_append(b, h1, 2); g_byte_array_append(b, (const guint8 *)relay, strlen(relay));
  }
  if (with_kind) {
    guint8 h3[6] = {3, 4, (guint8)(kind >> 24), (guint8)(kind >> 16), (guint8)(kind >> 8), (guint8)kind};
    g_byte_array_append(b, h3, 6);
  }
  char *bech = NULL;
  g_assert_cmpint(nostr_nip19_encode_tlv("nevent", b->data, b->len, &bech), ==, 0);
  g_byte_array_unref(b);
  char *uri = g_strconcat("nostr:", bech, NULL);
  free(bech);
  return uri;
}

static void test_note_has_no_kind(void) {
  guint8 id[32] = {0};
  id[0] = 0xd1;
  char *bech = NULL;
  g_assert_cmpint(nostr_nip19_encode_note(id, &bech), ==, 0);
  g_autofree char *uri = g_strconcat("nostr:", bech, NULL);
  free(bech);
  g_autoptr(GError) err = NULL;
  g_autoptr(NdTarget) t = nd_target_parse_uri(uri, &err);
  g_assert_no_error(err);
  g_assert_cmpint(t->entity, ==, ND_ENTITY_EVENT);
  g_assert_cmpint(t->kind, ==, -1);
  g_assert_true(g_str_has_prefix(t->id_hex, "d1000000"));
  g_autofree char *canon = nd_target_to_uri(t);
  g_assert_cmpstr(canon, ==, uri); /* note stays note */
}

static void test_nevent_tlv_kind(void) {
  g_autofree char *with = make_nevent(30023, TRUE, "wss://relay.example.com");
  g_autofree char *without = make_nevent(0, FALSE, NULL);
  g_autofree char *kind0 = make_nevent(0, TRUE, NULL);

  g_autoptr(NdTarget) a = nd_target_parse_uri(with, NULL);
  g_assert_nonnull(a);
  g_assert_cmpint(a->kind, ==, 30023);
  g_assert_cmpstr(a->id_hex, ==, ID);
  g_assert_cmpstr(a->relays[0], ==, "wss://relay.example.com");

  g_autoptr(NdTarget) b = nd_target_parse_uri(without, NULL);
  g_assert_cmpint(b->kind, ==, -1); /* kind TLV is optional */

  /* Present kind 0 must not be confused with "absent". */
  g_autoptr(NdTarget) c = nd_target_parse_uri(kind0, NULL);
  g_assert_cmpint(c->kind, ==, 0);
  guint32 k = 99;
  g_assert_true(nd_bech32_tlv_kind(kind0 + strlen("nostr:"), &k));
  g_assert_cmpuint(k, ==, 0);
  g_assert_false(nd_bech32_tlv_kind(without + strlen("nostr:"), &k));

  /* Canonical re-encode round-trips kind + relay. */
  g_autofree char *canon = nd_target_to_uri(a);
  g_autoptr(NdTarget) a2 = nd_target_parse_uri(canon, NULL);
  g_assert_cmpint(a2->kind, ==, 30023);
  g_assert_cmpstr(a2->relays[0], ==, "wss://relay.example.com");
  g_autofree char *canon0 = nd_target_to_uri(c);
  g_autoptr(NdTarget) c2 = nd_target_parse_uri(canon0, NULL);
  g_assert_cmpint(c2->kind, ==, 0);
}

static void test_naddr_and_profiles(void) {
  const char *relays[] = {"wss://r.example"};
  NostrEntityPointer ap = {.public_key = (char *)PK, .kind = 30023,
                           .identifier = (char *)"my-article",
                           .relays = (char **)relays, .relays_count = 1};
  char *bech = NULL;
  g_assert_cmpint(nostr_nip19_encode_naddr(&ap, &bech), ==, 0);
  g_autofree char *uri = g_strconcat("web+nostr:", bech, NULL);
  free(bech);
  g_autoptr(NdTarget) t = nd_target_parse_uri(uri, NULL);
  g_assert_nonnull(t);
  g_assert_cmpint(t->entity, ==, ND_ENTITY_ADDRESS);
  g_assert_cmpint(t->kind, ==, 30023);
  g_assert_cmpstr(t->identifier, ==, "my-article");
  g_assert_cmpstr(t->pubkey_hex, ==, PK);

  guint8 pk[32] = {0x3b};
  g_assert_cmpint(nostr_nip19_encode_npub(pk, &bech), ==, 0);
  g_autofree char *npub = g_strconcat("NOSTR:", bech, "?utm=x", NULL); /* case + junk */
  free(bech);
  g_autoptr(NdTarget) p = nd_target_parse_uri(npub, NULL);
  g_assert_nonnull(p);
  g_assert_cmpint(p->entity, ==, ND_ENTITY_PROFILE);
  g_assert_cmpint(p->kind, ==, 0);
  g_autofree char *canon = nd_target_to_uri(p);
  g_assert_true(g_str_has_prefix(canon, "nostr:npub1"));
  g_assert_null(strchr(canon, '?')); /* raw query never forwarded */
}

static void test_rejections(void) {
  guint8 sk[32] = {1};
  char *bech = NULL;
  g_assert_cmpint(nostr_nip19_encode_nsec(sk, &bech), ==, 0);
  g_autofree char *nsec = g_strconcat("nostr:", bech, NULL);
  free(bech);
  g_autoptr(GError) err = NULL;
  g_assert_null(nd_target_parse_uri(nsec, &err));
  g_assert_error(err, ND_ERROR, ND_ERROR_FORBIDDEN);
  g_assert_null(strstr(err->message, "nsec1")); /* never echoed */
  g_clear_error(&err);

  g_assert_null(nd_target_parse_uri("nostr:ncryptsec1qqqq", &err));
  g_assert_error(err, ND_ERROR, ND_ERROR_FORBIDDEN);
  g_clear_error(&err);

  g_assert_cmpint(nostr_nip19_encode_nrelay("wss://r.example", &bech), ==, 0);
  g_autofree char *nrelay = g_strconcat("nostr:", bech, NULL);
  free(bech);
  g_assert_null(nd_target_parse_uri(nrelay, &err));
  g_assert_error(err, ND_ERROR, ND_ERROR_INVALID_URI);
  g_clear_error(&err);

  const char *junk[] = {"", "nostr:", "nostr:note1", "https://example.com",
                        "nostr:npub1invalidchecksum", NULL};
  for (int i = 0; junk[i]; i++) {
    g_assert_null(nd_target_parse_uri(junk[i], &err));
    g_assert_error(err, ND_ERROR, ND_ERROR_INVALID_URI);
    g_clear_error(&err);
  }
}

/* nostrc-prqu.7: the transition-release `nostr://open?` forms are gone. */
static void test_legacy_notify_forms_rejected(void) {
  g_autofree char *dm = g_strdup_printf("nostr://open?event=%s", ID);
  g_autofree char *grp = g_strdup_printf("nostr://open?group=abc&event=%s", ID);
  const char *forms[] = {dm, grp, "nostr://open?event=nothex", NULL};
  for (int i = 0; forms[i]; i++) {
    g_autoptr(GError) err = NULL;
    g_assert_null(nd_target_parse_uri(forms[i], &err));
    g_assert_error(err, ND_ERROR, ND_ERROR_INVALID_URI);
  }
}

static void test_relay_filter(void) {
  g_assert_true(nd_relay_url_acceptable("wss://relay.damus.io"));
  g_assert_true(nd_relay_url_acceptable("wss://relay.example.com:4443/path"));
  g_assert_true(nd_relay_url_acceptable("ws://localhost:7777"));
  g_assert_true(nd_relay_url_acceptable("ws://127.0.0.1:7777"));
  g_assert_true(nd_relay_url_acceptable("ws://[::1]:7777"));
  g_assert_false(nd_relay_url_acceptable("ws://relay.example.com"));
  g_assert_false(nd_relay_url_acceptable("ws://127.evil.example"));
  g_assert_false(nd_relay_url_acceptable("ws://169.254.169.254/latest"));
  g_assert_false(nd_relay_url_acceptable("http://relay.example.com"));
  g_assert_false(nd_relay_url_acceptable("file:///etc/passwd"));
  g_assert_false(nd_relay_url_acceptable("wss://user:pw@relay.example.com"));
  g_assert_false(nd_relay_url_acceptable("wss://relay example.com"));
  g_assert_false(nd_relay_url_acceptable("wss://"));

  const char *in[] = {"ws://evil.example", "wss://a.example", "wss://a.example",
                      "wss://b.example", "wss://c.example", "wss://d.example"};
  g_auto(GStrv) out = nd_relays_filter(in, G_N_ELEMENTS(in));
  g_assert_cmpuint(g_strv_length(out), ==, ND_URI_MAX_RELAYS);
  g_assert_cmpstr(out[0], ==, "wss://a.example");
  g_assert_cmpstr(out[1], ==, "wss://b.example");
  g_assert_cmpstr(out[2], ==, "wss://c.example");

  /* Hints are filtered before re-encoding, too. */
  g_autofree char *uri = make_nevent(1, TRUE, "ws://10.0.0.1:80");
  g_autoptr(NdTarget) t = nd_target_parse_uri(uri, NULL);
  g_assert_nonnull(t);
  g_assert_null(t->relays[0]);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nd/uri/note", test_note_has_no_kind);
  g_test_add_func("/nd/uri/nevent-tlv-kind", test_nevent_tlv_kind);
  g_test_add_func("/nd/uri/naddr-profile", test_naddr_and_profiles);
  g_test_add_func("/nd/uri/rejections", test_rejections);
  g_test_add_func("/nd/uri/legacy-rejected", test_legacy_notify_forms_rejected);
  g_test_add_func("/nd/uri/relay-filter", test_relay_filter);
  return g_test_run();
}
