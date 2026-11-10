#ifndef GH_MESSAGE_POLICY_H
#define GH_MESSAGE_POLICY_H

#include "gh-message.h"
typedef struct _GhStore GhStore;

G_BEGIN_DECLS

typedef enum { GH_MESSAGE_INCLUDE, GH_MESSAGE_HIDE_RETAIN, GH_MESSAGE_DROP } GhMessageDisposition;

typedef struct _GhConversationStore GhConversationStore;
#define GH_TYPE_MESSAGE_POLICY (gh_message_policy_get_type())
G_DECLARE_FINAL_TYPE(GhMessagePolicy, gh_message_policy, GH, MESSAGE_POLICY, GObject)

/* Owner main context only. A new policy is deliberately not loaded. */
GhMessagePolicy *gh_message_policy_new(const gchar *account);
/* Storage is borrowed until close(). Initial projection finishes before restore. */
gboolean gh_message_policy_load(GhMessagePolicy *self, GhStore *store, GError **error);
void gh_message_policy_set_model(GhMessagePolicy *self, GhConversationStore *model);
void gh_message_policy_close(GhMessagePolicy *self);
/* Called after the kind-10000 transaction. p-only diffs use indexed SQL, never
 * mask or scan plaintext. Content diffs run cancellable 256-row idle batches. */
gboolean gh_message_policy_refresh(GhMessagePolicy *self, GError **error);
gboolean gh_message_policy_classify(GhMessagePolicy *self, GhMessage *message,
                                    GhMessageDisposition *out, GError **error);
gboolean gh_message_policy_hides_author(GhMessagePolicy *self, const gchar *pubkey);
gboolean gh_message_policy_is_loaded(GhMessagePolicy *self);
gboolean gh_message_policy_is_updating(GhMessagePolicy *self);
const gchar *gh_message_policy_get_error(GhMessagePolicy *self);
/* Track authenticated thread edges only after a successful retained commit. */
void gh_message_policy_committed(GhMessagePolicy *self, GhMessage *message);
/* Signals: changed (snapshot installed; recheck notifications), projected
 * (visible models reset; never message-added), failed (projection stays masked). */

G_END_DECLS
#endif
