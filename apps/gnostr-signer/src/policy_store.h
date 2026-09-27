#ifndef APPS_GNOSTR_SIGNER_POLICY_STORE_H
#define APPS_GNOSTR_SIGNER_POLICY_STORE_H
#include <glib.h>

typedef struct _PolicyStore PolicyStore;

/* Remembered approval decisions, keyed on (identity, request kind, app).
 * The kind is the org.nostr.Signer ApprovalRequested kind ("event",
 * "nip44_conversation_key", "nip44_decrypt", "get_public_key", ...) so that
 * allowing one kind never covers another (nostrc-f7hk); app_id is the
 * principal the signer verified (ApprovalRequested's app_id). Entries saved
 * before kinds existed load as kind "event". */
#define POLICY_KIND_EVENT "event"

typedef struct {
  gchar *app_id;
  gchar *identity;
  gboolean decision;        /* TRUE=approve, FALSE=deny */
  guint64 expires_at;       /* unix epoch seconds; 0 = forever */
  gchar *kind;              /* request kind; free with the entry */
} PolicyEntry;

void policy_entry_free(PolicyEntry *e);

PolicyStore *policy_store_new(void);
void policy_store_free(PolicyStore *ps);

/* Load from disk (no error if missing). */
void policy_store_load(PolicyStore *ps);
/* Save to disk; best-effort. */
void policy_store_save(PolicyStore *ps);

/* Lookup; returns TRUE if a remembered decision exists. */
gboolean policy_store_get(PolicyStore *ps, const gchar *app_id, const gchar *identity, gboolean *out_decision);

/* Set or update decision. */
void policy_store_set(PolicyStore *ps, const gchar *app_id, const gchar *identity, gboolean decision);

/* Set or update decision with TTL (in seconds). ttl_seconds==0 means forever. */
void policy_store_set_with_ttl(PolicyStore *ps, const gchar *app_id, const gchar *identity, gboolean decision, guint64 ttl_seconds);

/* Remove a policy; returns TRUE if removed. */
gboolean policy_store_unset(PolicyStore *ps, const gchar *app_id, const gchar *identity);

/* Kind-aware variants; the kind-less calls above mean POLICY_KIND_EVENT. */
gboolean policy_store_get_for_kind(PolicyStore *ps, const gchar *kind, const gchar *app_id,
                                   const gchar *identity, gboolean *out_decision);
void policy_store_set_for_kind(PolicyStore *ps, const gchar *kind, const gchar *app_id,
                               const gchar *identity, gboolean decision, guint64 ttl_seconds);
gboolean policy_store_unset_for_kind(PolicyStore *ps, const gchar *kind, const gchar *app_id,
                                     const gchar *identity);

/* Enumerate all entries (including expired); caller owns returned GPtrArray of PolicyEntry* and each entry fields. */
GPtrArray *policy_store_list(PolicyStore *ps);
#endif /* APPS_GNOSTR_SIGNER_POLICY_STORE_H */
