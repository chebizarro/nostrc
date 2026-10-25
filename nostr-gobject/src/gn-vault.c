/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gn-vault.h"
#include <string.h>

struct _GnVault { GObject parent_instance; };
G_DEFINE_FINAL_TYPE(GnVault, gn_vault, G_TYPE_OBJECT)
G_DEFINE_QUARK(gn-vault-error-quark, gn_vault_error)
static void gn_vault_class_init(GnVaultClass *klass) { (void)klass; }
static void gn_vault_init(GnVault *self) { (void)self; }
GnVault *gn_vault_new(void) { return g_object_new(GN_TYPE_VAULT, NULL); }

struct _GnVaultGrant { gatomicrefcount refs; vault_grant *value; };
GnVaultGrant *gn_vault_grant_ref(GnVaultGrant *grant)
{
  g_atomic_ref_count_inc(&grant->refs);
  return grant;
}
void gn_vault_grant_unref(GnVaultGrant *grant)
{
  if (grant && g_atomic_ref_count_dec(&grant->refs)) {
    vault_grant_free(grant->value);
    g_free(grant);
  }
}
G_DEFINE_BOXED_TYPE(GnVaultGrant, gn_vault_grant, gn_vault_grant_ref, gn_vault_grant_unref)
const vault_grant *gn_vault_grant_peek(const GnVaultGrant *grant)
{
  g_return_val_if_fail(grant != NULL, NULL);
  return grant->value;
}

struct _GnVaultRevocation { gatomicrefcount refs; vault_revoke_result *value; };
GnVaultRevocation *gn_vault_revocation_ref(GnVaultRevocation *revocation)
{
  g_atomic_ref_count_inc(&revocation->refs);
  return revocation;
}
void gn_vault_revocation_unref(GnVaultRevocation *revocation)
{
  if (revocation && g_atomic_ref_count_dec(&revocation->refs)) {
    vault_revoke_result_free(revocation->value);
    g_free(revocation);
  }
}
G_DEFINE_BOXED_TYPE(GnVaultRevocation, gn_vault_revocation, gn_vault_revocation_ref, gn_vault_revocation_unref)
const vault_revoke_result *gn_vault_revocation_peek(const GnVaultRevocation *revocation)
{
  g_return_val_if_fail(revocation != NULL, NULL);
  return revocation->value;
}

static GVariant *bytes_variant(const guint8 *bytes, gsize len)
{
  return g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, bytes, len, sizeof(guint8));
}
static GVariant *fold_variant(const vault_fold_result *r)
{
  GVariantBuilder dict, frontier, dispositions, conflicts, roots;
  g_variant_builder_init(&dict, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&dict, "{sv}", "view",
      g_variant_new_uint64(r->view == SIZE_MAX ? G_MAXUINT64 : r->view));
  g_variant_builder_init(&frontier, G_VARIANT_TYPE("at"));
  for (gsize i = 0; i < r->frontier_len; ++i)
    g_variant_builder_add(&frontier, "t", (guint64)r->frontier[i]);
  g_variant_builder_add(&dict, "{sv}", "frontier", g_variant_builder_end(&frontier));
  g_variant_builder_init(&dispositions, G_VARIANT_TYPE("au"));
  for (gsize i = 0; i < r->records_len; ++i)
    g_variant_builder_add(&dispositions, "u", (guint32)r->dispositions[i]);
  g_variant_builder_add(&dict, "{sv}", "dispositions", g_variant_builder_end(&dispositions));
  g_variant_builder_init(&conflicts, G_VARIANT_TYPE("aa{sv}"));
  for (gsize i = 0; i < r->conflicts_len; ++i) {
    const vault_path_conflict *c = &r->conflicts[i];
    GVariantBuilder item;
    g_variant_builder_init(&item, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&item, "{sv}", "path-enc", g_variant_new_string(c->path_enc));
    g_variant_builder_add(&item, "{sv}", "other-record", g_variant_new_uint64(c->other_record));
    g_variant_builder_add(&item, "{sv}", "in-owner", g_variant_new_boolean(c->in_owner));
    g_variant_builder_add(&item, "{sv}", "in-other", g_variant_new_boolean(c->in_other));
    g_variant_builder_add(&item, "{sv}", "writer", bytes_variant(c->writer, 32));
    g_variant_builder_add(&item, "{sv}", "root-manifest", bytes_variant(c->root_manifest, 32));
    g_variant_builder_add(&item, "{sv}", "seq", g_variant_new_uint64(c->seq));
    g_variant_builder_add_value(&conflicts, g_variant_builder_end(&item));
  }
  g_variant_builder_add(&dict, "{sv}", "conflicts", g_variant_builder_end(&conflicts));
  g_variant_builder_init(&roots, G_VARIANT_TYPE("aay"));
  for (gsize i = 0; i < r->retained_roots_len; ++i)
    g_variant_builder_add_value(&roots, bytes_variant(r->retained_roots[i], 32));
  g_variant_builder_add(&dict, "{sv}", "retained-roots", g_variant_builder_end(&roots));
  return g_variant_ref_sink(g_variant_builder_end(&dict));
}
static GVariant *reseal_variant(const vault_reseal_task *task)
{
  GVariantBuilder dict;
  const guint8 *pending = vault_reseal_task_pending_address(task);
  g_variant_builder_init(&dict, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&dict, "{sv}", "completed", g_variant_new_uint64(vault_reseal_task_completed(task)));
  g_variant_builder_add(&dict, "{sv}", "reserved", g_variant_new_uint64(vault_reseal_task_reserved(task)));
  g_variant_builder_add(&dict, "{sv}", "pending-address", bytes_variant(pending, pending ? 32 : 0));
  return g_variant_ref_sink(g_variant_builder_end(&dict));
}

typedef enum { SHARE, ACCEPT, FOLD, REVOKE, RESEAL } Operation;
typedef struct {
  Operation op;
  vault_grant_carrier carrier;
  guint8 actor[32], member[32];
  const vault_grant *grant;
  const porthome_vault_info *anchor;
  const vault_record *const *records;
  gsize count;
  const porthome_dir_node *node;
  const vault_ring *ring;
  const porthome_node_entry *parent;
  const porthome_head *head;
  guint64 written_at, budget;
  vault_reseal_task *reseal;
  vault_blob_io io;
  GVariant *details;
  gint status;
} Request;
static void request_free(gpointer data)
{
  Request *r = data;
  g_clear_pointer(&r->details, g_variant_unref);
  g_free(r);
}
static GError *operation_error(gint status)
{
  return g_error_new(GN_VAULT_ERROR, -status, "libvault operation failed (status %d)", status);
}
static void worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
  (void)source; (void)cancel;
  Request *r = data;
  if (g_task_return_error_if_cancelled(task)) return;
  gpointer answer = NULL;
  GDestroyNotify destroy = NULL;
  switch (r->op) {
  case SHARE:
    r->status = vault_grant_send(&r->carrier, r->member, r->grant);
    break;
  case ACCEPT: {
    vault_grant *grant = NULL;
    r->status = vault_grant_receive(&r->carrier, &grant);
    if (r->status == VAULT_OK) {
      GnVaultGrant *owned = g_new0(GnVaultGrant, 1);
      g_atomic_ref_count_init(&owned->refs);
      owned->value = grant;
      answer = owned;
      destroy = (GDestroyNotify)gn_vault_grant_unref;
    } else {
      vault_grant_free(grant);
    }
    break;
  }
  case FOLD: {
    vault_fold_result *fold = NULL;
    r->status = vault_fold(r->anchor, r->records, r->count, &fold);
    if (fold) r->details = fold_variant(fold);
    vault_fold_result_free(fold);
    break;
  }
  case REVOKE: {
    vault_revoke_result *revocation = NULL;
    r->status = vault_revoke(r->actor, r->member, r->node, r->ring,
        r->parent, r->head, r->written_at, &revocation);
    if (r->status == VAULT_OK) {
      GnVaultRevocation *owned = g_new0(GnVaultRevocation, 1);
      g_atomic_ref_count_init(&owned->refs);
      owned->value = revocation;
      answer = owned;
      destroy = (GDestroyNotify)gn_vault_revocation_unref;
    } else {
      vault_revoke_result_free(revocation);
    }
    break;
  }
  case RESEAL:
    r->status = vault_reseal_task_run(r->reseal, r->budget, &r->io);
    r->details = reseal_variant(r->reseal);
    break;
  }
  if (r->status != VAULT_OK) g_task_return_error(task, operation_error(r->status));
  else if (r->op == ACCEPT || r->op == REVOKE) g_task_return_pointer(task, answer, destroy);
  else g_task_return_boolean(task, TRUE);
}
static GTask *request_new(GnVault *self, Operation op, gpointer tag,
    GCancellable *cancel, GAsyncReadyCallback callback, gpointer data)
{
  GTask *task = g_task_new(self, cancel, callback, data);
  Request *r = g_new0(Request, 1);
  r->op = op;
  /* Late cancellation must not obscure a sent grant or mutated re-seal task. */
  g_task_set_check_cancellable(task, FALSE);
  g_task_set_source_tag(task, tag);
  g_task_set_task_data(task, r, request_free);
  return task;
}
static void start(GTask *task)
{
  g_task_run_in_thread(task, worker);
  g_object_unref(task);
}
static gboolean copy_identity(GBytes *bytes, guint8 out[32])
{
  if (!bytes || g_bytes_get_size(bytes) != 32) return FALSE;
  memcpy(out, g_bytes_get_data(bytes, NULL), 32);
  return TRUE;
}
void gn_vault_share_async(GnVault *self, const vault_grant_carrier *carrier,
    GBytes *recipient, const vault_grant *grant, GCancellable *cancel,
    GAsyncReadyCallback callback, gpointer data)
{
  g_return_if_fail(GN_IS_VAULT(self) && carrier && grant);
  GTask *task = request_new(self, SHARE, gn_vault_share_async, cancel, callback, data);
  Request *r = g_task_get_task_data(task);
  r->carrier = *carrier;
  r->grant = grant;
  if (!copy_identity(recipient, r->member)) {
    g_task_return_error(task, operation_error(VAULT_ERR_ARG));
    g_object_unref(task);
    return;
  }
  start(task);
}
gboolean gn_vault_share_finish(GnVault *self, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, self) &&
      g_async_result_is_tagged(result, gn_vault_share_async), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}
void gn_vault_accept_async(GnVault *self, const vault_grant_carrier *carrier,
    GCancellable *cancel, GAsyncReadyCallback callback, gpointer data)
{
  g_return_if_fail(GN_IS_VAULT(self) && carrier);
  GTask *task = request_new(self, ACCEPT, gn_vault_accept_async, cancel, callback, data);
  Request *r = g_task_get_task_data(task);
  r->carrier = *carrier;
  start(task);
}
GnVaultGrant *gn_vault_accept_finish(GnVault *self, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, self) &&
      g_async_result_is_tagged(result, gn_vault_accept_async), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}
void gn_vault_fold_async(GnVault *self, const porthome_vault_info *anchor,
    const vault_record *const *records, gsize count, GCancellable *cancel,
    GAsyncReadyCallback callback, gpointer data)
{
  g_return_if_fail(GN_IS_VAULT(self) && anchor && (records || !count));
  GTask *task = request_new(self, FOLD, gn_vault_fold_async, cancel, callback, data);
  Request *r = g_task_get_task_data(task);
  r->anchor = anchor; r->records = records; r->count = count;
  start(task);
}
static gboolean details_finish(GAsyncResult *result, GVariant **details, GError **error)
{
  Request *r = g_task_get_task_data(G_TASK(result));
  if (details) *details = r->details ? g_variant_ref(r->details) : NULL;
  return g_task_propagate_boolean(G_TASK(result), error);
}
gboolean gn_vault_fold_finish(GnVault *self, GAsyncResult *result, GVariant **fold, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, self) &&
      g_async_result_is_tagged(result, gn_vault_fold_async), FALSE);
  return details_finish(result, fold, error);
}
void gn_vault_revoke_async(GnVault *self, GBytes *actor, GBytes *member,
    const porthome_dir_node *node, const vault_ring *ring,
    const porthome_node_entry *parent, const porthome_head *root_head,
    guint64 written_at, GCancellable *cancel, GAsyncReadyCallback callback, gpointer data)
{
  g_return_if_fail(GN_IS_VAULT(self) && node && ring);
  GTask *task = request_new(self, REVOKE, gn_vault_revoke_async, cancel, callback, data);
  Request *r = g_task_get_task_data(task);
  r->node = node; r->ring = ring; r->parent = parent; r->head = root_head;
  r->written_at = written_at;
  if (!copy_identity(actor, r->actor) || !copy_identity(member, r->member)) {
    g_task_return_error(task, operation_error(VAULT_ERR_ARG));
    g_object_unref(task);
    return;
  }
  start(task);
}
GnVaultRevocation *gn_vault_revoke_finish(GnVault *self, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, self) &&
      g_async_result_is_tagged(result, gn_vault_revoke_async), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}
void gn_vault_reseal_async(GnVault *self, vault_reseal_task *reseal,
    guint64 budget, const vault_blob_io *io, GCancellable *cancel,
    GAsyncReadyCallback callback, gpointer data)
{
  g_return_if_fail(GN_IS_VAULT(self) && reseal && io);
  GTask *task = request_new(self, RESEAL, gn_vault_reseal_async, cancel, callback, data);
  Request *r = g_task_get_task_data(task);
  r->reseal = reseal; r->budget = budget; r->io = *io;
  start(task);
}
gboolean gn_vault_reseal_finish(GnVault *self, GAsyncResult *result, GVariant **progress, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, self) &&
      g_async_result_is_tagged(result, gn_vault_reseal_async), FALSE);
  return details_finish(result, progress, error);
}
