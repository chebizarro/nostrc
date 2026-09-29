#ifndef GH_INBOX_RESOLVER_H
#define GH_INBOX_RESOLVER_H

#include <gio/gio.h>

G_BEGIN_DECLS

/*
 * GhInboxResolver: where to deliver a NIP-17 gift wrap for one recipient,
 * i.e. that recipient's kind-10050 DM inbox relay list.
 *
 * The send pipeline takes recipient inboxes only through this interface, so the
 * contact directory (cached 10050s refreshed off the send path, G10) backs it
 * in the app instead of a send-time lookup (GhInboxLookup). Every implementation must:
 *  - answer only from a signed kind-10050 authored by that recipient, newest
 *    first (NIP-01); never from kind 10002, home relays or any default;
 *  - report absence honestly (NOT_FOUND/EMPTY) and distinguish it from not
 *    having been able to ask anyone (UNREACHABLE/NO_SOURCES);
 *  - never authenticate as the account while looking others up;
 *  - be bound to the account generation at the call: a switch or the
 *    caller's cancellable finishes the call with G_IO_ERROR_CANCELLED.
 *
 * "changed" (gchar *pubkey_hex, lowercase) is emitted on the main context by
 * an implementation that refreshes lists in the background (the contact
 * directory, charter §4.5 S2) when what it would now answer for that
 * recipient differs from an earlier answer. A consumer that already sealed a
 * wrap for the recipient resolves again and adds the new relays as targets
 * of the same stored wrap. A send-time lookup never emits it.
 */

typedef enum {
  GH_INBOX_FOUND,       /* newest valid list names >= 1 usable relay */
  GH_INBOX_EMPTY,       /* newest valid list names no usable relay */
  GH_INBOX_NOT_FOUND,   /* >= 1 source answered; no valid list exists */
  GH_INBOX_UNREACHABLE, /* no source answered and none sent a list */
  GH_INBOX_NO_SOURCES   /* nothing to ask */
} GhInboxStatus;

typedef struct {
  GhInboxStatus status;
  gchar *recipient;   /* lowercase hex pubkey */
  GStrv relays;       /* FOUND only: NULL-terminated ws(s) URLs, else NULL */
  gchar *event_id;    /* the kind-10050 used (FOUND/EMPTY), else NULL */
  gint64 created_at;  /* of that event, else 0 */
  guint sources;      /* relays asked (0 when served from a cache) */
  guint answered;     /* sources that sent EOSE */
  guint failed;       /* sources that failed or timed out before EOSE */
  gboolean truncated; /* the list named more than 16 usable relays */
  gboolean cached;    /* served without a network request */
} GhInboxResult;

GhInboxResult *gh_inbox_result_copy(const GhInboxResult *result);
void gh_inbox_result_free(GhInboxResult *result);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhInboxResult, gh_inbox_result_free)

#define GH_TYPE_INBOX_RESOLVER (gh_inbox_resolver_get_type())
G_DECLARE_INTERFACE(GhInboxResolver, gh_inbox_resolver, GH, INBOX_RESOLVER, GObject)

struct _GhInboxResolverInterface {
  GTypeInterface parent_iface;

  void (*resolve_async)(GhInboxResolver *self, const gchar *pubkey_hex,
                        GCancellable *cancellable, GAsyncReadyCallback callback,
                        gpointer user_data);
  GhInboxResult *(*resolve_finish)(GhInboxResolver *self, GAsyncResult *result,
                                   GError **error);
  /* Optional: drop anything cached for pubkey_hex. */
  void (*forget)(GhInboxResolver *self, const gchar *pubkey_hex);
};

void gh_inbox_resolver_resolve_async(GhInboxResolver *self, const gchar *pubkey_hex,
                                     GCancellable *cancellable,
                                     GAsyncReadyCallback callback, gpointer user_data);
GhInboxResult *gh_inbox_resolver_resolve_finish(GhInboxResolver *self,
                                                GAsyncResult *result, GError **error);
void gh_inbox_resolver_forget(GhInboxResolver *self, const gchar *pubkey_hex);
/* For implementations: emits "changed" for pubkey_hex. */
void gh_inbox_resolver_emit_changed(GhInboxResolver *self, const gchar *pubkey_hex);

G_END_DECLS
#endif
