#include "gh-account-controller.h"
#include "gh-identity.h"

#include <glib.h>

static const gchar *npub_one =
  "npub10xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqpkge6d";

typedef struct {
  gboolean fail;
  const gchar *npub;
} Source;

static GPtrArray *
list_source(gpointer data, GError **error)
{
  Source *source = data;
  if (source->fail) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "source unavailable");
    return NULL;
  }
  GPtrArray *items = g_ptr_array_new_with_free_func((GDestroyNotify)gh_identity_info_free);
  if (source->npub) {
    GhIdentityInfo *info = g_new0(GhIdentityInfo, 1);
    info->npub = g_strdup(source->npub);
    g_ptr_array_add(items, info);
  }
  return items;
}

static GSettings *
fresh_settings(void)
{
  GSettings *settings = g_settings_new("org.nostr.Groundhog");
  g_settings_set_string(settings, "current-npub", "");
  g_settings_set_string(settings, "current-backend", "grotto");
  g_settings_set_string(settings, "signer-method", "auto");
  g_settings_set_int(settings, "backend-migration-version", 0);
  return settings;
}

static void
wait_listed(GhAccountController *controller)
{
  while (gh_account_controller_get_state(controller) == GH_ACCOUNT_STATE_DISCOVERING)
    g_main_context_iteration(NULL, TRUE);
}

static void
drain_idle(void)
{
  while (g_main_context_iteration(NULL, FALSE)) {}
}

static void
test_dual_identity_and_gate(void)
{
  Source local = { FALSE, npub_one }, remote = { FALSE, npub_one };
  g_autoptr(GSettings) settings = fresh_settings();
  g_autoptr(GhAccountController) controller =
    gh_account_controller_new_full_with_remote_list(settings, NULL,
      list_source, &local, list_source, &remote);
  wait_listed(controller);
  GPtrArray *items = gh_account_controller_get_identities(controller);
  g_assert_cmpuint(items->len, ==, 2);
  GhIdentityInfo *first = g_ptr_array_index(items, 0);
  GhIdentityInfo *second = g_ptr_array_index(items, 1);
  g_assert_cmpint(first->backend, ==, GH_SIGNER_BACKEND_GROTTO);
  g_assert_cmpint(second->backend, ==, GH_SIGNER_BACKEND_NIP46);
  g_assert_cmpstr(first->npub, ==, second->npub);
  guint64 before = gh_account_controller_get_generation(controller);
  g_assert_true(gh_account_controller_select_backend(controller,
                 GH_SIGNER_BACKEND_NIP46, npub_one, NULL));
  g_assert_cmpuint(gh_account_controller_get_generation(controller), ==, before + 1);
  g_assert_cmpint(gh_account_controller_get_active_backend(controller), ==,
                  GH_SIGNER_BACKEND_NIP46);
  g_assert_cmpint(gh_account_controller_get_state(controller), ==, GH_ACCOUNT_STATE_ACTIVE);
  gh_account_controller_refresh(controller);
  while (gh_account_controller_get_generation(controller) == before + 1)
    g_main_context_iteration(NULL, TRUE);
  g_assert_cmpuint(gh_account_controller_get_generation(controller), ==, before + 2);
  g_autofree gchar *reason = gh_account_controller_describe_limits(controller, TRUE);
  g_assert_nonnull(g_strstr_len(reason, -1, "encrypted storage"));
  guint64 remote_generation = gh_account_controller_get_generation(controller);
  gh_account_controller_set_remote_storage_ready(controller, remote_generation - 1, TRUE);
  g_autofree gchar *still_closed = gh_account_controller_describe_limits(controller, TRUE);
  g_assert_nonnull(g_strstr_len(still_closed, -1, "encrypted storage"));
  gh_account_controller_set_remote_storage_ready(controller, remote_generation, TRUE);
  g_autofree gchar *open_reason = gh_account_controller_describe_limits(controller, TRUE);
  g_assert_null(g_strstr_len(open_reason, -1, "encrypted storage"));
  gh_account_controller_set_remote_storage_ready(controller, remote_generation, FALSE);
  g_autofree gchar *closed_again = gh_account_controller_describe_limits(controller, TRUE);
  g_assert_nonnull(g_strstr_len(closed_again, -1, "encrypted storage"));
  gh_account_controller_set_remote_storage_ready(controller, remote_generation, TRUE);
  g_settings_set_string(settings, "network-mode", "none");
  g_assert_false(gh_account_controller_is_current(controller, remote_generation));
  drain_idle();
  g_assert_cmpuint(gh_account_controller_get_generation(controller), ==,
                   remote_generation + 1);
  g_autofree gchar *after_mode = gh_account_controller_describe_limits(controller, TRUE);
  g_assert_nonnull(g_strstr_len(after_mode, -1, "encrypted storage"));
  g_assert_true(gh_account_controller_select_backend(controller,
                 GH_SIGNER_BACKEND_GROTTO, npub_one, NULL));
  g_assert_cmpuint(gh_account_controller_get_generation(controller), ==,
                   remote_generation + 2);
  g_settings_set_string(settings, "network-mode", "system");
  g_assert_cmpint(gh_account_controller_get_active_backend(controller), ==,
                  GH_SIGNER_BACKEND_GROTTO);
  g_autofree gchar *backend = g_settings_get_string(settings, "current-backend");
  g_assert_cmpstr(backend, ==, "grotto");
  /* Pair transactions must not leave the shared settings reader delayed. */
  g_settings_set_boolean(settings, "run-in-background", FALSE);
  g_assert_false(g_settings_get_has_unapplied(settings));
  g_autoptr(GSettings) another_reader = g_settings_new("org.nostr.Groundhog");
  g_assert_false(g_settings_get_boolean(another_reader, "run-in-background"));
  g_settings_set_boolean(settings, "run-in-background", TRUE);
}

typedef struct {
  GhAccountController *controller;
  gboolean wrong_binding;
  guint changes;
} Trace;

static void
changed(GhAccountController *controller, gpointer data)
{
  Trace *trace = data;
  trace->changes++;
  if (gh_account_controller_get_state(controller) == GH_ACCOUNT_STATE_ACTIVE &&
      g_strcmp0(gh_account_controller_get_active_npub(controller), npub_one) != 0)
    trace->wrong_binding = TRUE;
}

static void
test_idle_pair_reconciliation(void)
{
  Source local = { FALSE, npub_one }, remote = { FALSE, npub_one };
  g_autoptr(GSettings) settings = fresh_settings();
  g_settings_set_string(settings, "current-npub", npub_one);
  g_autoptr(GhAccountController) controller =
    gh_account_controller_new_full_with_remote_list(settings, NULL,
      list_source, &local, list_source, &remote);
  wait_listed(controller);
  guint64 generation = gh_account_controller_get_generation(controller);
  Trace trace = { controller, FALSE, 0 };
  g_signal_connect(controller, "changed", G_CALLBACK(changed), &trace);
  g_settings_set_string(settings, "current-backend", "nip46");
  g_assert_false(gh_account_controller_is_current(controller, generation));
  g_settings_set_string(settings, "current-npub", npub_one);
  g_assert_cmpuint(gh_account_controller_get_generation(controller), ==, generation);
  drain_idle();
  g_assert_cmpuint(gh_account_controller_get_generation(controller), ==, generation + 1);
  g_assert_cmpint(gh_account_controller_get_active_backend(controller), ==,
                  GH_SIGNER_BACKEND_NIP46);
  g_assert_false(trace.wrong_binding);
  g_assert_cmpuint(trace.changes, ==, 1);
  g_settings_set_string(settings, "current-npub", "");
  g_settings_set_string(settings, "current-backend", "grotto");
  drain_idle();
  g_assert_cmpuint(gh_account_controller_get_generation(controller), ==, generation + 2);
  g_assert_null(gh_account_controller_get_active_npub(controller));
}

static void
test_partial_sources(void)
{
  Source local = { TRUE, npub_one }, remote = { FALSE, npub_one };
  g_autoptr(GSettings) settings = fresh_settings();
  g_settings_set_int(settings, "backend-migration-version", 1);
  g_settings_set_string(settings, "current-backend", "nip46");
  g_settings_set_string(settings, "current-npub", npub_one);
  g_autoptr(GhAccountController) controller =
    gh_account_controller_new_full_with_remote_list(settings, NULL,
      list_source, &local, list_source, &remote);
  wait_listed(controller);
  g_assert_cmpint(gh_account_controller_get_state(controller), ==, GH_ACCOUNT_STATE_ACTIVE);
  g_assert_cmpuint(gh_account_controller_get_identities(controller)->len, ==, 1);
  local.fail = FALSE;
  remote.fail = TRUE;
  gh_account_controller_refresh(controller);
  /* A refresh keeps the old list until both new sources finish. */
  while (gh_account_controller_get_state(controller) == GH_ACCOUNT_STATE_ACTIVE)
    g_main_context_iteration(NULL, TRUE);
  g_assert_cmpint(gh_account_controller_get_state(controller), ==,
                  GH_ACCOUNT_STATE_STORE_UNAVAILABLE);
  g_assert_cmpuint(gh_account_controller_get_identities(controller)->len, ==, 1);
  g_assert_cmpstr(gh_account_controller_get_active_npub(controller), ==, NULL);
}

static void
test_migration(void)
{
  Source local = { FALSE, npub_one };
  g_autoptr(GSettings) settings = fresh_settings();
  g_settings_set_string(settings, "current-npub", npub_one);
  g_settings_set_string(settings, "signer-method", "nip46");
  g_autoptr(GhAccountController) controller =
    gh_account_controller_new_full(settings, NULL, list_source, &local);
  g_assert_cmpint(g_settings_get_int(settings, "backend-migration-version"), ==, 1);
  g_autofree gchar *backend = g_settings_get_string(settings, "current-backend");
  g_autofree gchar *method = g_settings_get_string(settings, "signer-method");
  g_assert_cmpstr(backend, ==, "grotto");
  g_assert_cmpstr(method, ==, "auto");
  wait_listed(controller);
  g_assert_cmpint(gh_account_controller_get_state(controller), ==, GH_ACCOUNT_STATE_ACTIVE);
  g_settings_set_string(settings, "current-backend", "nip46");
  g_settings_set_string(settings, "signer-method", "local");
  g_autoptr(GhAccountController) second =
    gh_account_controller_new_full(settings, NULL, list_source, &local);
  g_autofree gchar *unchanged_backend = g_settings_get_string(settings, "current-backend");
  g_autofree gchar *unchanged_method = g_settings_get_string(settings, "signer-method");
  g_assert_cmpstr(unchanged_backend, ==, "nip46");
  g_assert_cmpstr(unchanged_method, ==, "local");
  drain_idle();
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/groundhog/backends/dual-identity-and-gate", test_dual_identity_and_gate);
  g_test_add_func("/groundhog/backends/idle-pair", test_idle_pair_reconciliation);
  g_test_add_func("/groundhog/backends/partial-sources", test_partial_sources);
  g_test_add_func("/groundhog/backends/migration", test_migration);
  return g_test_run();
}
