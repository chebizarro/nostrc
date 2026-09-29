#ifndef GH_NIP17_INBOX_H
#define GH_NIP17_INBOX_H

#include "gh-account-controller.h"
#include "gh-nip17-file.h"

G_BEGIN_DECLS

/* Size bounds applied before any parsing or signer call. The plaintext bound
 * is the NIP-44 v2 maximum, so it covers both the seal and the rumor. */
#define GH_NIP17_MAX_WRAP_JSON   (128 * 1024)
#define GH_NIP17_MAX_PLAINTEXT   65535
#define GH_NIP17_MAX_RECIPIENTS  128
/* Largest NIP-40 `expiration` accepted: 9999-12-31T23:59:59Z. A value must be
 * canonical decimal unix seconds in [1, this]: digits only, no sign, no
 * leading zero. */
#define GH_NIP17_MAX_EXPIRATION  G_GINT64_CONSTANT(253402300799)

typedef enum {
  GH_NIP17_INBOX_ERROR_TOO_LARGE = 1,
  GH_NIP17_INBOX_ERROR_INVALID_WRAP,     /* parse, kind, id, signature or expiration */
  GH_NIP17_INBOX_ERROR_WRONG_RECIPIENT,  /* p tag is not the active account */
  GH_NIP17_INBOX_ERROR_INVALID_SEAL,     /* parse, kind, id, signature or tags */
  GH_NIP17_INBOX_ERROR_INVALID_RUMOR,    /* parse, signed, id, p or expiration tags */
  GH_NIP17_INBOX_ERROR_UNSUPPORTED_KIND, /* rumor is not kind 14 or 15, or a kind 15
                                          * not encrypted with aes-gcm */
  GH_NIP17_INBOX_ERROR_SENDER_MISMATCH   /* rumor.pubkey != seal.pubkey */
} GhNip17InboxError;
#define GH_NIP17_INBOX_ERROR gh_nip17_inbox_error_quark()
GQuark gh_nip17_inbox_error_quark(void);

/* A fully validated inbound kind-14 (or kind-15 file, G21) message. Every field was derived from
 * checked data; nothing here was taken from an unverified id field. */
typedef struct {
  gchar *account_pubkey; /* hex; the account this wrap was addressed to */
  gchar *wrap_id;        /* verified kind-1059 id (per-copy dedup key) */
  gchar *rumor_id;       /* canonical rumor id (per-message dedup key) */
  gchar *rumor_json;     /* canonical re-serialization, id included, no sig */
  gchar *sender_pubkey;  /* hex; seal signer == rumor author */
  gchar **recipients;    /* hex, lowercase, unique, in p-tag order */
  gint64 created_at;     /* rumor created_at (the seal/wrap ones are random) */
  gboolean self_copy;    /* sender_pubkey == account_pubkey */
  /* NIP-40 `expiration` of each layer in unix seconds, 0 when absent. NIP-17
   * puts it on each wrap and says the seal SHOULD carry it too; a rumor may
   * carry it as an ordinary tag. The rumor and seal values are authenticated
   * by the seal signature. The wrap value is signed only by a throwaway key,
   * so anyone who obtains the seal can rewrap it with any wrap expiration;
   * it may also be deliberately later than the real one (jittered). */
  gint64 rumor_expiration;
  gint64 seal_expiration;
  gint64 wrap_expiration;
  /* rumor, else seal, else wrap expiration (most to least authenticated);
   * 0 means the message does not expire. It may already be in the past:
   * the unwrap does not drop expired messages, so the caller can still
   * record them as seen without storing them. */
  gint64 expires_at;
} GhNip17Message;

void gh_nip17_message_free(GhNip17Message *message);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhNip17Message, gh_nip17_message_free)

/* Unwraps one inbound kind-1059 gift wrap for the active account using only
 * the account's external signer; the app never holds a secret key.
 *
 *   1. wrap: bounded, strictly parsed, kind 1059, id and signature verified,
 *      exactly one p tag and it equals the active account pubkey, at most one
 *      well-formed expiration tag;
 *   2. signer NIP-44 decrypt(wrap.content, wrap.pubkey) -> seal;
 *   3. seal: bounded, kind 13, id and signature verified, and no tags except
 *      at most one well-formed expiration tag (NIP-17 disappearing messages;
 *      any other seal tag or a second expiration is INVALID_SEAL, and it is
 *      rejected before the second signer call);
 *   4. signer NIP-44 decrypt(seal.content, seal.pubkey) -> rumor;
 *   5. rumor: bounded, unsigned, kind 14, or kind 15 with valid file tags
 *      (gh-nip17-file.h; G21: INVALID_RUMOR otherwise), other kinds rejected,
 *      rumor.pubkey == seal.pubkey, canonical id (a declared id must match),
 *      lowercase hex p tags, the account is the sender or a recipient, and
 *      at most one well-formed expiration tag.
 *
 * A well-formed expiration tag is exactly ["expiration", "<seconds>"] with
 * the value bounded as described at GH_NIP17_MAX_EXPIRATION.
 *
 * The account generation and the caller's cancellable are checked before and
 * after every signer call and again in _finish. Work for a switched or
 * disposed account completes with G_IO_ERROR_CANCELLED and yields nothing;
 * cancelling revokes a pending signer approval. Signer errors (e.g. denial)
 * are returned unchanged. No state is stored by this call.
 *
 * A GH_NIP17_INBOX_ERROR is a final verdict on the wrap's content: the same
 * wrap is rejected the same way every time. Every other error (signer
 * denial, timeout or outage, cancellation) is transient. */
void gh_nip17_unwrap_async(GhAccountController *accounts,
                           const gchar *wrap_json,
                           GCancellable *cancellable,
                           GAsyncReadyCallback callback,
                           gpointer user_data);
GhNip17Message *gh_nip17_unwrap_finish(GAsyncResult *result, GError **error);
/* The signer calls (NIP-44 decrypts) the unwrap behind @result started: 0
 * when it was rejected by the wrap checks of step 1, else 1 or 2. Readable
 * from the callback, before or after _finish. A final rejection with a
 * nonzero count cost the user a signer approval; record it with
 * gh_nip17_seen_record_rejected() so no later session asks again. */
guint gh_nip17_unwrap_get_signer_calls(GAsyncResult *result);

/* Restart-safe seen-set for one account. Keys:
 *   - wrap id: one relay copy of one delivery. Check it before unwrapping to
 *     avoid a repeated signer prompt. Only ids that passed full validation are
 *     recorded, so checking an unverified declared id is safe: a forgery that
 *     claims a recorded id would have failed validation anyway.
 *   - rumor id: the message itself, which a sender may re-wrap. The canonical
 *     id commits to the verified sender, so it cannot be claimed by another.
 *   - rejected wrap id: a wrap whose outer event verified but which was finally
 *     rejected (a GH_NIP17_INBOX_ERROR) after a signer call. It is skipped
 *     before any signer call, like a seen wrap, but is never a seen message.
 *     Its id is the verified hash of the event, so no other event can claim
 *     it; transient failures (denial, outage, cancellation) are never recorded.
 * File format: a header line, then one "w <id>", "r <id>" or "x <id>" line
 * per key ("x" is rejected). Files written before "x" existed load unchanged;
 * a build without "x" refuses a file holding one (fail closed). The three
 * namespaces share the capacity. The encrypted store (G05) must carry the
 * rejected namespace into its seen table.
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
gboolean gh_nip17_seen_has_rejected(GhNip17Seen *seen, const gchar *wrap_id);
/* Records a finally rejected wrap id (lowercase hex), durably like _record. */
gboolean gh_nip17_seen_record_rejected(GhNip17Seen *seen, const gchar *wrap_id,
                                       GError **error);
/* Moves only the rejected keys ("x" lines) of the seen file at @path, which
 * must be the same account's, into @seen, then deletes the file; its "w" and
 * "r" keys are dropped. A file written by an inbox that kept its messages in
 * memory only must never make those messages count as seen: the relays
 * still hold them, and they are fetched again (W13 review B1). A missing file
 * is success with nothing moved. A foreign, malformed or oversized file is
 * refused and left in place. *out_moved (nullable): rejected keys new to
 * @seen. */
gboolean gh_nip17_seen_import_rejected(GhNip17Seen *seen, const gchar *path,
                                       guint *out_moved, GError **error);
/* "<acct>.seen", the pseudonymous file name of an account's seen file
 * (charter §3.2): <acct> = hex(SHA-256("groundhog/v1/account-dir" ||
 * pubkey))[0:32], the same name as the account's store directory. NULL for
 * anything but a lowercase 64-hex pubkey. */
gchar *gh_nip17_seen_file_name(const gchar *account_pubkey_hex);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhNip17Seen, gh_nip17_seen_free)

G_END_DECLS
#endif
