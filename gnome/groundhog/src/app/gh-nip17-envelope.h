#ifndef GH_NIP17_ENVELOPE_H
#define GH_NIP17_ENVELOPE_H

#include "gh-account-controller.h"

G_BEGIN_DECLS

typedef struct {
  gchar *rumor_json;          /* one canonical unsigned kind-14 rumor */
  gchar *recipient_wrap_json; /* signed kind-1059 event, not published */
  gchar *sender_wrap_json;    /* signed kind-1059 self-copy, not published */
} GhNip17Envelope;

/* Creates both outbound envelopes for the active account. Encryption and
 * seal signing use the selected external signer. Cancellation revokes a
 * pending approval. This codec performs no relay lookup or publication. */
void gh_nip17_envelope_build_async(GhAccountController *accounts,
                                    const gchar *recipient_pubkey_hex,
                                    const gchar *content,
                                    GCancellable *cancellable,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data);
GhNip17Envelope *gh_nip17_envelope_build_finish(GAsyncResult *result,
                                                  GError **error);
void gh_nip17_envelope_free(GhNip17Envelope *envelope);

G_END_DECLS
#endif
