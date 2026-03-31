/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 gnostr contributors
 *
 * test_profile_event_validation.c - Unit tests for strict profile event screening
 */

#include <glib.h>

#include <nostr-event.h>
#include <nostr-gobject-1.0/nostr_json.h>

#include "util/profile_event_validation.h"

static char *
make_valid_profile_event_json(void)
{
  NostrEvent *event = nostr_event_new();
  g_assert_nonnull(event);

  nostr_event_set_created_at(event, 1700000000);
  nostr_event_set_kind(event, 0);
  nostr_event_set_content(event, "{\"name\":\"alice\"}");

  g_assert_cmpint(nostr_event_sign(event,
                                   "1111111111111111111111111111111111111111111111111111111111111111"),
                  ==,
                  0);

  char *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  g_assert_nonnull(json);
  return json;
}

static char *
replace_field_value(const char *json, const char *old_value, const char *new_value)
{
  g_assert_nonnull(json);
  g_assert_nonnull(old_value);
  g_assert_nonnull(new_value);

  gchar *pos = g_strstr_len(json, -1, old_value);
  g_assert_nonnull(pos);

  gsize prefix_len = (gsize)(pos - json);
  return g_strdup_printf("%.*s%s%s",
                         (int)prefix_len, json,
                         new_value,
                         pos + strlen(old_value));
}

static void
test_valid_profile_event_is_accepted(void)
{
  g_autofree gchar *valid_kind0_event = make_valid_profile_event_json();
  g_autofree gchar *expected_pubkey_hex = gnostr_json_get_string(valid_kind0_event, "pubkey", NULL);
  g_autofree gchar *pubkey_hex = NULL;
  g_autofree gchar *content_json = NULL;
  g_autofree gchar *reason = NULL;
  gint64 created_at = 0;

  gboolean ok = gnostr_profile_event_extract_for_apply(valid_kind0_event,
                                                       &pubkey_hex,
                                                       &content_json,
                                                       &created_at,
                                                       &reason);
  g_assert_true(ok);
  g_assert_cmpstr(pubkey_hex, ==, expected_pubkey_hex);
  g_assert_cmpstr(content_json, ==, "{\"name\":\"alice\"}");
  g_assert_cmpint(created_at, ==, 1700000000);
  g_assert_null(reason);
}

static void
test_missing_tags_event_is_rejected(void)
{
  g_autofree gchar *valid_kind0_event = make_valid_profile_event_json();
  g_autofree gchar *missing_tags_event =
    replace_field_value(valid_kind0_event, "\"tags\":[],", "");
  g_autofree gchar *pubkey_hex = NULL;
  g_autofree gchar *content_json = NULL;
  g_autofree gchar *reason = NULL;
  gint64 created_at = 123;

  gboolean ok = gnostr_profile_event_extract_for_apply(missing_tags_event,
                                                       &pubkey_hex,
                                                       &content_json,
                                                       &created_at,
                                                       &reason);
  g_assert_false(ok);
  g_assert_null(pubkey_hex);
  g_assert_null(content_json);
  g_assert_cmpint(created_at, ==, 0);
  g_assert_cmpstr(reason, ==, "missing tags field");
}

static void
test_wrong_kind_event_is_rejected(void)
{
  g_autofree gchar *valid_kind0_event = make_valid_profile_event_json();
  g_autofree gchar *wrong_kind_event =
    replace_field_value(valid_kind0_event, "\"kind\":0", "\"kind\":1");
  g_autofree gchar *pubkey_hex = NULL;
  g_autofree gchar *content_json = NULL;
  g_autofree gchar *reason = NULL;
  gint64 created_at = 0;

  gboolean ok = gnostr_profile_event_extract_for_apply(wrong_kind_event,
                                                       &pubkey_hex,
                                                       &content_json,
                                                       &created_at,
                                                       &reason);
  g_assert_false(ok);
  g_assert_null(pubkey_hex);
  g_assert_null(content_json);
  g_assert_cmpstr(reason, ==, "unexpected kind (expected 0)");
}

static void
test_invalid_canonical_id_is_rejected(void)
{
  g_autofree gchar *valid_kind0_event = make_valid_profile_event_json();
  g_autofree gchar *current_id = gnostr_json_get_string(valid_kind0_event, "id", NULL);
  g_autofree gchar *bad_id_event =
    replace_field_value(valid_kind0_event,
                        current_id,
                        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  g_autofree gchar *pubkey_hex = NULL;
  g_autofree gchar *content_json = NULL;
  g_autofree gchar *reason = NULL;
  gint64 created_at = 0;

  gboolean ok = gnostr_profile_event_extract_for_apply(bad_id_event,
                                                       &pubkey_hex,
                                                       &content_json,
                                                       &created_at,
                                                       &reason);
  g_assert_false(ok);
  g_assert_null(pubkey_hex);
  g_assert_null(content_json);
  g_assert_cmpstr(reason, ==, "canonical id mismatch");
}

static void
test_invalid_signature_is_rejected(void)
{
  g_autofree gchar *valid_kind0_event = make_valid_profile_event_json();
  g_autofree gchar *current_sig = gnostr_json_get_string(valid_kind0_event, "sig", NULL);
  g_autofree gchar *bad_sig_event =
    replace_field_value(valid_kind0_event,
                        current_sig,
                        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  g_autofree gchar *pubkey_hex = NULL;
  g_autofree gchar *content_json = NULL;
  g_autofree gchar *reason = NULL;
  gint64 created_at = 0;

  gboolean ok = gnostr_profile_event_extract_for_apply(bad_sig_event,
                                                       &pubkey_hex,
                                                       &content_json,
                                                       &created_at,
                                                       &reason);
  g_assert_false(ok);
  g_assert_null(pubkey_hex);
  g_assert_null(content_json);
  g_assert_cmpstr(reason, ==, "invalid signature");
}

int
main(int argc, char *argv[])
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/gnostr/profile-event-validation/valid-kind0", test_valid_profile_event_is_accepted);
  g_test_add_func("/gnostr/profile-event-validation/missing-tags", test_missing_tags_event_is_rejected);
  g_test_add_func("/gnostr/profile-event-validation/wrong-kind", test_wrong_kind_event_is_rejected);
  g_test_add_func("/gnostr/profile-event-validation/canonical-id-mismatch", test_invalid_canonical_id_is_rejected);
  g_test_add_func("/gnostr/profile-event-validation/invalid-signature", test_invalid_signature_is_rejected);

  return g_test_run();
}
