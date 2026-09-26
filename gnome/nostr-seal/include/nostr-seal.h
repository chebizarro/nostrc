/*
 * nostr-seal.h — seal files for Nostr public keys (or a passphrase).
 * SPDX-License-Identifier: MIT
 *
 * Bead nostrc-da9c. The byte layout is specified in
 * gnome/nostr-seal/README.md ("Format"); read that first.
 *
 * NIP-44 v2 is a message cipher (65535-byte cap, no streaming), so it is
 * used only for what it is good at — wrapping a random 32-byte file key per
 * recipient under the NIP-44 conversation key. The file body is the
 * porthome chunked AEAD container (docs/designs/porthome-crypto-spec.md,
 * nh_porthome_crypto.h) rooted at that file key.
 *
 * All entry points are synchronous and stream through file descriptors:
 * memory is bounded by two chunks plus 32 bytes of index per chunk.
 */

#ifndef NOSTR_SEAL_H
#define NOSTR_SEAL_H

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

#define NSEAL_MAGIC            "NSEALED"      /* 7 bytes, then version */
#define NSEAL_FORMAT_VERSION   0x01
#define NSEAL_FILE_KEY_LEN     32
#define NSEAL_PUBKEY_LEN       32

#define NSEAL_CHUNK_LOG2_MIN      12   /* 4 KiB */
#define NSEAL_CHUNK_LOG2_MAX      24   /* 16 MiB */
#define NSEAL_CHUNK_LOG2_DEFAULT  20   /* 1 MiB */

#define NSEAL_MAX_STANZAS      1024

/* NIP-49 scrypt work factor. 16 matches the nip49-keyencrypt plugin
 * default; decrypt refuses anything above NSEAL_LOG_N_MAX before running
 * scrypt (scrypt memory is 1 KiB * 2^log_n: 20 → 1 GiB; a hostile file
 * must not be able to demand more). */
#define NSEAL_LOG_N_DEFAULT    16
#define NSEAL_LOG_N_MIN        16
#define NSEAL_LOG_N_MAX        20

#define NSEAL_SUFFIX           ".nsealed"
#define NSEAL_MIME_TYPE        "application/vnd.nostr.sealed"

#define NSEAL_ERROR (nseal_error_quark())
GQuark nseal_error_quark(void);

typedef enum {
  NSEAL_ERROR_ARG,             /* caller misuse / bad option */
  NSEAL_ERROR_IO,              /* read/write failure, short file */
  NSEAL_ERROR_FORMAT,          /* not a .nsealed file / malformed header */
  NSEAL_ERROR_UNSUPPORTED,     /* unknown version, flag or stanza type */
  NSEAL_ERROR_NOT_A_RECIPIENT, /* no stanza for the offered identity */
  NSEAL_ERROR_UNWRAP,          /* stanza present but key unwrap failed
                                * (wrong passphrase / wrong key) */
  NSEAL_ERROR_INTEGRITY,       /* tampered, truncated or reordered body */
  NSEAL_ERROR_CRYPTO,          /* primitive failure (RNG, OOM, ...) */
  NSEAL_ERROR_SIGNER,          /* signer daemon unavailable / refused */
} NsealError;

typedef enum {
  NSEAL_STANZA_NIP44  = 0x01,  /* recipient pubkey + ephemeral pubkey + NIP-44 v2 payload */
  NSEAL_STANZA_NIP49  = 0x02,  /* ncryptsec1 (NIP-49) of the file key */
} NsealStanzaType;

/* ─── Encrypt ─────────────────────────────────────────────────────────── */

typedef struct {
  /* x-only secp256k1 public keys. Either recipients or passphrase, not
   * both (a passphrase file must not be openable by anyone else). */
  const uint8_t (*recipients)[NSEAL_PUBKEY_LEN];
  gsize n_recipients;
  const char *passphrase;      /* UTF-8; NFKC-normalised per NIP-49 */
  guint8 log_n;                /* 0 → NSEAL_LOG_N_DEFAULT */
  guint8 chunk_log2;           /* 0 → NSEAL_CHUNK_LOG2_DEFAULT */
} NsealEncryptOptions;

/* Read plaintext from in_fd until EOF (pipes fine) and write the sealed
 * file to out_fd (pipes fine; only sequential writes). */
gboolean nseal_encrypt_fd(int in_fd, int out_fd,
                          const NsealEncryptOptions *opts,
                          GError **error);

/* ─── Header inspection ───────────────────────────────────────────────── */

typedef struct _NsealHeader NsealHeader;

/* Parse and strictly validate the header at offset 0 of a seekable fd. */
NsealHeader *nseal_header_read_fd(int fd, GError **error);
void         nseal_header_free(NsealHeader *h);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(NsealHeader, nseal_header_free)

guint           nseal_header_chunk_log2(const NsealHeader *h);
gsize           nseal_header_n_stanzas(const NsealHeader *h);
NsealStanzaType nseal_header_stanza_type(const NsealHeader *h, gsize i);
/* NIP44 stanzas only; NULL otherwise. */
const uint8_t  *nseal_header_stanza_recipient(const NsealHeader *h, gsize i);
gboolean        nseal_header_is_passphrase(const NsealHeader *h);
gboolean        nseal_header_has_recipient(const NsealHeader *h,
                                           const uint8_t pubkey[NSEAL_PUBKEY_LEN]);

/* ─── Decrypt ─────────────────────────────────────────────────────────── */

/* Recover the file key from one NIP-44 stanza: the conversation key is
 * convkey(identity_sk, ephemeral_pk); `payload` is the stanza's NIP-44 v2
 * payload (canonical base64). Implementations: nseal_unwrap_with_seckey
 * (in-process key; tests) and the signer daemon lane in nostr-seal-signer.h. */
typedef gboolean (*NsealUnwrapFunc)(gpointer user_data,
                                    const uint8_t ephemeral_pk[NSEAL_PUBKEY_LEN],
                                    const char *payload,
                                    uint8_t out_file_key[NSEAL_FILE_KEY_LEN],
                                    GError **error);

typedef struct {
  /* Recipient lane: identity is matched against the stanza list. */
  const uint8_t *identity_pubkey;   /* 32 bytes, or NULL */
  NsealUnwrapFunc unwrap;
  gpointer unwrap_data;
  /* Passphrase lane. */
  const char *passphrase;
  guint8 max_log_n;                 /* 0 → NSEAL_LOG_N_MAX */
} NsealDecryptOptions;

/* in_fd must be seekable (the chunk index sits at the end). Plaintext is
 * written to out_fd only after the index authenticated and each chunk's
 * address matched it, so every byte written is authentic and in order; a
 * failure part-way (tamper/truncation) returns FALSE and the caller must
 * discard what was written (the CLI writes to a temp file and renames). */
gboolean nseal_decrypt_fd(int in_fd, int out_fd,
                          const NsealDecryptOptions *opts,
                          GError **error);

/* In-process unwrap for a raw secret key (user_data = const uint8_t[32]). */
gboolean nseal_unwrap_with_seckey(gpointer seckey32,
                                  const uint8_t ephemeral_pk[NSEAL_PUBKEY_LEN],
                                  const char *payload,
                                  uint8_t out_file_key[NSEAL_FILE_KEY_LEN],
                                  GError **error);

/* ─── Helpers ─────────────────────────────────────────────────────────── */

/* npub1… or 64-hex → 32-byte x-only key (validated on the curve). */
gboolean nseal_parse_pubkey(const char *text, uint8_t out[NSEAL_PUBKEY_LEN], GError **error);
/* 32-byte key → newly allocated npub1… */
char    *nseal_pubkey_to_npub(const uint8_t pk[NSEAL_PUBKEY_LEN]);

G_END_DECLS

#endif /* NOSTR_SEAL_H */
