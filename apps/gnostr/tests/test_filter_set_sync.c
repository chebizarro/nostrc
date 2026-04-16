/* test_filter_set_sync.c — Unit tests for GnostrFilterSetSync.
 *
 * SPDX-License-Identifier: MIT
 *
 * The sync module talks to the process-wide default filter-set manager
 * and (in production builds) to the relay-backed app-data manager.
 * For unit testing we compile with GNOSTR_FILTER_SET_SYNC_TEST_ONLY so
 * the relay paths are stubbed out and we can focus on the pure
 * serialize/apply round-trip plus the merge semantics.
 *
 * nostrc-yg8j.9
 */

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <string.h>

#include "model/gnostr-filter-set.h"
#include "model/gnostr-filter-set-manager.h"
#include "model/gnostr-filter-set-sync.h"

/* Test-only probe exported by gnostr-filter-set-sync.c when built with
 * GNOSTR_FILTER_SET_SYNC_TEST_ONLY. Lets us assert the applying_remote
 * guard suppresses the push echo that would otherwise follow every
 * items-changed fired during apply(). */
gboolean gnostr_filter_set_sync_debug_has_pending_push(void);

/* ------------------------------------------------------------------------
 * Helpers: the sync module operates on the default singleton manager.
 * We reset it between tests by removing all custom entries (predefined
 * ones can't be removed, but their ids never collide with test ids).
 * ------------------------------------------------------------------------ */

static void
reset_default_manager(void)
{
  GnostrFilterSetManager *mgr = gnostr_filter_set_manager_get_default();
  GListModel *model = gnostr_filter_set_manager_get_model(mgr);

  /* Walk forwards, collect ids, then remove — we can't remove-while-
   * iterating safely. */
  g_autoptr(GPtrArray) to_remove = g_ptr_array_new_with_free_func(g_free);
  guint n = g_list_model_get_n_items(model);
  for (guint i = 0; i < n; i++) {
    GnostrFilterSet *fs = g_list_model_get_item(model, i);
    if (!fs) continue;
    if (gnostr_filter_set_get_source(fs) == GNOSTR_FILTER_SET_SOURCE_CUSTOM) {
      const gchar *id = gnostr_filter_set_get_id(fs);
      if (id && *id)
        g_ptr_array_add(to_remove, g_strdup(id));
    }
    g_object_unref(fs);
  }

  for (guint i = 0; i < to_remove->len; i++)
    gnostr_filter_set_manager_remove(mgr, g_ptr_array_index(to_remove, i));
}

static GnostrFilterSet *
make_custom(const gchar *id, const gchar *name)
{
  GnostrFilterSet *fs = gnostr_filter_set_new_with_name(name);
  gnostr_filter_set_set_id(fs, id);
  gnostr_filter_set_set_source(fs, GNOSTR_FILTER_SET_SOURCE_CUSTOM);
  return fs;
}

/* ------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------ */

static void
test_serialize_empty_no_custom(void)
{
  reset_default_manager();

  g_autofree gchar *json = gnostr_filter_set_sync_serialize();
  g_assert_nonnull(json);

  g_autoptr(JsonParser) parser = json_parser_new();
  g_assert_true(json_parser_load_from_data(parser, json, -1, NULL));
  JsonObject *obj = json_node_get_object(json_parser_get_root(parser));
  g_assert_nonnull(obj);
  g_assert_cmpint(json_object_get_int_member(obj, "version"),
                  ==, GNOSTR_FILTER_SET_SYNC_FORMAT_VERSION);

  JsonArray *arr = json_object_get_array_member(obj, "filter_sets");
  g_assert_nonnull(arr);
  g_assert_cmpint(json_array_get_length(arr), ==, 0);
}

static void
test_serialize_skips_predefined(void)
{
  reset_default_manager();

  GnostrFilterSetManager *mgr = gnostr_filter_set_manager_get_default();
  gnostr_filter_set_manager_install_defaults(mgr);

  /* Add a single custom set alongside the predefined ones. */
  g_autoptr(GnostrFilterSet) fs = make_custom("sync-a", "Sync A");
  g_assert_true(gnostr_filter_set_manager_add(mgr, fs));

  g_autofree gchar *json = gnostr_filter_set_sync_serialize();
  g_assert_nonnull(json);

  g_autoptr(JsonParser) parser = json_parser_new();
  g_assert_true(json_parser_load_from_data(parser, json, -1, NULL));
  JsonObject *obj = json_node_get_object(json_parser_get_root(parser));
  JsonArray *arr = json_object_get_array_member(obj, "filter_sets");

  /* Only the custom set should be present. */
  g_assert_cmpint(json_array_get_length(arr), ==, 1);
  JsonObject *entry = json_array_get_object_element(arr, 0);
  g_assert_cmpstr(json_object_get_string_member(entry, "id"), ==, "sync-a");

  reset_default_manager();
}

static void
test_apply_into_empty_manager(void)
{
  reset_default_manager();

  const gchar *payload =
    "{\"version\":1,\"filter_sets\":["
      "{\"id\":\"remote-1\",\"name\":\"Remote One\",\"source\":\"custom\"},"
      "{\"id\":\"remote-2\",\"name\":\"Remote Two\",\"source\":\"custom\"}"
    "]}";

  g_autoptr(GError) err = NULL;
  g_assert_true(gnostr_filter_set_sync_apply(payload, &err));
  g_assert_no_error(err);

  GnostrFilterSetManager *mgr = gnostr_filter_set_manager_get_default();
  g_assert_true(gnostr_filter_set_manager_contains(mgr, "remote-1"));
  g_assert_true(gnostr_filter_set_manager_contains(mgr, "remote-2"));

  GnostrFilterSet *a = gnostr_filter_set_manager_get(mgr, "remote-1");
  g_assert_cmpstr(gnostr_filter_set_get_name(a), ==, "Remote One");
  /* Sync layer must force all remote sets to CUSTOM. */
  g_assert_cmpint(gnostr_filter_set_get_source(a),
                  ==, GNOSTR_FILTER_SET_SOURCE_CUSTOM);

  reset_default_manager();
}

static void
test_apply_updates_existing_id(void)
{
  reset_default_manager();

  GnostrFilterSetManager *mgr = gnostr_filter_set_manager_get_default();

  /* Seed a local entry that will be overwritten by the remote one. */
  g_autoptr(GnostrFilterSet) seed = make_custom("merge-1", "Local Original");
  g_assert_true(gnostr_filter_set_manager_add(mgr, seed));

  const gchar *payload =
    "{\"version\":1,\"filter_sets\":["
      "{\"id\":\"merge-1\",\"name\":\"Remote Replaces\",\"source\":\"custom\"}"
    "]}";

  g_autoptr(GError) err = NULL;
  g_assert_true(gnostr_filter_set_sync_apply(payload, &err));

  GnostrFilterSet *after = gnostr_filter_set_manager_get(mgr, "merge-1");
  g_assert_nonnull(after);
  g_assert_cmpstr(gnostr_filter_set_get_name(after), ==, "Remote Replaces");

  reset_default_manager();
}

static void
test_apply_preserves_local_only(void)
{
  reset_default_manager();

  GnostrFilterSetManager *mgr = gnostr_filter_set_manager_get_default();

  g_autoptr(GnostrFilterSet) local_only = make_custom("local-only", "Local Only");
  g_assert_true(gnostr_filter_set_manager_add(mgr, local_only));

  /* Remote payload only mentions remote-x — local-only must survive. */
  const gchar *payload =
    "{\"version\":1,\"filter_sets\":["
      "{\"id\":\"remote-x\",\"name\":\"Remote X\",\"source\":\"custom\"}"
    "]}";

  g_autoptr(GError) err = NULL;
  g_assert_true(gnostr_filter_set_sync_apply(payload, &err));

  g_assert_true(gnostr_filter_set_manager_contains(mgr, "local-only"));
  g_assert_true(gnostr_filter_set_manager_contains(mgr, "remote-x"));

  reset_default_manager();
}

static void
test_apply_forces_source_to_custom(void)
{
  reset_default_manager();

  /* Payload claims PREDEFINED — sync layer must override to CUSTOM so
   * remote data never colonises the predefined slot reserved for
   * built-ins. */
  const gchar *payload =
    "{\"version\":1,\"filter_sets\":["
      "{\"id\":\"sneaky-id\",\"name\":\"Fake\",\"source\":\"predefined\"}"
    "]}";

  g_autoptr(GError) err = NULL;
  g_assert_true(gnostr_filter_set_sync_apply(payload, &err));

  GnostrFilterSetManager *mgr = gnostr_filter_set_manager_get_default();
  GnostrFilterSet *got = gnostr_filter_set_manager_get(mgr, "sneaky-id");
  g_assert_nonnull(got);
  g_assert_cmpint(gnostr_filter_set_get_source(got),
                  ==, GNOSTR_FILTER_SET_SOURCE_CUSTOM);

  reset_default_manager();
}

static void
test_apply_empty_payload_is_noop(void)
{
  reset_default_manager();

  GnostrFilterSetManager *mgr = gnostr_filter_set_manager_get_default();
  g_autoptr(GnostrFilterSet) seed = make_custom("keep-me", "Keep Me");
  g_assert_true(gnostr_filter_set_manager_add(mgr, seed));

  /* Missing filter_sets field — treated as "remote has nothing" and
   * must leave local state untouched. */
  g_autoptr(GError) err = NULL;
  g_assert_true(gnostr_filter_set_sync_apply("{\"version\":1}", &err));
  g_assert_true(gnostr_filter_set_manager_contains(mgr, "keep-me"));

  /* Empty array likewise. */
  g_assert_true(gnostr_filter_set_sync_apply(
      "{\"version\":1,\"filter_sets\":[]}", &err));
  g_assert_true(gnostr_filter_set_manager_contains(mgr, "keep-me"));

  reset_default_manager();
}

static void
test_apply_rejects_invalid_json(void)
{
  reset_default_manager();

  g_autoptr(GError) err = NULL;
  g_assert_false(gnostr_filter_set_sync_apply("not-json", &err));
  g_assert_nonnull(err);
}

static void
test_apply_rejects_non_object_root(void)
{
  reset_default_manager();

  g_autoptr(GError) err = NULL;
  g_assert_false(gnostr_filter_set_sync_apply("[]", &err));
  g_assert_nonnull(err);
}

static void
test_apply_rejects_bad_filter_sets_type(void)
{
  reset_default_manager();

  g_autoptr(GError) err = NULL;
  g_assert_false(gnostr_filter_set_sync_apply(
      "{\"version\":1,\"filter_sets\":\"nope\"}", &err));
  g_assert_nonnull(err);
}

static void
test_round_trip(void)
{
  reset_default_manager();

  GnostrFilterSetManager *mgr = gnostr_filter_set_manager_get_default();

  /* Seed a couple of custom sets with distinguishing content. */
  g_autoptr(GnostrFilterSet) a = make_custom("rt-a", "Round Trip A");
  gint kinds_a[] = {1, 6};
  gnostr_filter_set_set_kinds(a, kinds_a, G_N_ELEMENTS(kinds_a));
  g_assert_true(gnostr_filter_set_manager_add(mgr, a));

  g_autoptr(GnostrFilterSet) b = make_custom("rt-b", "Round Trip B");
  const gchar *tags_b[] = {"bitcoin", "nostr", NULL};
  gnostr_filter_set_set_hashtags(b, tags_b);
  g_assert_true(gnostr_filter_set_manager_add(mgr, b));

  g_autofree gchar *payload = gnostr_filter_set_sync_serialize();
  g_assert_nonnull(payload);

  reset_default_manager();
  g_assert_false(gnostr_filter_set_manager_contains(mgr, "rt-a"));
  g_assert_false(gnostr_filter_set_manager_contains(mgr, "rt-b"));

  g_autoptr(GError) err = NULL;
  g_assert_true(gnostr_filter_set_sync_apply(payload, &err));

  g_assert_true(gnostr_filter_set_manager_contains(mgr, "rt-a"));
  g_assert_true(gnostr_filter_set_manager_contains(mgr, "rt-b"));

  GnostrFilterSet *got_a = gnostr_filter_set_manager_get(mgr, "rt-a");
  g_assert_cmpstr(gnostr_filter_set_get_name(got_a), ==, "Round Trip A");

  reset_default_manager();
}

/* Oracle review P1.1: deletions do NOT propagate across devices. A
 * filter set removed on device A that is absent from a future remote
 * payload must remain on device B after apply(). Lock that
 * contract in so a future refactor can't silently flip the semantics. */
static void
test_apply_does_not_remove_local_only(void)
{
  reset_default_manager();

  GnostrFilterSetManager *mgr = gnostr_filter_set_manager_get_default();
  g_autoptr(GnostrFilterSet) ghost = make_custom("will-survive", "Ghost");
  g_assert_true(gnostr_filter_set_manager_add(mgr, ghost));

  /* Remote payload intentionally omits "will-survive" — mimicking
   * another device's push after the user deleted a different set. */
  const gchar *payload =
    "{\"version\":1,\"filter_sets\":["
      "{\"id\":\"remote-only\",\"name\":\"Remote Only\",\"source\":\"custom\"}"
    "]}";

  g_autoptr(GError) err = NULL;
  g_assert_true(gnostr_filter_set_sync_apply(payload, &err));

  /* Both the local-only and the newly-pulled set must exist. */
  g_assert_true(gnostr_filter_set_manager_contains(mgr, "will-survive"));
  g_assert_true(gnostr_filter_set_manager_contains(mgr, "remote-only"));

  reset_default_manager();
}

/* Oracle review P2.5: apply() must not schedule a debounced push via
 * its own items-changed mutations. Without the applying_remote guard
 * every pull would round-trip into an echo push and clobber the
 * remote event with identical content on every login. */
static void
test_apply_does_not_schedule_push(void)
{
  reset_default_manager();

  /* Enable sync so items-changed fires through the real handler. */
  gnostr_filter_set_sync_enable(
    "deadbeef00000000000000000000000000000000000000000000000000000000");
  g_assert_false(gnostr_filter_set_sync_debug_has_pending_push());

  const gchar *payload =
    "{\"version\":1,\"filter_sets\":["
      "{\"id\":\"echo-1\",\"name\":\"Echo One\",\"source\":\"custom\"},"
      "{\"id\":\"echo-2\",\"name\":\"Echo Two\",\"source\":\"custom\"}"
    "]}";

  g_autoptr(GError) err = NULL;
  g_assert_true(gnostr_filter_set_sync_apply(payload, &err));

  /* The two add() calls inside apply() would normally schedule a
   * debounced push each time items-changed fires. applying_remote
   * must suppress that. */
  g_assert_false(gnostr_filter_set_sync_debug_has_pending_push());

  /* Sanity check: a mutation outside apply() *does* schedule a push. */
  GnostrFilterSetManager *mgr = gnostr_filter_set_manager_get_default();
  g_autoptr(GnostrFilterSet) extra = make_custom("local-edit", "Local Edit");
  g_assert_true(gnostr_filter_set_manager_add(mgr, extra));
  g_assert_true(gnostr_filter_set_sync_debug_has_pending_push());

  gnostr_filter_set_sync_disable();
  reset_default_manager();
}

static void
test_enable_disable_without_relay(void)
{
  /* In TEST_ONLY mode enable()/disable() should not touch the network
   * but must still toggle is_enabled() correctly. */
  reset_default_manager();

  g_assert_false(gnostr_filter_set_sync_is_enabled());
  gnostr_filter_set_sync_enable("deadbeef00000000000000000000000000000000000000000000000000000000");
  g_assert_true(gnostr_filter_set_sync_is_enabled());

  /* Idempotent with same pubkey. */
  gnostr_filter_set_sync_enable("deadbeef00000000000000000000000000000000000000000000000000000000");
  g_assert_true(gnostr_filter_set_sync_is_enabled());

  /* Empty pubkey is ignored. */
  gnostr_filter_set_sync_enable("");
  g_assert_true(gnostr_filter_set_sync_is_enabled());

  gnostr_filter_set_sync_disable();
  g_assert_false(gnostr_filter_set_sync_is_enabled());

  /* Double-disable is safe. */
  gnostr_filter_set_sync_disable();
  g_assert_false(gnostr_filter_set_sync_is_enabled());
}

/* ------------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------------ */

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/filter-set-sync/serialize/empty",
                  test_serialize_empty_no_custom);
  g_test_add_func("/filter-set-sync/serialize/skips-predefined",
                  test_serialize_skips_predefined);
  g_test_add_func("/filter-set-sync/apply/empty-manager",
                  test_apply_into_empty_manager);
  g_test_add_func("/filter-set-sync/apply/updates-existing",
                  test_apply_updates_existing_id);
  g_test_add_func("/filter-set-sync/apply/preserves-local-only",
                  test_apply_preserves_local_only);
  g_test_add_func("/filter-set-sync/apply/forces-source-custom",
                  test_apply_forces_source_to_custom);
  g_test_add_func("/filter-set-sync/apply/empty-payload-noop",
                  test_apply_empty_payload_is_noop);
  g_test_add_func("/filter-set-sync/apply/invalid-json",
                  test_apply_rejects_invalid_json);
  g_test_add_func("/filter-set-sync/apply/non-object-root",
                  test_apply_rejects_non_object_root);
  g_test_add_func("/filter-set-sync/apply/bad-filter-sets-type",
                  test_apply_rejects_bad_filter_sets_type);
  g_test_add_func("/filter-set-sync/round-trip", test_round_trip);
  g_test_add_func("/filter-set-sync/apply/no-delete-of-local-only",
                  test_apply_does_not_remove_local_only);
  g_test_add_func("/filter-set-sync/apply/no-push-echo",
                  test_apply_does_not_schedule_push);
  g_test_add_func("/filter-set-sync/enable-disable",
                  test_enable_disable_without_relay);

  return g_test_run();
}
