#include "gh-mls-invitee.h"

#include "gh-mls-key-packages.h"

#include <string.h>

GhMlsInviteeState
gh_mls_invitee_classify(const GhMlsKeyPackage *key_package, const GError *error,
                        gboolean require_proof)
{
  if (key_package) {
    if (key_package->adopted) {
      if (!key_package->legacy_event_json)
        return GH_MLS_INVITEE_READY_ADOPTED_ONLY;
#if GH_MLS_SERVICE_ACCOUNT_PROOF
      /* Their MDK 0.8 KeyPackage is only for an older-format group: judged
       * by the same proof rule as an MDK 0.8-only person's (review L2). If
       * it can't be used, they join newer-format groups only. */
      bool legacy_proven = false;
      MarmotError legacy_err =
        marmot_key_package_event_has_account_proof(key_package->legacy_event_json,
                                                   &legacy_proven);
      if (legacy_err != MARMOT_OK || (!legacy_proven && require_proof))
        return GH_MLS_INVITEE_READY_ADOPTED_ONLY;
#endif
      return GH_MLS_INVITEE_READY;
    }
#if GH_MLS_SERVICE_ACCOUNT_PROOF
    bool proven = false;
    MarmotError err = marmot_key_package_event_has_account_proof(key_package->event_json,
                                                                 &proven);
    if (err != MARMOT_OK)
      return GH_MLS_INVITEE_NOT_SET_UP;   /* the lookup selected it: not expected */
    if (proven)
      return GH_MLS_INVITEE_READY_LEGACY;
    return require_proof ? GH_MLS_INVITEE_NEEDS_UPDATE : GH_MLS_INVITEE_READY_UNPROVEN;
#else
    (void)require_proof;
    return GH_MLS_INVITEE_READY_LEGACY;
#endif
  }
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    return GH_MLS_INVITEE_NOT_SET_UP;
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE))
    return GH_MLS_INVITEE_UNREACHABLE;
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT))
    return GH_MLS_INVITEE_NO_RELAYS;
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return GH_MLS_INVITEE_CHECKING;
  return GH_MLS_INVITEE_FAILED;
}

static void
lookup_done(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  GTask *task = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GhMlsKeyPackage) key_package = gh_mls_key_package_lookup_finish(result, &error);
  if (!key_package && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_task_return_error(task, g_steal_pointer(&error));
  else
    g_task_return_int(task, gh_mls_invitee_classify(key_package, error,
                                                    GPOINTER_TO_INT(g_task_get_task_data(task))));
  g_object_unref(task);
}

void
gh_mls_invitee_check_async(GhAccountController *accounts, GSettings *settings,
                           const gchar *pubkey, guint deadline,
                           const gchar *const *group_relays,
                           GCancellable *cancellable,
                           GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(GH_IS_ACCOUNT_CONTROLLER(accounts));
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  g_task_set_source_tag(task, gh_mls_invitee_check_async);
  g_task_set_task_data(task, GINT_TO_POINTER(gh_mls_requires_proofs(settings)), NULL);
  /* The service's own lookup sources (gh-mls-service.c discovery_relays()):
   * the lookup skips any URL it can't use. */
  g_auto(GStrv) sources = settings ? g_settings_get_strv(settings, "discovery-relays")
                                   : g_new0(gchar *, 1);
  /* @group_relays (nostrc-c0yo): skip the new group's relays, exactly as
   * look_up_invitees() does at Create.  Without this an invitee whose only
   * write relay is also a group relay shows Ready but Create fails. */
  gh_mls_key_package_lookup_async(accounts, (const gchar *const *)sources, group_relays, pubkey,
                                  deadline, cancellable, lookup_done, task);
}

GhMlsInviteeState
gh_mls_invitee_check_finish(GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, NULL), GH_MLS_INVITEE_FAILED);
  GError *local = NULL;
  gssize state = g_task_propagate_int(G_TASK(result), &local);
  if (local) {
    g_propagate_error(error, local);
    return GH_MLS_INVITEE_CHECKING;
  }
  return (GhMlsInviteeState)state;
}

static gboolean
hex64(const gchar *value)
{
  if (!value || strlen(value) != 64)
    return FALSE;
  for (const gchar *p = value; *p; p++)
    if (!g_ascii_isxdigit(*p))
      return FALSE;
  return TRUE;
}

static gint
compare_strings(gconstpointer a, gconstpointer b)
{
  return g_strcmp0(*(const gchar *const *)a, *(const gchar *const *)b);
}

GStrv
gh_mls_contacts_dup(GhConversationStore *model)
{
  g_return_val_if_fail(GH_IS_CONVERSATION_STORE(model), NULL);
  const gchar *account = gh_conversation_store_get_account(model);
  g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  GListModel *rooms = G_LIST_MODEL(model);
  guint n = g_list_model_get_n_items(rooms);
  for (guint i = 0; i < n; i++) {
    g_autoptr(GhConversation) room = g_list_model_get_item(rooms, i);
    GhConversationBackend backend = gh_conversation_get_backend(room);
    /* NIP-17 peers, Marmot DM peers, and co-members of accepted MLS
     * groups are all invitable. The contact directory (PT-8) is narrower
     * (DM peers only); this list is for the invitee picker. */
    gboolean invitable = backend == GH_CONVERSATION_BACKEND_NIP17 ||
                         backend == GH_CONVERSATION_BACKEND_MLS;
    if (!invitable || gh_conversation_get_is_request(room))
      continue;
    const gchar *const *peers = gh_conversation_get_peers(room);
    for (guint j = 0; peers && peers[j]; j++) {
      gchar *lower = g_ascii_strdown(peers[j], -1);
      if (!hex64(lower) || g_strcmp0(lower, account) == 0 ||
          g_hash_table_contains(seen, lower))
        g_free(lower);
      else
        g_hash_table_add(seen, lower);
    }
  }
  g_autoptr(GPtrArray) out = g_ptr_array_new();
  GHashTableIter iter;
  gpointer key;
  g_hash_table_iter_init(&iter, seen);
  while (g_hash_table_iter_next(&iter, &key, NULL))
    g_ptr_array_add(out, g_strdup(key));
  g_ptr_array_sort(out, compare_strings);
  g_ptr_array_add(out, NULL);
  return (GStrv)g_ptr_array_steal(out, NULL);
}

gboolean
gh_mls_is_contact(GhConversationStore *model, const gchar *pubkey)
{
  g_auto(GStrv) contacts = gh_mls_contacts_dup(model);
  g_autofree gchar *lower = pubkey ? g_ascii_strdown(pubkey, -1) : NULL;
  return lower && contacts && g_strv_contains((const gchar *const *)contacts, lower);
}

static gboolean
group_id_of(GhMlsGroup *group, MarmotGroupId *out)
{
  const gchar *hex = gh_mls_group_get_group_id(group);
  gsize length = hex ? strlen(hex) : 0;
  if (length == 0 || length % 2 != 0)
    return FALSE;
  g_autofree guint8 *bytes = g_malloc(length / 2);
  for (gsize i = 0; i < length / 2; i++) {
    gint high = g_ascii_xdigit_value(hex[2 * i]);
    gint low = g_ascii_xdigit_value(hex[2 * i + 1]);
    if (high < 0 || low < 0)
      return FALSE;
    bytes[i] = (guint8)(high * 16 + low);
  }
  *out = marmot_group_id_new(bytes, length / 2);
  return TRUE;
}

GStrv
gh_mls_group_dup_ordered_admins(GhMlsService *service, GhMlsGroup *group)
{
  g_return_val_if_fail(GH_IS_MLS_SERVICE(service), NULL);
  g_return_val_if_fail(GH_IS_MLS_GROUP(group), NULL);
  Marmot *marmot = gh_mls_service_get_marmot(service);
  MarmotGroupId gid;
  MarmotGroup *data = NULL;
  if (marmot && group_id_of(group, &gid)) {
    if (marmot_get_group(marmot, &gid, &data) != MARMOT_OK)
      data = NULL;
    marmot_group_id_free(&gid);
  }
  if (!data)
    return gh_mls_group_dup_admins(group);
  GStrvBuilder *builder = g_strv_builder_new();
  for (size_t i = 0; i < data->admin_count; i++) {
    g_autofree gchar *hex = g_malloc(65);
    for (guint j = 0; j < 32; j++)
      g_snprintf(hex + 2 * j, 3, "%02x", data->admin_pubkeys[i][j]);
    g_strv_builder_add(builder, hex);
  }
  marmot_group_free(data);
  GStrv admins = g_strv_builder_end(builder);
  g_strv_builder_unref(builder);
  return admins;
}

GhMlsRole
gh_mls_role_of(const gchar *const *ordered_admins, const gchar *pubkey)
{
  if (!ordered_admins || !pubkey)
    return GH_MLS_ROLE_MEMBER;
  for (guint i = 0; ordered_admins[i]; i++)
    if (g_ascii_strcasecmp(ordered_admins[i], pubkey) == 0)
      return i == 0 ? GH_MLS_ROLE_OWNER : GH_MLS_ROLE_ADMIN;
  return GH_MLS_ROLE_MEMBER;
}
