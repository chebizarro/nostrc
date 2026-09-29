#ifndef GH_NIP17_ENVELOPE_H
#define GH_NIP17_ENVELOPE_H

#include "gh-account-controller.h"

G_BEGIN_DECLS

typedef struct {
  gchar *rumor_json;          /* one canonical unsigned kind-14 rumor */
  gchar *recipient_wrap_json; /* signed kind-1059 event, not published;
                               * NULL for a note to self */
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
/* A note to self: one rumor p-tagged to the active account and exactly one
 * wrap to the account itself (sender_wrap_json; recipient_wrap_json is NULL).
 * A second wrap would carry the same rumor to the same inbox. Two signer
 * operations (NIP-44 encrypt to self, seal signature) instead of four. */
void gh_nip17_envelope_build_self_async(GhAccountController *accounts,
                                         const gchar *content,
                                         GCancellable *cancellable,
                                         GAsyncReadyCallback callback,
                                         gpointer user_data);
/* The canonical unsigned kind-14 rumor of a one-to-one message (a note to
 * self when recipient == sender) created at @created_at (unix seconds, > 0),
 * as compact JSON; @out_rumor_id receives its id. A durable outbox stores it
 * before the first signer call and seals exactly it later. Needs no signer. */
gchar *gh_nip17_rumor_new(const gchar *sender_pubkey_hex,
                          const gchar *recipient_pubkey_hex,
                          const gchar *content, gint64 created_at,
                          gchar **out_rumor_id, GError **error);
/* The recipient (lowercase hex; the sender itself for a note to self) of a
 * canonical unsigned kind-14 rumor authored by @sender_pubkey_hex with
 * exactly one "p" tag, or NULL for anything else. */
gchar *gh_nip17_rumor_get_recipient(const gchar *rumor_json,
                                    const gchar *sender_pubkey_hex);
/* Seals an existing rumor of the active account: a canonical unsigned
 * kind 14 it authored, with exactly one "p" tag. A "p" naming the account is
 * a note to self (one wrap, like build_self); any other names the recipient
 * (a recipient wrap and a self-copy, like build). The rumor text is encrypted
 * byte-for-byte, so every wrap carries exactly the stored rumor id.
 * Anything else fails with G_IO_ERROR_INVALID_ARGUMENT before any signer
 * call. */
void gh_nip17_envelope_seal_async(GhAccountController *accounts,
                                  const gchar *rumor_json,
                                  GCancellable *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data);
/* Finishes any build or seal. */
GhNip17Envelope *gh_nip17_envelope_build_finish(GAsyncResult *result,
                                                  GError **error);
void gh_nip17_envelope_free(GhNip17Envelope *envelope);

G_END_DECLS
#endif
