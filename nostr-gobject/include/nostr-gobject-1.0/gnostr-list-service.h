#ifndef GNOSTR_LIST_SERVICE_H
#define GNOSTR_LIST_SERVICE_H

#include "gnostr-list-document.h"

G_BEGIN_DECLS

#define GNOSTR_TYPE_LIST_SERVICE (gnostr_list_service_get_type())
NOSTR_GOBJECT_API G_DECLARE_FINAL_TYPE(GNostrListService, gnostr_list_service, GNOSTR, LIST_SERVICE, GObject)

typedef enum {
  GNOSTR_LIST_INACTIVE, GNOSTR_LIST_LOCAL_ONLY, GNOSTR_LIST_WAITING_FOR_SIGNER,
  GNOSTR_LIST_READING, GNOSTR_LIST_PARTIAL, GNOSTR_LIST_UNREADABLE,
  GNOSTR_LIST_DIRTY, GNOSTR_LIST_PUBLISHING, GNOSTR_LIST_SYNCED, GNOSTR_LIST_FAILED,
  GNOSTR_LIST_WAITING_FOR_DEPENDENCY
} GNostrListState;
typedef enum {
  GNOSTR_LIST_AUTO_WHEN_SYNC_ENABLED, GNOSTR_LIST_AUTO_ALWAYS,
  GNOSTR_LIST_EXPLICIT_ONLY, GNOSTR_LIST_READ_ONLY
} GNostrListPublishPolicy;
typedef enum { GNOSTR_LIST_SOURCES_ALL, GNOSTR_LIST_SOURCES_MAJORITY } GNostrListRequiredSources;
typedef enum {
  GNOSTR_LIST_EVENT, GNOSTR_LIST_EOSE, GNOSTR_LIST_CLOSED, GNOSTR_LIST_AUTH_REQUIRED,
  GNOSTR_LIST_ERROR, GNOSTR_LIST_DISCONNECTED, GNOSTR_LIST_RECONNECTED
} GNostrListReadNotification;
typedef enum {
  GNOSTR_LIST_ACCEPTED, GNOSTR_LIST_DUPLICATE, GNOSTR_LIST_REJECTED,
  GNOSTR_LIST_AUTH_FAILURE, GNOSTR_LIST_TRANSPORT_FAILURE, GNOSTR_LIST_CANCELLED
} GNostrListPublishOutcome;
typedef enum {
  GNOSTR_LIST_APPROVE_NONE = 0, GNOSTR_LIST_APPROVE_PUBLIC = 1 << 0,
  GNOSTR_LIST_APPROVE_PRIVATE = 1 << 1, GNOSTR_LIST_APPROVE_PARTIAL = 1 << 2
} GNostrListApproval;

typedef struct {
  int kind;
  const char *identifier; /* "" for non-addressable lists */
  const char *const *sources;
  const char *const *targets;
  const char *read_purpose;
  const char *publish_purpose;
  GNostrListPublishPolicy publish_policy;
  GNostrListRequiredSources required_sources;
  gboolean waiting_for_dependency; /* no subscription or publication until clear */
} GNostrListReadPlanEntry;

/* The table and each plan are copied. All callbacks execute on the captured
 * thread-default context. load/commit use versioned JSON returned by this
 * service; commit must atomically replace one identity's state or leave it
 * unchanged on failure. load returns owned JSON or NULL for no stored state.
 *
 * Async functions MUST complete once, including cancellation, with the standard
 * async/finish pattern. Completion can come from any thread; the service marshals
 * it to its context. Adapter data survives until all callbacks have finished.
 * Crypto always addresses the service account (no caller-selected peer).
 *
 * open_subscription installs author/kind/#d filters with limit and stays live
 * after EOSE. Transport implementations own reconnect/backoff and re-REQ, report
 * RECONNECTED before the next catch-up, and never turn a timer into EOSE. They
 * must gate every transport write/reconnect/AUTH on generation_valid too.
 * notify() may be called from any thread, using the supplied read_serial; stale
 * callbacks are ignored. close_subscription must detach notifications before
 * returning. Cleanup/cancellation remain permitted after generation revocation.
 *
 * publish_finish returns an owned a{si} GVariant: one GNostrListPublishOutcome
 * per target, based on OK (not socket writes). Missing outcomes mean failure.
 * derive_operations returns owned operations to save/rebase before each sign;
 * it is called again following a signing race, not against a borrowed store.
 */
typedef struct {
  gboolean (*generation_valid)(gpointer data, const char *account, guint64 generation);
  char *(*load)(gpointer data, int kind, const char *identifier, GError **error);
  gboolean (*commit)(gpointer data, int kind, const char *identifier, const char *state_json, GError **error);
  gpointer (*open_subscription)(gpointer data, GNostrListService *service, const char *account,
                                guint64 generation, guint64 read_serial,
                                const GNostrListReadPlanEntry *entry, GError **error);
  void (*close_subscription)(gpointer data, gpointer subscription);
  void (*encrypt_async)(gpointer data, const char *account, const char *plaintext,
                        GCancellable *cancel, GAsyncReadyCallback callback, gpointer user_data);
  char *(*encrypt_finish)(gpointer data, GAsyncResult *result, GError **error);
  void (*decrypt_async)(gpointer data, const char *account, const char *ciphertext, gboolean legacy_nip04,
                        GCancellable *cancel, GAsyncReadyCallback callback, gpointer user_data);
  char *(*decrypt_finish)(gpointer data, GAsyncResult *result, GError **error);
  void (*sign_async)(gpointer data, const char *unsigned_json, GCancellable *cancel,
                     GAsyncReadyCallback callback, gpointer user_data);
  char *(*sign_finish)(gpointer data, GAsyncResult *result, GError **error);
  void (*publish_async)(gpointer data, const char *signed_json, const char *const *targets,
                        const char *purpose, GCancellable *cancel,
                        GAsyncReadyCallback callback, gpointer user_data);
  GVariant *(*publish_finish)(gpointer data, GAsyncResult *result, GError **error);
  GPtrArray *(*derive_operations)(gpointer data, GNostrListDocument *snapshot, GError **error);
  gint64 (*now)(gpointer data);
  /* Optional replacement for commit. The immutable, post-edit snapshot and
   * versioned replay state must commit together, including derived indexes.
   * No document-changed signal is emitted if this returns FALSE. The snapshot
   * is borrowed only for this call; it has not yet been installed in the model. */
  gboolean (*commit_snapshot)(gpointer data, GNostrListDocument *snapshot,
                              const char *state_json, GError **error);
} GNostrListAdapter;

/* All calls except notify() are owner-thread-only. Signals: document-changed
 * (GNostrListDocument), state-changed (). Implements GListModel of snapshots.
 * Error domain is G_IO_ERROR; diagnostic messages never contain list contents. */
NOSTR_GOBJECT_API GNostrListService *gnostr_list_service_new(const char *account, guint64 generation,
  const GNostrListAdapter *adapter, gpointer data, GDestroyNotify destroy);
NOSTR_GOBJECT_API gboolean gnostr_list_service_start(GNostrListService *self,
  const GNostrListReadPlanEntry *entries, gsize count, GError **error);
NOSTR_GOBJECT_API gboolean gnostr_list_service_update_read_plan(GNostrListService *self,
  const GNostrListReadPlanEntry *entries, gsize count, GError **error);
/**
 * gnostr_list_service_lookup:
 * @self: the service
 * @kind: document kind
 * @identifier: address identifier, or empty string
 * Returns: (transfer full) (nullable): an immutable snapshot
 */
NOSTR_GOBJECT_API GNostrListDocument *gnostr_list_service_lookup(GNostrListService *self, int kind, const char *identifier);
/**
 * gnostr_list_service_apply:
 * @self: the service
 * @kind: document kind
 * @identifier: address identifier, or empty string
 * @expected_local_revision: optimistic concurrency token
 * @operations: (element-type GNostrListOperation) (transfer none): edits in order;
 *   empty saves a new local identity without scheduling an empty publication
 * @error: return location for a commit or validation error
 * Returns: (transfer full) (nullable): the committed immutable snapshot
 */
NOSTR_GOBJECT_API GNostrListDocument *gnostr_list_service_apply(GNostrListService *self, int kind,
  const char *identifier, guint64 expected_local_revision, GPtrArray *operations, GError **error);
NOSTR_GOBJECT_API void gnostr_list_service_notify(GNostrListService *self, guint64 read_serial,
  int kind, const char *identifier, const char *source, GNostrListReadNotification notification, const char *payload);
NOSTR_GOBJECT_API void gnostr_list_service_publish_async(GNostrListService *self, int kind,
  const char *identifier, GNostrListApproval approval, GCancellable *cancel, GAsyncReadyCallback callback, gpointer user_data);
NOSTR_GOBJECT_API GVariant *gnostr_list_service_publish_finish(GNostrListService *self, GAsyncResult *result, GError **error);
NOSTR_GOBJECT_API void gnostr_list_service_set_private_sync_enabled(GNostrListService *self, gboolean enabled);
NOSTR_GOBJECT_API void gnostr_list_service_retry(GNostrListService *self, int kind, const char *identifier);
NOSTR_GOBJECT_API GNostrListState gnostr_list_service_get_state(GNostrListService *self, int kind, const char *identifier);
NOSTR_GOBJECT_API guint64 gnostr_list_service_get_read_serial(GNostrListService *self);
NOSTR_GOBJECT_API void gnostr_list_service_close(GNostrListService *self);

G_END_DECLS
#endif
