#include <glib.h>

#include "../src/ui/gnostr-live-state-kinds.h"
#include <nostr-json.h>

static void
test_build_filters_without_user(void)
{
  NostrFilters *filters = gnostr_live_state_build_subscription_filters(NULL);
  g_assert_nonnull(filters);
  g_assert_cmpuint(filters->count, ==, 1);

  g_autofree char *json = nostr_filter_serialize(&filters->filters[0]);
  g_assert_nonnull(json);
  g_assert_nonnull(strstr(json, "\"kinds\":[0,1,5,6,7,16,1111]"));
  g_assert_null(strstr(json, "10000"));
  g_assert_null(strstr(json, "\"authors\""));
  nostr_filters_free(filters);
}

static void
test_build_filters_with_user_state_scope(void)
{
  const char *user = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  NostrFilters *filters = gnostr_live_state_build_subscription_filters(user);
  g_assert_nonnull(filters);
  g_assert_cmpuint(filters->count, ==, 2);

  g_autofree char *timeline = nostr_filter_serialize(&filters->filters[0]);
  g_autofree char *state = nostr_filter_serialize(&filters->filters[1]);
  g_assert_nonnull(timeline);
  g_assert_nonnull(state);

  g_assert_nonnull(strstr(timeline, "\"kinds\":[0,1,5,6,7,16,1111]"));
  g_assert_nonnull(strstr(state, "\"kinds\":[3,10000,10002]"));
  g_assert_nonnull(strstr(state, user));
  nostr_filters_free(filters);
}

static void
test_refresh_kind_requires_current_user(void)
{
  const char *user = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  const char *other = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";

  g_assert_cmpint(gnostr_live_state_refresh_kind_from_event_json(
                    "{\"kind\":3,\"pubkey\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"}",
                    user),
                  ==, GNOSTR_LIVE_STATE_REFRESH_FOLLOW_LIST);
  g_assert_cmpint(gnostr_live_state_refresh_kind_from_event_json(
                    "{\"kind\":10000,\"pubkey\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"}",
                    user),
                  ==, GNOSTR_LIVE_STATE_REFRESH_MUTE_LIST);
  g_assert_cmpint(gnostr_live_state_refresh_kind_from_event_json(
                    "{\"kind\":10002,\"pubkey\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"}",
                    user),
                  ==, GNOSTR_LIVE_STATE_REFRESH_RELAY_LIST);
  g_assert_cmpint(gnostr_live_state_refresh_kind_from_event_json(
                    "{\"kind\":3,\"pubkey\":\"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\"}",
                    user),
                  ==, GNOSTR_LIVE_STATE_REFRESH_NONE);
  g_assert_cmpint(gnostr_live_state_refresh_kind_from_event_json(
                    "{\"kind\":3,\"pubkey\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"}",
                    other),
                  ==, GNOSTR_LIVE_STATE_REFRESH_NONE);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/gnostr/live-state-kinds/build-filters/no-user", test_build_filters_without_user);
  g_test_add_func("/gnostr/live-state-kinds/build-filters/user-scoped", test_build_filters_with_user_state_scope);
  g_test_add_func("/gnostr/live-state-kinds/refresh-kind/current-user-only", test_refresh_kind_requires_current_user);

  return g_test_run();
}
