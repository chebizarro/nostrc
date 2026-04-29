/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 gnostr contributors
 *
 * test_startup_live_eose.c - Unit tests for startup live EOSE tracking
 */

#include <glib.h>

#include "ui/gnostr-startup-live-eose.h"

static void
test_startup_live_eose_tracks_first_and_all(void)
{
  g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  guint flags = gnostr_main_window_track_startup_live_eose_internal(seen, 3, "wss://relay-one.example");
  g_assert_cmpuint(flags, ==, GNOSTR_STARTUP_LIVE_EOSE_FLAG_FIRST);
  g_assert_cmpuint(g_hash_table_size(seen), ==, 1);

  flags = gnostr_main_window_track_startup_live_eose_internal(seen, 3, "wss://relay-two.example");
  g_assert_cmpuint(flags, ==, GNOSTR_STARTUP_LIVE_EOSE_FLAG_NONE);
  g_assert_cmpuint(g_hash_table_size(seen), ==, 2);

  flags = gnostr_main_window_track_startup_live_eose_internal(seen, 3, "wss://relay-three.example");
  g_assert_cmpuint(flags, ==, GNOSTR_STARTUP_LIVE_EOSE_FLAG_ALL);
  g_assert_cmpuint(g_hash_table_size(seen), ==, 3);
}

static void
test_startup_live_eose_ignores_duplicates(void)
{
  g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  guint flags = gnostr_main_window_track_startup_live_eose_internal(seen, 2, "wss://relay-one.example");
  g_assert_cmpuint(flags, ==, GNOSTR_STARTUP_LIVE_EOSE_FLAG_FIRST);
  g_assert_cmpuint(g_hash_table_size(seen), ==, 1);

  flags = gnostr_main_window_track_startup_live_eose_internal(seen, 2, "wss://relay-one.example");
  g_assert_cmpuint(flags, ==, GNOSTR_STARTUP_LIVE_EOSE_FLAG_NONE);
  g_assert_cmpuint(g_hash_table_size(seen), ==, 1);

  flags = gnostr_main_window_track_startup_live_eose_internal(seen, 2, "wss://relay-two.example");
  g_assert_cmpuint(flags, ==, GNOSTR_STARTUP_LIVE_EOSE_FLAG_ALL);
  g_assert_cmpuint(g_hash_table_size(seen), ==, 2);
}

static void
test_startup_live_eose_single_relay_sets_first_and_all(void)
{
  g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  guint flags = gnostr_main_window_track_startup_live_eose_internal(seen, 1, "wss://relay-only.example");
  g_assert_cmpuint(flags, ==, GNOSTR_STARTUP_LIVE_EOSE_FLAG_FIRST | GNOSTR_STARTUP_LIVE_EOSE_FLAG_ALL);
  g_assert_cmpuint(g_hash_table_size(seen), ==, 1);
}

/* F36 error-path test: Relay never sends EOSE */
static void
test_startup_live_eose_incomplete_never_sets_all(void)
{
  g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  /* Configure for 3 relays total */
  guint flags = gnostr_main_window_track_startup_live_eose_internal(seen, 3, "wss://relay-one.example");
  g_assert_cmpuint(flags, ==, GNOSTR_STARTUP_LIVE_EOSE_FLAG_FIRST);
  g_assert_cmpuint(g_hash_table_size(seen), ==, 1);

  flags = gnostr_main_window_track_startup_live_eose_internal(seen, 3, "wss://relay-two.example");
  g_assert_cmpuint(flags, ==, GNOSTR_STARTUP_LIVE_EOSE_FLAG_NONE);
  g_assert_cmpuint(g_hash_table_size(seen), ==, 2);

  /* Third relay never sends EOSE - verify ALL flag is never set */
  g_assert_cmpuint(flags & GNOSTR_STARTUP_LIVE_EOSE_FLAG_ALL, ==, 0);
}

int
main(int argc, char *argv[])
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/gnostr/startup-live-eose/first-and-all", test_startup_live_eose_tracks_first_and_all);
  g_test_add_func("/gnostr/startup-live-eose/duplicate-relay", test_startup_live_eose_ignores_duplicates);
  g_test_add_func("/gnostr/startup-live-eose/single-relay", test_startup_live_eose_single_relay_sets_first_and_all);
  g_test_add_func("/gnostr/startup-live-eose/incomplete-eose", test_startup_live_eose_incomplete_never_sets_all);

  return g_test_run();
}
