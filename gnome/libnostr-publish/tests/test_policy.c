/* test_policy.c - libnostr-publish policy, OK classification, NIP-65
 *
 * SPDX-License-Identifier: MIT
 */

#include <nostr-publish/nostr-publish.h>

#include <glib.h>

static void
test_zero_policy_is_default(void)
{
  NostrPublishPolicy zero = {0};
  NostrPublishPolicy init;
  nostr_publish_policy_init(&init);

  g_assert_cmpint(nostr_publish_policy_backoff_delay(&zero, 0), ==, 60);
  g_assert_cmpint(nostr_publish_policy_backoff_delay(&init, 0), ==, 60);
  g_assert_cmpint(nostr_publish_policy_backoff_delay(NULL, 0), ==, 60);
  /* quorum 0 = every relay */
  g_assert_cmpuint(nostr_publish_policy_required_acks(&zero, 3), ==, 3);
  g_assert_cmpuint(nostr_publish_policy_required_acks(NULL, 3), ==, 3);
}

static void
test_backoff_curve(void)
{
  NostrPublishPolicy p = {0};
  /* Identical to nostr-dav's historic curve: 60 s doubling, 60 min cap. */
  g_assert_cmpint(nostr_publish_policy_backoff_delay(&p, 0), ==, 60);
  g_assert_cmpint(nostr_publish_policy_backoff_delay(&p, 1), ==, 120);
  g_assert_cmpint(nostr_publish_policy_backoff_delay(&p, 5), ==, 1920);
  g_assert_cmpint(nostr_publish_policy_backoff_delay(&p, 6), ==, 3600);
  g_assert_cmpint(nostr_publish_policy_backoff_delay(&p, 1000), ==, 3600);

  p.backoff_initial_sec = 5;
  p.backoff_max_sec     = 30;
  g_assert_cmpint(nostr_publish_policy_backoff_delay(&p, 0), ==, 5);
  g_assert_cmpint(nostr_publish_policy_backoff_delay(&p, 2), ==, 20);
  g_assert_cmpint(nostr_publish_policy_backoff_delay(&p, 3), ==, 30);
}

static void
test_required_acks_clamp(void)
{
  NostrPublishPolicy p = {0};
  p.quorum = 2;
  g_assert_cmpuint(nostr_publish_policy_required_acks(&p, 3), ==, 2);
  p.quorum = 5;
  g_assert_cmpuint(nostr_publish_policy_required_acks(&p, 3), ==, 3);
  g_assert_cmpuint(nostr_publish_policy_required_acks(&p, 0), ==, 1);
}

static void
test_classify_ok(void)
{
  g_assert_cmpint(nostr_publish_classify_ok(TRUE, NULL), ==, NOSTR_PUBLISH_OK_ACCEPT);
  g_assert_cmpint(nostr_publish_classify_ok(TRUE, "invalid: x"), ==, NOSTR_PUBLISH_OK_ACCEPT);
  g_assert_cmpint(nostr_publish_classify_ok(FALSE, "duplicate: have it"), ==, NOSTR_PUBLISH_OK_ACCEPT);
  g_assert_cmpint(nostr_publish_classify_ok(FALSE, NULL), ==, NOSTR_PUBLISH_OK_TRANSIENT);
  g_assert_cmpint(nostr_publish_classify_ok(FALSE, "error: busy"), ==, NOSTR_PUBLISH_OK_TRANSIENT);
  g_assert_cmpint(nostr_publish_classify_ok(FALSE, "rate-limited: slow"), ==, NOSTR_PUBLISH_OK_TRANSIENT);
  static const gchar *const permanent[] = {
    "invalid: bad sig", "blocked: no", "banned: you",
    "restricted: paid only", "auth-required: AUTH first",
  };
  for (gsize i = 0; i < G_N_ELEMENTS(permanent); i++)
    g_assert_cmpint(nostr_publish_classify_ok(FALSE, permanent[i]), ==,
                    NOSTR_PUBLISH_OK_PERMANENT);
}

#define RELAY_LIST \
  "{\"kind\":10002,\"content\":\"\",\"created_at\":1,\"tags\":[" \
  "[\"r\",\"wss://both.example\"]," \
  "[\"r\",\"wss://write.example\",\"write\"]," \
  "[\"r\",\"wss://read.example\",\"read\"]," \
  "[\"r\",\"wss://both.example\"]," \
  "[\"r\",\"wss://both.example/\"]," \
  "[\"r\",\"https://not-a-relay.example\"]," \
  "[\"r\",\"wss://weird.example\",\"sideways\"]," \
  "[\"p\",\"wss://ptag.example\"]," \
  "[\"r\"]," \
  "[\"r\",42]" \
  "]}"

static void
test_nip65_write(void)
{
  GError *err = NULL;
  g_auto(GStrv) w = nostr_publish_nip65_relays(RELAY_LIST,
                                               NOSTR_PUBLISH_NIP65_WRITE, &err);
  g_assert_no_error(err);
  /* Tag order kept; exact dup dropped; trailing slash NOT normalised. */
  const gchar *want[] = { "wss://both.example", "wss://write.example",
                          "wss://both.example/", NULL };
  g_assert_cmpstrv(w, want);
}

static void
test_nip65_read(void)
{
  g_auto(GStrv) r = nostr_publish_nip65_relays(RELAY_LIST,
                                               NOSTR_PUBLISH_NIP65_READ, NULL);
  const gchar *want[] = { "wss://both.example", "wss://read.example",
                          "wss://both.example/", NULL };
  g_assert_cmpstrv(r, want);
}

static void
test_nip65_rejects_non_relay_list(void)
{
  static const gchar *const bad[] = {
    "not json",
    "[]",
    "{\"kind\":1,\"tags\":[]}",
    "{\"kind\":\"10002\",\"tags\":[]}",
    "{\"kind\":10002}",
  };
  for (gsize i = 0; i < G_N_ELEMENTS(bad); i++) {
    GError *err = NULL;
    GStrv out = nostr_publish_nip65_relays(bad[i], NOSTR_PUBLISH_NIP65_WRITE,
                                           &err);
    g_assert_null(out);
    g_assert_error(err, NOSTR_PUBLISH_ERROR,
                   NOSTR_PUBLISH_ERROR_INVALID_RELAY_LIST);
    g_clear_error(&err);
  }

  /* A valid list with no matching tags yields an empty, non-NULL array. */
  g_auto(GStrv) empty = nostr_publish_nip65_relays(
      "{\"kind\":10002,\"tags\":[]}", NOSTR_PUBLISH_NIP65_WRITE, NULL);
  g_assert_nonnull(empty);
  g_assert_null(empty[0]);
}

static void
test_select_targets(void)
{
  const gchar *write[] = { "wss://a", "wss://b", "wss://a", NULL };
  NostrPublishPolicy p = {0};
  GError *err = NULL;

  /* default: session relay preferred when known, else write relays */
  g_auto(GStrv) t1 = nostr_publish_policy_select_targets(&p, write, NULL, &err);
  g_assert_no_error(err);
  const gchar *want_write[] = { "wss://a", "wss://b", NULL };
  g_assert_cmpstrv(t1, want_write);

  g_auto(GStrv) t2 = nostr_publish_policy_select_targets(&p, write,
                                                         "ws://session", &err);
  g_assert_no_error(err);
  const gchar *want_session[] = { "ws://session", NULL };
  g_assert_cmpstrv(t2, want_session);

  p.upstream = NOSTR_PUBLISH_UPSTREAM_DIRECT_ONLY;
  g_auto(GStrv) t3 = nostr_publish_policy_select_targets(&p, write,
                                                         "ws://session", &err);
  g_assert_no_error(err);
  g_assert_cmpstrv(t3, want_write);

  p.upstream = NOSTR_PUBLISH_UPSTREAM_SESSION_RELAY_ONLY;
  GStrv t4 = nostr_publish_policy_select_targets(&p, write, NULL, &err);
  g_assert_null(t4);
  g_assert_error(err, NOSTR_PUBLISH_ERROR, NOSTR_PUBLISH_ERROR_NO_RELAYS);
  g_clear_error(&err);

  p.upstream = NOSTR_PUBLISH_UPSTREAM_DIRECT_ONLY;
  GStrv t5 = nostr_publish_policy_select_targets(&p, NULL, NULL, &err);
  g_assert_null(t5);
  g_assert_error(err, NOSTR_PUBLISH_ERROR, NOSTR_PUBLISH_ERROR_NO_RELAYS);
  g_clear_error(&err);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nostr-publish/policy/zero-is-default", test_zero_policy_is_default);
  g_test_add_func("/nostr-publish/policy/backoff", test_backoff_curve);
  g_test_add_func("/nostr-publish/policy/required-acks", test_required_acks_clamp);
  g_test_add_func("/nostr-publish/policy/classify-ok", test_classify_ok);
  g_test_add_func("/nostr-publish/policy/select-targets", test_select_targets);
  g_test_add_func("/nostr-publish/nip65/write", test_nip65_write);
  g_test_add_func("/nostr-publish/nip65/read", test_nip65_read);
  g_test_add_func("/nostr-publish/nip65/rejects", test_nip65_rejects_non_relay_list);
  return g_test_run();
}
