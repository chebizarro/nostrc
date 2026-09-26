/* Event admission: signature/id validation, kind extraction, pointer match. */
#include "nd-error.h"
#include "nd-event.h"

#include <stdlib.h>
#include <string.h>

#include "nostr-event.h"
#include "nostr-keys.h"
#include "nostr-tag.h"

static char *sk, *pk;

static char *signed_event(int kind, const char *d_tag) {
  NostrEvent *e = nostr_event_new();
  nostr_event_set_pubkey(e, pk);
  nostr_event_set_kind(e, kind);
  nostr_event_set_created_at(e, 1700000000);
  nostr_event_set_content(e, "hello");
  if (d_tag) nostr_event_set_tags(e, nostr_tags_new(1, nostr_tag_new("d", d_tag, NULL)));
  else nostr_event_set_tags(e, nostr_tags_new(0));
  g_assert_cmpint(nostr_event_sign(e, sk), ==, 0);
  char *json = nostr_event_serialize_compact(e);
  nostr_event_free(e);
  char *out = g_strdup(json);
  free(json);
  return out;
}

static void test_valid_and_match(void) {
  g_autofree char *json = signed_event(1, NULL);
  g_autoptr(GError) err = NULL;
  g_autoptr(NdEvent) ev = nd_event_parse(json, -1, &err);
  g_assert_no_error(err);
  g_assert_true(ev->validated);
  g_assert_cmpint(ev->kind, ==, 1);
  g_assert_cmpstr(ev->pubkey_hex, ==, pk);

  NdTarget t = {.entity = ND_ENTITY_EVENT, .id_hex = ev->id_hex, .kind = -1};
  g_assert_true(nd_event_matches_target(ev, &t));
  t.kind = 1;
  g_assert_true(nd_event_matches_target(ev, &t));
  t.kind = 7; /* TLV kind disagrees with the fetched event */
  g_assert_false(nd_event_matches_target(ev, &t));
  t.kind = -1;
  t.id_hex = (char *)"0000000000000000000000000000000000000000000000000000000000000000";
  g_assert_false(nd_event_matches_target(ev, &t));
}

static void test_tampered(void) {
  g_autofree char *json = signed_event(1, NULL);
  /* Re-kind the signed event: a hostile relay steering routing. */
  g_autofree char *tampered = NULL;
  {
    GString *s = g_string_new(json);
    g_string_replace(s, "\"kind\":1,", "\"kind\":7,", 1);
    tampered = g_string_free(s, FALSE);
  }
  g_assert_cmpstr(tampered, !=, json);
  g_autoptr(NdEvent) ev = nd_event_parse(tampered, -1, NULL);
  g_assert_nonnull(ev);
  g_assert_cmpint(ev->kind, ==, 7);
  g_assert_false(ev->validated);
  NdTarget t = {.entity = ND_ENTITY_EVENT, .id_hex = ev->id_hex, .kind = -1};
  g_assert_false(nd_event_matches_target(ev, &t)); /* never admitted */
}

static void test_unsigned_local_file(void) {
  const char *draft = "{\"kind\":30023,\"content\":\"draft\",\"tags\":[[\"d\",\"x\"]]}";
  g_autoptr(NdEvent) ev = nd_event_parse(draft, -1, NULL);
  g_assert_nonnull(ev);
  g_assert_false(ev->validated);
  g_assert_cmpint(ev->kind, ==, 30023);
  g_assert_cmpstr(ev->d_tag, ==, "x");
}

static void test_address_match(void) {
  g_autofree char *json = signed_event(30023, "my-article");
  g_autoptr(NdEvent) ev = nd_event_parse(json, -1, NULL);
  g_assert_true(ev->validated);
  NdTarget t = {.entity = ND_ENTITY_ADDRESS, .pubkey_hex = pk, .kind = 30023,
                .identifier = (char *)"my-article"};
  g_assert_true(nd_event_matches_target(ev, &t));
  t.identifier = (char *)"other";
  g_assert_false(nd_event_matches_target(ev, &t));
}

static void test_bad_input(void) {
  const char *bad[] = {"", "[]", "{}", "{\"kind\":\"1\"}", "{\"kind\":-1}",
                       "{\"kind\":70000}", "not json", NULL};
  for (int i = 0; bad[i]; i++) {
    g_autoptr(GError) err = NULL;
    g_assert_null(nd_event_parse(bad[i], -1, &err));
    g_assert_error(err, ND_ERROR, ND_ERROR_INVALID_EVENT);
  }
  g_autofree char *big = g_malloc(ND_EVENT_MAX_JSON + 2);
  memset(big, ' ', ND_EVENT_MAX_JSON + 1);
  big[ND_EVENT_MAX_JSON + 1] = 0;
  g_autoptr(GError) err = NULL;
  g_assert_null(nd_event_parse(big, -1, &err));
  g_assert_error(err, ND_ERROR, ND_ERROR_INVALID_EVENT);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  sk = nostr_key_generate_private();
  pk = nostr_key_get_public(sk);
  g_test_add_func("/nd/event/valid", test_valid_and_match);
  g_test_add_func("/nd/event/tampered", test_tampered);
  g_test_add_func("/nd/event/unsigned", test_unsigned_local_file);
  g_test_add_func("/nd/event/address", test_address_match);
  g_test_add_func("/nd/event/bad", test_bad_input);
  int rc = g_test_run();
  free(sk);
  free(pk);
  return rc;
}
