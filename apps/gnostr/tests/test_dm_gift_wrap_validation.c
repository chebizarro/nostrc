/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 gnostr contributors
 */

#include <glib.h>

#include "nostr-event.h"
#include "nostr-kinds.h"
#include "util/dm_gift_wrap_validation.h"

static char *
make_event_json(gint kind, const char *content)
{
  NostrEvent *event = nostr_event_new();
  g_assert_nonnull(event);

  nostr_event_set_created_at(event, 1700000000);
  nostr_event_set_kind(event, kind);
  nostr_event_set_content(event, content ? content : "");

  g_assert_cmpint(nostr_event_sign(event,
                                   "1111111111111111111111111111111111111111111111111111111111111111"),
                  ==,
                  0);

  char *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  g_assert_nonnull(json);
  return json;
}

static void
test_valid_gift_wrap_is_accepted(void)
{
  g_autofree gchar *gift_wrap_json = make_event_json(NOSTR_KIND_GIFT_WRAP, "ciphertext");
  g_autofree gchar *reason = NULL;
  NostrEvent *gift_wrap = NULL;

  gboolean ok = gnostr_dm_gift_wrap_parse_for_processing(gift_wrap_json, &gift_wrap, &reason);
  g_assert_true(ok);
  g_assert_nonnull(gift_wrap);
  g_assert_cmpint(nostr_event_get_kind(gift_wrap), ==, NOSTR_KIND_GIFT_WRAP);
  g_assert_null(reason);

  nostr_event_free(gift_wrap);
}

static void
test_wrong_kind_is_rejected(void)
{
  g_autofree gchar *event_json = make_event_json(1, "hello");
  g_autofree gchar *reason = NULL;
  NostrEvent *gift_wrap = NULL;

  gboolean ok = gnostr_dm_gift_wrap_parse_for_processing(event_json, &gift_wrap, &reason);
  g_assert_false(ok);
  g_assert_null(gift_wrap);
  g_assert_cmpstr(reason, ==, "unexpected kind 1");
}

static void
test_malformed_json_is_rejected(void)
{
  g_autofree gchar *reason = NULL;
  NostrEvent *gift_wrap = NULL;

  gboolean ok = gnostr_dm_gift_wrap_parse_for_processing("{\"kind\":1059", &gift_wrap, &reason);
  g_assert_false(ok);
  g_assert_null(gift_wrap);
  g_assert_cmpstr(reason, ==, "deserialize failed");
}

static void
test_invalid_signature_is_rejected(void)
{
  g_autofree gchar *gift_wrap_json = make_event_json(NOSTR_KIND_GIFT_WRAP, "ciphertext");
  g_autofree gchar *reason = NULL;
  NostrEvent *gift_wrap = NULL;

  gchar *sig = g_strstr_len(gift_wrap_json, -1, "\"sig\":\"");
  g_assert_nonnull(sig);
  sig += strlen("\"sig\":\"");
  memset(sig, 'a', 128);

  gboolean ok = gnostr_dm_gift_wrap_parse_for_processing(gift_wrap_json, &gift_wrap, &reason);
  g_assert_false(ok);
  g_assert_null(gift_wrap);
  g_assert_cmpstr(reason, ==, "invalid signature");
}

int
main(int argc, char *argv[])
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/gnostr/dm-gift-wrap-validation/valid", test_valid_gift_wrap_is_accepted);
  g_test_add_func("/gnostr/dm-gift-wrap-validation/wrong-kind", test_wrong_kind_is_rejected);
  g_test_add_func("/gnostr/dm-gift-wrap-validation/malformed", test_malformed_json_is_rejected);
  g_test_add_func("/gnostr/dm-gift-wrap-validation/invalid-signature", test_invalid_signature_is_rejected);

  return g_test_run();
}
