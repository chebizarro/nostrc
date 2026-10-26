/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GN_VAULT_H
#define GN_VAULT_H
#include "nostr-gobject-export.h"
#include <gio/gio.h>
#include <vault/vault.h>
G_BEGIN_DECLS

#define GN_TYPE_VAULT (gn_vault_get_type())
NOSTR_GOBJECT_API G_DECLARE_FINAL_TYPE(GnVault, gn_vault, GN, VAULT, GObject)

/* GN_VAULT_ERROR codes are the negated libvault/porthome status, preserving
 * every lower-layer distinction (including EPOCH, AUTH, AGAIN and BUDGET).
 * Cancellation before dispatch uses G_IO_ERROR_CANCELLED instead. */
#define GN_VAULT_ERROR (gn_vault_error_quark())
NOSTR_GOBJECT_API GQuark gn_vault_error_quark(void);

/* Ref-counted boxed owners. Native views must not be freed or replaced. Only
 * a libvault re-seal task may mutate a proposal's node under its usual contract.
 * Grant disposal uses libvault's secret-wiping destructor, not a JSON GVariant. */
typedef struct _GnVaultGrant GnVaultGrant;
#define GN_TYPE_VAULT_GRANT (gn_vault_grant_get_type())
NOSTR_GOBJECT_API GType gn_vault_grant_get_type(void) G_GNUC_CONST;
NOSTR_GOBJECT_API GnVaultGrant *gn_vault_grant_ref(GnVaultGrant *grant);
NOSTR_GOBJECT_API void gn_vault_grant_unref(GnVaultGrant *grant);
/**
 * gn_vault_grant_peek: (skip)
 * Returns: (transfer none): grant metadata and key ring, valid until unref
 */
NOSTR_GOBJECT_API const vault_grant *gn_vault_grant_peek(const GnVaultGrant *grant);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GnVaultGrant, gn_vault_grant_unref)

typedef struct _GnVaultRevocation GnVaultRevocation;
#define GN_TYPE_VAULT_REVOCATION (gn_vault_revocation_get_type())
NOSTR_GOBJECT_API GType gn_vault_revocation_get_type(void) G_GNUC_CONST;
NOSTR_GOBJECT_API GnVaultRevocation *gn_vault_revocation_ref(GnVaultRevocation *revocation);
NOSTR_GOBJECT_API void gn_vault_revocation_unref(GnVaultRevocation *revocation);
/**
 * gn_vault_revocation_peek: (skip)
 * Returns: (transfer none): atomic proposal; caller owns re-sealing ancestors,
 *   delivering the new ring and signed publication, as specified by libvault
 */
NOSTR_GOBJECT_API const vault_revoke_result *gn_vault_revocation_peek(const GnVaultRevocation *revocation);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GnVaultRevocation, gn_vault_revocation_unref)

/**
 * gn_vault_new:
 *
 * Thin C-facing adapter. No transport, identity, publication or key policy is
 * selected here. All libvault calls and carrier/blob callbacks run on GTask
 * workers; completion runs on the initiating thread's main context.
 *
 * Native pointer inputs (including their nested data, carrier/I/O contexts,
 * records and re-seal task) are borrowed until the completion callback returns.
 * Do not mutate/free them in that interval or use the same mutable re-seal
 * task/node concurrently. Callback structs and identity bytes are copied.
 * Carrier/blob callbacks must be worker-thread safe; callers must serialize
 * operations that share a non-thread-safe callback context.
 *
 * Cancellation is checked immediately before entering libvault. Its callback
 * ABI cannot interrupt in-flight work. Once entered, the actual result is
 * reported even if cancelled, so completed effects/partial progress are never
 * hidden by a late cancellation. No operation retries or publishes implicitly.
 * Returns: (transfer full): a new adapter
 */
NOSTR_GOBJECT_API GnVault *gn_vault_new(void);

/**
 * gn_vault_share_async: (skip)
 * Builds and delivers a grant from caller-supplied metadata/ring through any
 * carrier. Does not create a vault tree/head or sign/publish a bare rumor.
 */
NOSTR_GOBJECT_API void gn_vault_share_async(GnVault *self, const vault_grant_carrier *carrier,
    GBytes *recipient, const vault_grant *grant, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data);
NOSTR_GOBJECT_API gboolean gn_vault_share_finish(GnVault *self, GAsyncResult *result, GError **error);
/**
 * gn_vault_accept_async: (skip)
 * libvault verifies the authenticated carrier sender against the grant owner.
 */
NOSTR_GOBJECT_API void gn_vault_accept_async(GnVault *self, const vault_grant_carrier *carrier,
    GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data);
/**
 * gn_vault_accept_finish:
 * Returns: (transfer full) (nullable): an owned, secret-wiping grant
 */
NOSTR_GOBJECT_API GnVaultGrant *gn_vault_accept_finish(GnVault *self, GAsyncResult *result, GError **error);

/**
 * gn_vault_fold_async: (skip)
 * Folds verified records under caller-supplied trusted owner metadata.
 */
NOSTR_GOBJECT_API void gn_vault_fold_async(GnVault *self, const porthome_vault_info *anchor,
    const vault_record *const *records, gsize count, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data);
/**
 * gn_vault_fold_finish:
 * @self: an adapter
 * @result: async result
 * @fold: (out) (transfer full) (nullable): a{sv} result, also on partial failure
 * @error: return location for an error
 *
 * Keys: view (t, input index or G_MAXUINT64), frontier (at), dispositions (au),
 * conflicts (aa{sv}: path-enc s, other-record t, in-owner b, in-other b,
 * writer ay, root-manifest ay, seq t), retained-roots (aay).
 * No view is fabricated on CONFLICT/INCOMPLETE. Indices refer to input order.
 * Returns: TRUE on VAULT_OK; FALSE with the exact error and any partial result
 */
NOSTR_GOBJECT_API gboolean gn_vault_fold_finish(GnVault *self, GAsyncResult *result, GVariant **fold, GError **error);

/**
 * gn_vault_revoke_async: (skip)
 * Calls libvault's atomic proposal operation. Exactly one parent/head is needed.
 */
NOSTR_GOBJECT_API void gn_vault_revoke_async(GnVault *self, GBytes *actor, GBytes *member,
    const porthome_dir_node *node, const vault_ring *ring,
    const porthome_node_entry *parent, const porthome_head *root_head,
    guint64 written_at, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data);
/**
 * gn_vault_revoke_finish:
 * Returns: (transfer full) (nullable): an owned proposal, not published authority
 */
NOSTR_GOBJECT_API GnVaultRevocation *gn_vault_revoke_finish(GnVault *self, GAsyncResult *result, GError **error);

/**
 * gn_vault_reseal_async: (skip)
 * Runs an existing task with the exact caller-supplied bandwidth budget.
 * Node/ring/task ownership and terminal I/O failure rules remain libvault's.
 */
NOSTR_GOBJECT_API void gn_vault_reseal_async(GnVault *self, vault_reseal_task *task,
    guint64 bandwidth_budget, const vault_blob_io *io, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data);
/**
 * gn_vault_reseal_finish:
 * @self: an adapter
 * @result: async result
 * @progress: (out) (transfer full) (nullable): a{sv}, also on partial failure
 * @error: return location for an error
 *
 * Keys: completed (t), reserved (t), pending-address (ay, empty if absent).
 * Returns: TRUE on VAULT_OK; FALSE with exact error and progress, including on
 *   BUDGET/IO. Cancellation before dispatch has no progress result.
 */
NOSTR_GOBJECT_API gboolean gn_vault_reseal_finish(GnVault *self, GAsyncResult *result,
    GVariant **progress, GError **error);
G_END_DECLS
#endif
