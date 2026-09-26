/* Query classification (npub / nprofile / note / nevent / naddr / hex /
 * NIP-05 / free text / secrets) and the REQ filters built for each. */
#include <stdlib.h>
#include <string.h>

#include "nostr/nip19/nip19.h"
#include "nsp-query.h"

static const char *PK = "3bf0c63fcb93463407af97a5e5ee64fa883d107ef9e558472c4eb9aaaefa459d";
static const char *ID = "d1b3f0c1e5a2d4f60718293a4b5c6d7e8f9011223344556677889900aabbccdd";

static void hex32(const char *hex, guint8 out[32]) {
  for (int i = 0; i < 32; i++) out[i] = (guint8)strtoul((char[]){hex[2 * i], hex[2 * i + 1], 0}, NULL, 16);
}

static char *npub(void) {
  guint8 b[32];
  char *s = NULL;
  hex32(PK, b);
  g_assert_cmpint(nostr_nip19_encode_npub(b, &s), ==, 0);
  char *r = g_strdup(s);
  free(s);
  return r;
}

static NspQuery *classify1(const char *term) {
  const char *terms[] = {term, NULL};
  return nsp_query_classify(terms);
}

static char *filters_of(NspQuery *q) {
  g_autoptr(JsonNode) n = nsp_query_filters(q);
  return nsp_json_to_string(n);
}

static void test_npub_forms(void) {
  g_autofree char *np = npub();
  g_autofree char *upper = g_ascii_strup(np, -1);
  g_autofree char *withscheme = g_strconcat("nostr:", np, NULL);
  g_autofree char *web = g_strconcat("web+nostr:", np, NULL);
  const char *forms[] = {np, upper, withscheme, web};
  g_autofree char *want = g_strdup_printf("[{\"kinds\":[0],\"authors\":[\"%s\"],\"limit\":1}]", PK);
  for (guint i = 0; i < G_N_ELEMENTS(forms); i++) {
    g_autoptr(NspQuery) q = classify1(forms[i]);
    g_assert_cmpint(q->type, ==, NSP_QUERY_PROFILE);
    g_assert_cmpstr(q->target->pubkey_hex, ==, PK);
    g_autofree char *f = filters_of(q);
    g_assert_cmpstr(f, ==, want);
    g_assert_true(nsp_query_is_identifier(q));
  }
  /* surrounding whitespace from the overview entry */
  g_autofree char *padded = g_strdup_printf("  %s \n", np);
  g_autoptr(NspQuery) q = classify1(padded);
  g_assert_cmpint(q->type, ==, NSP_QUERY_PROFILE);
}

static void test_nprofile(void) {
  char *relays[] = {"wss://relay.example.com", NULL};
  NostrProfilePointer p = {.public_key = (char *)PK, .relays = relays, .relays_count = 1};
  char *b = NULL;
  g_assert_cmpint(nostr_nip19_encode_nprofile(&p, &b), ==, 0);
  g_autoptr(NspQuery) q = classify1(b);
  free(b);
  g_assert_cmpint(q->type, ==, NSP_QUERY_PROFILE);
  g_assert_cmpstr(q->target->pubkey_hex, ==, PK);
}

static void test_note_nevent(void) {
  guint8 id[32];
  hex32(ID, id);
  char *note = NULL;
  g_assert_cmpint(nostr_nip19_encode_note(id, &note), ==, 0);
  g_autoptr(NspQuery) q = classify1(note);
  free(note);
  g_assert_cmpint(q->type, ==, NSP_QUERY_EVENT);
  g_autofree char *f = filters_of(q);
  g_autofree char *want = g_strdup_printf("[{\"ids\":[\"%s\"],\"limit\":1}]", ID);
  g_assert_cmpstr(f, ==, want);

  NdTarget t = {.entity = ND_ENTITY_EVENT, .id_hex = (char *)ID, .pubkey_hex = (char *)PK, .kind = 1};
  g_autofree char *nevent = nd_target_to_uri(&t);
  g_autoptr(NspQuery) q2 = classify1(nevent);
  g_assert_cmpint(q2->type, ==, NSP_QUERY_EVENT);
  g_autofree char *f2 = filters_of(q2);
  g_assert_cmpstr(f2, ==, want);
}

static void test_naddr(void) {
  NdTarget t = {.entity = ND_ENTITY_ADDRESS, .pubkey_hex = (char *)PK, .kind = 30023,
                .identifier = (char *)"my-article"};
  g_autofree char *naddr = nd_target_to_uri(&t);
  g_autoptr(NspQuery) q = classify1(naddr);
  g_assert_cmpint(q->type, ==, NSP_QUERY_ADDRESS);
  g_autofree char *f = filters_of(q);
  g_autofree char *want = g_strdup_printf(
      "[{\"kinds\":[30023],\"authors\":[\"%s\"],\"#d\":[\"my-article\"],\"limit\":1}]", PK);
  g_assert_cmpstr(f, ==, want);
}

static void test_hex(void) {
  g_autofree char *upper = g_ascii_strup(ID, -1);
  g_autoptr(NspQuery) q = classify1(upper);
  g_assert_cmpint(q->type, ==, NSP_QUERY_HEX);
  g_assert_cmpstr(q->hex, ==, ID);
  g_autofree char *f = filters_of(q);
  g_autofree char *want = g_strdup_printf(
      "[{\"ids\":[\"%s\"],\"limit\":1},{\"kinds\":[0],\"authors\":[\"%s\"],\"limit\":1}]", ID, ID);
  g_assert_cmpstr(f, ==, want);
}

static void test_nip05(void) {
  g_autoptr(NspQuery) q = classify1("Alice@Nos.Social");
  g_assert_cmpint(q->type, ==, NSP_QUERY_NIP05);
  g_assert_cmpstr(q->nip05, ==, "alice@nos.social");
  g_assert_cmpstr(q->nip05_local, ==, "alice");
  g_assert_cmpstr(q->nip05_domain, ==, "nos.social");
  g_autofree char *f = filters_of(q);
  g_assert_cmpstr(f, ==, "[{\"kinds\":[0],\"search\":\"alice@nos.social\",\"limit\":20}]");

  g_autoptr(NspQuery) root = classify1("_@nostr.com");
  g_assert_cmpint(root->type, ==, NSP_QUERY_NIP05);
  g_assert_cmpstr(root->nip05, ==, "_@nostr.com");

  /* Not publicly resolvable: never NIP-05 (no network probe of private
   * names or half-typed domains); they fall back to free text. */
  const char *text_like[] = {"alice@nos",        "alice@localhost",  "bob@192.168.1.10",
                             "bob@printer.local", "x@corp.internal", "x@router.lan",
                             "x@box.home.arpa",  "x@abc.onion",      "alice@nos.s"};
  for (guint i = 0; i < G_N_ELEMENTS(text_like); i++) {
    g_autoptr(NspQuery) t = classify1(text_like[i]);
    g_assert_cmpint(t->type, ==, NSP_QUERY_TEXT);
  }
}

static void test_secrets_and_partials(void) {
  const char *none[] = {
      "nsec1vl029mgpspedva04g90vltkh6fvh240zqtv9k0t9af8935ke9laqsnlfe5",
      "nostr:nsec1vl029mgpspedva04g90vltkh6fvh240zqtv9k0t9af8935ke9laqsnlfe5",
      "ncryptsec1qgg9947rlpvqu76pj5ecreduf9jxhselq2nae2kghhvd5g7dgjtcxfqtd67p9m0w57lspw8gsq6yphnm8623nsl8xn9j4jdzz84zm3frztj3z7s35vpzmqf6ksu8r89qk5z2zxfmu5gv8th8wclt0h4p",
      "NSEC1BOGUS-not-bech32",            /* malformed secret: still refused */
      "my key is nsec1vl029mgpspedva04g", /* secret inside free text */
      "npub1qqqx",     /* partially typed */
      "note1",         /* hrp only */
      "nostr:",        /* scheme only */
      "ab",            /* too short for text */
      "",
  };
  for (guint i = 0; i < G_N_ELEMENTS(none); i++) {
    g_autoptr(NspQuery) q = classify1(none[i]);
    if (q->type != NSP_QUERY_NONE) g_error("'%s' classified as %s", none[i], nsp_query_type_name(q->type));
    g_assert_null(nsp_query_filters(q));
  }
  g_autoptr(NspQuery) q = nsp_query_classify(NULL);
  g_assert_cmpint(q->type, ==, NSP_QUERY_NONE);
}

static void test_text(void) {
  const char *terms[] = {"Hello", "WORLD", "hello", NULL};
  g_autoptr(NspQuery) q = nsp_query_classify(terms);
  g_assert_cmpint(q->type, ==, NSP_QUERY_TEXT);
  g_assert_false(nsp_query_is_identifier(q));
  g_assert_cmpstr(q->text, ==, "Hello WORLD hello");
  g_assert_cmpuint(g_strv_length(q->words), ==, 2); /* folded + de-duplicated */
  g_assert_cmpstr(q->words[0], ==, "hello");
  g_assert_cmpstr(q->words[1], ==, "world");
  g_autofree char *f = filters_of(q);
  g_assert_cmpstr(f, ==, "[{\"kinds\":[0,1,30023],\"search\":\"Hello WORLD hello\",\"limit\":40}]");

  g_autoptr(NspQuery) cjk = classify1("日本語");
  g_assert_cmpint(cjk->type, ==, NSP_QUERY_TEXT);

  /* controls / bidi overrides never reach the relay */
  g_autoptr(NspQuery) dirty = classify1("he\xe2\x80\xaell\x07o");
  g_assert_cmpint(dirty->type, ==, NSP_QUERY_TEXT);
  g_assert_cmpstr(dirty->text, ==, "hell o"); /* bidi dropped, BEL -> space */

  /* long input: clamped on a UTF-8 boundary, words capped */
  g_autoptr(GString) big = g_string_new(NULL);
  for (int i = 0; i < 200; i++) g_string_append(big, "é w");
  g_autoptr(NspQuery) lq = classify1(big->str);
  g_assert_cmpint(lq->type, ==, NSP_QUERY_TEXT);
  g_assert_cmpuint(strlen(lq->text), <=, NSP_TEXT_MAX_BYTES);
  g_assert_true(g_utf8_validate(lq->text, -1, NULL));
  g_autoptr(GString) many = g_string_new(NULL);
  for (int i = 0; i < 20; i++) g_string_append_printf(many, "w%d ", i);
  g_autoptr(NspQuery) mq = classify1(many->str);
  g_assert_cmpuint(g_strv_length(mq->words), ==, NSP_TEXT_MAX_WORDS);
}

static void test_other_filters(void) {
  g_autoptr(JsonNode) scan = nsp_filters_text_scan();
  g_autofree char *s = nsp_json_to_string(scan);
  g_assert_cmpstr(s, ==, "[{\"kinds\":[0],\"limit\":250},{\"kinds\":[1,30023],\"limit\":250}]");
  const char *pks[] = {PK, ID, NULL};
  g_autoptr(JsonNode) prof = nsp_filters_profiles(pks);
  g_autofree char *p = nsp_json_to_string(prof);
  g_autofree char *want = g_strdup_printf("[{\"kinds\":[0],\"authors\":[\"%s\",\"%s\"],\"limit\":2}]", PK, ID);
  g_assert_cmpstr(p, ==, want);
  g_assert_null(nsp_filters_profiles(NULL));
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nsp/query/npub-forms", test_npub_forms);
  g_test_add_func("/nsp/query/nprofile", test_nprofile);
  g_test_add_func("/nsp/query/note-nevent", test_note_nevent);
  g_test_add_func("/nsp/query/naddr", test_naddr);
  g_test_add_func("/nsp/query/hex", test_hex);
  g_test_add_func("/nsp/query/nip05", test_nip05);
  g_test_add_func("/nsp/query/secrets-partials", test_secrets_and_partials);
  g_test_add_func("/nsp/query/text", test_text);
  g_test_add_func("/nsp/query/other-filters", test_other_filters);
  return g_test_run();
}
