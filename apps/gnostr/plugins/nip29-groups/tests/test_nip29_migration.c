/*
 * test_nip29_migration — NIP-29 migration/fork detection (nostrc-7n4t).
 *
 * Real signed kind:10009 events (NIP-51 simple groups). Checks that only
 * verified lists of trusted authors count, only each author's newest list,
 * that the current relay (in any spelling) is not a relocation, and that
 * relays are ranked by how many trusted authors list them.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "gn-nip29-migration.h"

#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr-tag.h>
#include <string.h>

#define SK_A "7f7ff03d123792d6ac594bfa67bf6d0c0ab55b6b1fdb6249303fe861f1ccba9a"
#define SK_B "3a3e5e1f0b87d1c0e6a4b3f2d8a9c7e6f5d4c3b2a1908f7e6d5c4b3a29181716"
#define SK_C "0f1e2d3c4b5a69788796a5b4c3d2e1f00f1e2d3c4b5a69788796a5b4c3d2e1f0"

/* tags: "id relay" pairs separated by ';' */
static char *
list_event(const char *sk, gint64 created_at, const char *groups)
{
  NostrEvent *ev = nostr_event_new();
  nostr_event_set_kind(ev, 10009);
  nostr_event_set_created_at(ev, created_at);
  nostr_event_set_content(ev, "");
  NostrTags *tags = nostr_tags_new(0);
  g_auto(GStrv) entries = g_strsplit(groups, ";", -1);
  for (gsize i = 0; entries[i]; i++) {
    g_auto(GStrv) f = g_strsplit(entries[i], " ", 2);
    if (!f[0] || !f[1]) continue;
    nostr_tags_append(tags, nostr_tag_new("group", f[0], f[1], NULL));
  }
  nostr_event_set_tags(ev, tags);
  g_assert_cmpint(nostr_event_sign(ev, sk), ==, 0);
  char *json = nostr_event_serialize_compact(ev);
  nostr_event_free(ev);
  return json;
}

static void
test_relocations(void)
{
  g_autofree char *pk_a = nostr_key_get_public(SK_A);
  g_autofree char *pk_b = nostr_key_get_public(SK_B);
  g_autofree char *pk_c = nostr_key_get_public(SK_C);
  const char *trusted[] = { pk_a, pk_b, NULL };   /* C is not an admin */

  GPtrArray *events = g_ptr_array_new_with_free_func(g_free);
  /* A: old list had the group on the old relay, newest on relay-2. */
  g_ptr_array_add(events, list_event(SK_A, 100, "pizza wss://old.example"));
  g_ptr_array_add(events, list_event(SK_A, 200, "pizza wss://relay-2.example;other wss://x.example"));
  /* B: the old relay spelled differently (no relocation) and relay-2, and
   * a third relay. */
  g_ptr_array_add(events, list_event(SK_B, 150,
      "pizza WSS://Old.Example/;pizza wss://relay-2.example/;pizza wss://relay-3.example"));
  /* C (untrusted) claims relay-4. */
  g_ptr_array_add(events, list_event(SK_C, 300, "pizza wss://relay-4.example"));
  /* A forged newer list "by A" (signature does not verify) must not win. */
  char *forged = list_event(SK_A, 999, "pizza wss://evil.example");
  char *p = strstr(forged, "evil");
  g_assert_nonnull(p);
  memcpy(p, "evim", 4);
  g_ptr_array_add(events, forged);

  g_autoptr(GPtrArray) r = gn_nip29_find_relocations("pizza", "wss://old.example",
                                                     trusted, events);
  g_assert_cmpuint(r->len, ==, 2);
  GnNip29Relocation *first = g_ptr_array_index(r, 0);
  GnNip29Relocation *second = g_ptr_array_index(r, 1);
  g_assert_true(gn_nip29_relay_url_equal(first->relay_url, "wss://relay-2.example"));
  g_assert_cmpuint(first->pubkeys->len, ==, 2);
  g_assert_true(gn_nip29_relay_url_equal(second->relay_url, "wss://relay-3.example"));
  g_assert_cmpuint(second->pubkeys->len, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(second->pubkeys, 0), ==, pk_b);

  /* Another group id, or nobody trusted: nothing. */
  g_autoptr(GPtrArray) none = gn_nip29_find_relocations("pasta", "wss://old.example",
                                                        trusted, events);
  g_assert_cmpuint(none->len, ==, 0);
  const char *nobody[] = { NULL };
  g_autoptr(GPtrArray) none2 = gn_nip29_find_relocations("pizza", "wss://old.example",
                                                         nobody, events);
  g_assert_cmpuint(none2->len, ==, 0);
  g_ptr_array_unref(events);
}

static void
test_relay_equal(void)
{
  g_assert_true(gn_nip29_relay_url_equal("wss://Relay.Example/", "wss://relay.example"));
  g_assert_true(gn_nip29_relay_url_equal("wss://r.example/nip29/", "wss://R.example/nip29"));
  g_assert_false(gn_nip29_relay_url_equal("wss://r.example/A", "wss://r.example/a"));
  g_assert_false(gn_nip29_relay_url_equal("wss://a.example", "wss://b.example"));
  g_assert_false(gn_nip29_relay_url_equal(NULL, "wss://b.example"));
  g_assert_false(gn_nip29_relay_url_equal("", ""));
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nip29/migration/relocations", test_relocations);
  g_test_add_func("/nip29/migration/relay-equal", test_relay_equal);
  return g_test_run();
}
