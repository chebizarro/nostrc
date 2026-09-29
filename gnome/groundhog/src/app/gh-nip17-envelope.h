#ifndef GH_NIP17_ENVELOPE_H
#define GH_NIP17_ENVELOPE_H

#include "gh-account-controller.h"
#include "gh-nip17-file.h"

G_BEGIN_DECLS

/* NIP-40 expirations of the outer layers of a disappearing message (charter
 * §3.7, PT-7). The rumor inside carries the exact expiration; each seal and
 * each gift wrap carries its own value, never earlier than the rumor's, so
 * that no outer layer reveals the real send time (expiration - timer) and
 * the layers of one message do not share a value. gh-expiry.h computes
 * them. */
typedef struct {
  gint64 seal; /* the kind-13 seal */
  gint64 wrap; /* the kind-1059 gift wrap that carries it */
} GhNip17LayerExpiration;

typedef struct {
  GhNip17LayerExpiration recipient; /* the recipient's; unused for a note to self */
  GhNip17LayerExpiration self_copy; /* the sender's own (a note to self's only one) */
} GhNip17OuterExpiration;

/* The most people Groundhog sends one NIP-17 message to besides the sender
 * (NIP-17: rooms beyond 10 participants need another protocol; charter
 * §4.5 S4, §7.9). Equal to GH_CONVERSATION_MAX_PEERS. */
#define GH_NIP17_MAX_SEND_RECIPIENTS 10

/* The outer expirations of a disappearing message to a room (W17): one
 * independently drawn seal and wrap value per recipient, in the rumor's "p"
 * order, and the self-copy's. n_recipients is 0 for a note to self. */
typedef struct {
  guint n_recipients;
  GhNip17LayerExpiration recipients[GH_NIP17_MAX_SEND_RECIPIENTS];
  GhNip17LayerExpiration self_copy;
} GhNip17RoomExpiration;

typedef struct {
  gchar *rumor_json;          /* one canonical unsigned kind-14 rumor */
  gchar *recipient_wrap_json; /* the first recipient's signed kind-1059 wrap,
                               * not published; NULL for a note to self */
  gchar *sender_wrap_json;    /* signed kind-1059 self-copy, not published; NULL
                               * for a Welcome (seal_welcome) */
  /* Every recipient (lowercase hex, the rumor's "p" order, never the sender)
   * and its own wrap, index for index; both empty for a note to self. Each
   * wrap has its own ephemeral key, and no two wraps (nor two seals) of one
   * envelope share a created_at. */
  GStrv recipients;
  GStrv recipient_wraps;
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
/* The same rumor that disappears: it carries ["expiration", "<expires_at>"]
 * (NIP-40), inside the encryption. @expires_at is 0 (none, exactly
 * gh_nip17_rumor_new()) or later than @created_at and at most
 * GH_NIP17_MAX_EXPIRATION. */
gchar *gh_nip17_rumor_new_expiring(const gchar *sender_pubkey_hex,
                                   const gchar *recipient_pubkey_hex,
                                   const gchar *content, gint64 created_at,
                                   gint64 expires_at, gchar **out_rumor_id,
                                   GError **error);
/* The created_at and expiration (0: none) of a canonical unsigned kind-14
 * (or kind-15 file, G21) rumor; FALSE for anything else, including a malformed or repeated
 * expiration tag. Either out pointer may be NULL. */
gboolean gh_nip17_rumor_get_expiration(const gchar *rumor_json, gint64 *out_created_at,
                                       gint64 *out_expires_at);
/* The recipient (lowercase hex; the sender itself for a note to self) of a
 * canonical unsigned kind-14 or kind-15 rumor authored by @sender_pubkey_hex with
 * exactly one "p" tag, or NULL for anything else. */
gchar *gh_nip17_rumor_get_recipient(const gchar *rumor_json,
                                    const gchar *sender_pubkey_hex);

/* The rumor of a message to a NIP-17 room (W17): one "p" tag per recipient,
 * in the given order (NIP-17: the room is the set of pubkeys plus the
 * sender). @recipients holds 1 to GH_NIP17_MAX_SEND_RECIPIENTS distinct hex
 * pubkeys (any case); the sender is a note to self and only alone.
 * @expires_at as for gh_nip17_rumor_new_expiring(); one recipient gives
 * exactly that function's rumor. */
gchar *gh_nip17_rumor_new_room(const gchar *sender_pubkey_hex,
                               const gchar *const *recipients,
                               const gchar *content, gint64 created_at,
                               gint64 expires_at, gchar **out_rumor_id,
                               GError **error);
/* G21: the rumor of a kind-15 file message (gh-nip17-file.h: encrypted,
 * uploaded, URL set) to a NIP-17 room, built exactly as
 * gh_nip17_rumor_new_room() builds a text's (the same recipient rule, "p"
 * tags and expiration) with kind 15, the file's URL as content and the file
 * tags after the "p" tags. One recipient gives gh_nip17_file_rumor_new()'s
 * rumor. G_IO_ERROR_INVALID_ARGUMENT for an incomplete file too. */
gchar *gh_nip17_rumor_new_file_room(const gchar *sender_pubkey_hex,
                                    const gchar *const *recipients,
                                    const GhNip17File *file, gint64 created_at,
                                    gint64 expires_at, gchar **out_rumor_id,
                                    GError **error);
/* Every recipient of a canonical unsigned kind-14 (or kind-15 file) rumor
 * authored by @sender_pubkey_hex (lowercase hex, "p" order): its "p" tags, which must be
 * 1 to GH_NIP17_MAX_SEND_RECIPIENTS distinct valid pubkeys, the sender only
 * alone (a note to self). NULL for anything else. */
GStrv gh_nip17_rumor_dup_recipients(const gchar *rumor_json,
                                    const gchar *sender_pubkey_hex);
/* Seals an existing rumor of the active account: a canonical unsigned
 * kind 14 (or kind-15 file, G21) it authored, with exactly one "p" tag. A "p" naming the account is
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
/* gh_nip17_envelope_seal_async() with the outer expirations of a rumor that
 * expires: each seal carries ["expiration", <its value>] as its only tag and
 * each gift wrap has it beside its "p" tag (NIP-17: on the wrap, and on the
 * seal in case it leaks). @outer is required exactly when the rumor has an
 * expiration, and every value used (for a note to self only self_copy) must
 * be at least the rumor's and at most GH_NIP17_MAX_EXPIRATION; anything else
 * fails with G_IO_ERROR_INVALID_ARGUMENT before any signer call. So
 * gh_nip17_envelope_seal_async() refuses a rumor that expires: it would
 * leave the outer layers without one. */
void gh_nip17_envelope_seal_expiring_async(GhAccountController *accounts,
                                           const gchar *rumor_json,
                                           const GhNip17OuterExpiration *outer,
                                           GCancellable *cancellable,
                                           GAsyncReadyCallback callback,
                                           gpointer user_data);
/* Seals an existing rumor (kind 14, or a kind-15 file: G21) of the active
 * account to every recipient it names (gh_nip17_rumor_dup_recipients()): one
 * seal and one gift wrap per
 * recipient, then the self-copy, each with its own signer approvals (NIP-44
 * encryption, seal signature), its own fresh ephemeral key and its own
 * randomized seal and wrap created_at (NIP-59), none shared with another
 * wrap of the message. A rumor naming only the account is a note to self
 * (one wrap). @outer is required exactly when the rumor expires; its
 * n_recipients must equal the recipient count (0 for a note to self) and
 * every value used obeys gh_nip17_envelope_seal_expiring_async()'s rule.
 * Anything else fails with G_IO_ERROR_INVALID_ARGUMENT before any signer
 * call. */
void gh_nip17_envelope_seal_room_async(GhAccountController *accounts,
                                       const gchar *rumor_json,
                                       const GhNip17RoomExpiration *outer,
                                       GCancellable *cancellable,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data);
/* The kind of a Marmot Welcome rumor (MIP-02). */
#define GH_NIP17_WELCOME_KIND 444
/* nostrc-qp24.13: gift-wraps a Marmot Welcome (MIP-02; the adopted transport
 * profile: a kind-444 rumor in a NIP-59 kind-13 seal in a kind-1059 wrap)
 * to one invitee, exactly as a private message's recipient wrap is made
 * (signer NIP-44 encryption, signer seal signature, a fresh ephemeral wrap
 * key, randomized seal and wrap created_at) but with no self-copy: the
 * Welcome is for the invitee alone. @rumor_json must be a canonical unsigned
 * kind-444 rumor authored by the active account (its "pubkey" set and its
 * "id" matching, since the seal binds the rumor's author) with content, and
 * @recipient_pubkey_hex another person; G_IO_ERROR_INVALID_ARGUMENT before
 * any signer call otherwise. The envelope's recipient_wrap_json (and
 * recipient_wraps[0]) is the wrap; sender_wrap_json is NULL. Finish with
 * gh_nip17_envelope_build_finish(). */
void gh_nip17_envelope_seal_welcome_async(GhAccountController *accounts,
                                          const gchar *rumor_json,
                                          const gchar *recipient_pubkey_hex,
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data);
/* Finishes any build or seal. */
GhNip17Envelope *gh_nip17_envelope_build_finish(GAsyncResult *result,
                                                  GError **error);
void gh_nip17_envelope_free(GhNip17Envelope *envelope);

G_END_DECLS
#endif
