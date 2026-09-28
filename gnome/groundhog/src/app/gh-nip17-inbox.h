#ifndef GH_NIP17_INBOX_H
#define GH_NIP17_INBOX_H

#include "gh-account-controller.h"

G_BEGIN_DECLS

/* Size bounds applied before any parsing or signer call. The plaintext bound
 * is the NIP-44 v2 maximum, so it covers both the seal and the rumor. */
#define GH_NIP17_MAX_WRAP_JSON   (128 * 1024)
#define GH_NIP17_MAX_PLAINTEXT   65535
#define GH_NIP17_MAX_RECIPIENTS  128

typedef enum {
  GH_NIP17_INBOX_ERROR_TOO_LARGE = 1,
  GH_NIP17_INBOX_ERROR_INVALID_WRAP,     /* parse, kind, id or signature */
  GH_NIP17_INBOX_ERROR_WRONG_RECIPIENT,  /* p tag is not the active account */
  GH_NIP17_INBOX_ERROR_INVALID_SEAL,     /* parse, kind, id, signature or tags */
  GH_NIP17_INBOX_ERROR_INVALID_RUMOR,    /* parse, signed, id or p tags */
  GH_NIP17_INBOX_ERROR_UNSUPPORTED_KIND, /* rumor is not kind 14 */
  GH_NIP17_INBOX_ERROR_SENDER_MISMATCH   /* rumor.pubkey != seal.pubkey */
} GhNip17InboxError;
#define GH_NIP17_INBOX_ERROR gh_nip17_inbox_error_quark()
GQuark gh_nip17_inbox_error_quark(void);

/* A fully validated inbound kind-14 message. Every field was derived from
 * checked data; nothing here was taken from an unverified id field. */
typedef struct {
  gchar *account_pubkey; /* hex; the account this wrap was addressed to */
  gchar *wrap_id;        /* verified kind-1059 id (per-copy dedup key) */
  gchar *rumor_id;       /* canonical kind-14 id (per-message dedup key) */
  gchar *rumor_json;     /* canonical re-serialization, id included, no sig */
  gchar *sender_pubkey;  /* hex; seal signer == rumor author */
  gchar **recipients;    /* hex, lowercase, unique, in p-tag order */
  gint64 created_at;     /* rumor created_at (the seal/wrap ones are random) */
  gboolean self_copy;    /* sender_pubkey == account_pubkey */
} GhNip17Message;

void gh_nip17_message_free(GhNip17Message *message);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhNip17Message, gh_nip17_message_free)

/* Unwraps one inbound kind-1059 gift wrap for the active account using only
 * the account's external signer; the app never holds a secret key.
 *
 *   1. wrap: bounded, strictly parsed, kind 1059, id and signature verified,
 *      exactly one p tag and it equals the active account pubkey;
 *   2. signer NIP-44 decrypt(wrap.content, wrap.pubkey) -> seal;
 *   3. seal: bounded, kind 13, id and signature verified, no tags;
 *   4. signer NIP-44 decrypt(seal.content, seal.pubkey) -> rumor;
 *   5. rumor: bounded, unsigned, kind 14 only (15 and others are rejected),
 *      rumor.pubkey == seal.pubkey, canonical id (a declared id must match),
 *      lowercase hex p tags, and the account is the sender or a recipient.
 *
 * The account generation and the caller's cancellable are checked before and
 * after every signer call and again in _finish. Work for a switched or
 * disposed account completes with G_IO_ERROR_CANCELLED and yields nothing;
 * cancelling revokes a pending signer approval. Signer errors (e.g. denial)
 * are returned unchanged. No state is stored by this call. */
void gh_nip17_unwrap_async(GhAccountController *accounts,
                           const gchar *wrap_json,
                           GCancellable *cancellable,
                           GAsyncReadyCallback callback,
                           gpointer user_data);
GhNip17Message *gh_nip17_unwrap_finish(GAsyncResult *result, GError **error);

/* Restart-safe seen-set for one account. Keys:
 *   - wrap id: one relay copy of one delivery. Check it before unwrapping to
 *     avoid a repeated signer prompt. Only ids that passed full validation are
 *     recorded, so checking an unverified declared id is safe: a forgery that
 *     claims a recorded id would have failed validation anyway.
 *   - rumor id: the message itself, which a sender may re-wrap. The canonical
 *     id commits to the verified sender, so it cannot be claimed by another.
 * Record a message only after it is durably stored; the store must still be
 * idempotent on rumor id because a crash can fall between the two.
 * The file is bound to the account pubkey and holds at most capacity entries
 * (oldest evicted); reopen it with the same or a larger capacity. A foreign,
 * oversized or malformed file is refused rather than partly trusted; a torn
 * final append is ignored and dropped by the next record's rewrite. The file
 * is created and kept owner-only (0600). This is not conversation storage. */
typedef struct _GhNip17Seen GhNip17Seen;

GhNip17Seen *gh_nip17_seen_open(const gchar *path, const gchar *account_pubkey_hex,
                                guint capacity, GError **error);
void gh_nip17_seen_free(GhNip17Seen *seen);
gboolean gh_nip17_seen_has_wrap(GhNip17Seen *seen, const gchar *wrap_id);
gboolean gh_nip17_seen_has_rumor(GhNip17Seen *seen, const gchar *rumor_id);
/* Records both keys (durably appended, or atomically rewritten when the log
 * is compacted). Fails for another account's message or on I/O error; after
 * an I/O error the keys stay seen in memory for this process only. */
gboolean gh_nip17_seen_record(GhNip17Seen *seen, const GhNip17Message *message,
                              GError **error);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhNip17Seen, gh_nip17_seen_free)

G_END_DECLS
#endif
