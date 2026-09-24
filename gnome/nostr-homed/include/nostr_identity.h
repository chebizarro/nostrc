#ifndef NOSTR_IDENTITY_H
#define NOSTR_IDENTITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Shared identity-authority contract for nostr-authd and the read-only NSS
 * projection.  This is an in-tree API, not a wire format and not an installed
 * plugin ABI.  Do not serialize these structs or enum representations.
 */

/* Schema v2 (2026-09-23): providers.wrapped_home_key BLOB column added
 * for portable-home NIP-46 signer key handoff (design §4.3, bead
 * nostrc-pvha). NULL for existing rows after migration; populated on
 * first successful nip44_encrypt of a fresh wrap seed. */
#define NH_IDENTITY_AUTHORITY_SCHEMA_VERSION 2u
#define NH_IDENTITY_PROJECTION_SCHEMA_VERSION 1u
#define NH_IDENTITY_PROJECTION_APPLICATION_ID 0x4e485031u /* "NHP1" */

#define NH_IDENTITY_UUID_LEN 36u
#define NH_IDENTITY_UUID_CAP (NH_IDENTITY_UUID_LEN + 1u)
#define NH_IDENTITY_USERNAME_MAX 32u
#define NH_IDENTITY_USERNAME_CAP (NH_IDENTITY_USERNAME_MAX + 1u)
#define NH_IDENTITY_PUBKEY_HEX_LEN 64u
#define NH_IDENTITY_PUBKEY_HEX_CAP (NH_IDENTITY_PUBKEY_HEX_LEN + 1u)
#define NH_IDENTITY_HOME_MAX 255u
#define NH_IDENTITY_HOME_CAP (NH_IDENTITY_HOME_MAX + 1u)
#define NH_IDENTITY_SHELL_MAX 127u
#define NH_IDENTITY_SHELL_CAP (NH_IDENTITY_SHELL_MAX + 1u)
#define NH_IDENTITY_PROVIDER_CONFIG_MAX 4096u
#define NH_IDENTITY_PROVIDER_CONFIG_CAP (NH_IDENTITY_PROVIDER_CONFIG_MAX + 1u)
#define NH_IDENTITY_PROVIDER_SECRET_MAX 4096u
/* Wrapped-home-key ciphertext (NIP-44 v2 base64) stored on the provider
 * record. NIP-44 v2 caps plaintext at 65535 B; a 32-byte plaintext seals
 * to ~130 base64 chars, well under this cap. The upper bound is
 * defensive against a provider that returns something too long. */
#define NH_IDENTITY_WRAPPED_HOME_KEY_MAX 512u
#define NH_IDENTITY_READER_BUF_MAX 512u

#define NH_IDENTITY_DEFAULT_UID_MIN 200000u
#define NH_IDENTITY_DEFAULT_UID_MAX 299999u
#define NH_IDENTITY_DEFAULT_AUTHORITY_PATH \
  "/var/lib/nostr-auth/private/authority.db"
#define NH_IDENTITY_DEFAULT_PROJECTION_PATH "/var/lib/nostr-auth/nss.db"
#define NH_IDENTITY_DEFAULT_HOME_ROOT "/home"
#define NH_IDENTITY_DEFAULT_SHELL "/bin/bash"

/* Store and administrative results. Values are stable and never persisted. */
typedef enum nh_identity_rc {
  NH_IDENTITY_OK = 0,
  NH_IDENTITY_NOT_FOUND = 1,
  NH_IDENTITY_INVALID = 2,
  NH_IDENTITY_CONFLICT = 3,
  NH_IDENTITY_OPERATION_MISMATCH = 4,
  NH_IDENTITY_BAD_STATE = 5,
  NH_IDENTITY_STALE_GENERATION = 6,
  NH_IDENTITY_NOT_ACTIVE = 7,
  NH_IDENTITY_RANGE_EXHAUSTED = 8,
  NH_IDENTITY_BUSY = 9,
  NH_IDENTITY_NOT_INITIALIZED = 10,
  NH_IDENTITY_SCHEMA_UNSUPPORTED = 11,
  NH_IDENTITY_STORAGE_ERROR = 12,
  NH_IDENTITY_OWNERSHIP_CHECK_FAILED = 13,
  NH_IDENTITY_NO_MEMORY = 14,
  NH_IDENTITY_UNSUPPORTED = 15,
  NH_IDENTITY_PERMISSION_DENIED = 16
} nh_identity_rc;

/* Exact projection-reader results. NSS maps these without guessing. */
typedef enum nh_identity_reader_result {
  NH_IDENTITY_READER_FOUND = 0,
  NH_IDENTITY_READER_NOT_FOUND = 1,
  NH_IDENTITY_READER_UNAVAILABLE = 2,
  NH_IDENTITY_READER_TOO_SMALL = 3
} nh_identity_reader_result;

typedef enum nh_identity_status {
  NH_IDENTITY_STATUS_ENROLLING = 1,
  NH_IDENTITY_STATUS_ACTIVE = 2,
  NH_IDENTITY_STATUS_DISABLED = 3,
  NH_IDENTITY_STATUS_REPAIR_REQUIRED = 4,
  NH_IDENTITY_STATUS_RETIRED = 5
} nh_identity_status;

typedef enum nh_identity_origin {
  NH_IDENTITY_ORIGIN_ENROLLED = 1,
  NH_IDENTITY_ORIGIN_LEGACY_IMPORT = 2
} nh_identity_origin;

typedef enum nh_identity_provider_type {
  NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY = 1,
  NH_IDENTITY_PROVIDER_NIP46_BUNKER = 2,
  /* NIP-46 QR / nostrconnect:// client-initiated pairing at the greeter
   * (design nip46-qr-login-greeter.md §5, decision D1). Record carries a
   * relay list in public_config_json only; secret_blob is unused (the
   * client keypair is ephemeral per login). */
  NH_IDENTITY_PROVIDER_NIP46_QR = 3
} nh_identity_provider_type;

#define NH_IDENTITY_PROVIDER_BIT(type_) (1u << (unsigned int)(type_))

typedef enum nh_identity_operation_type {
  NH_IDENTITY_OPERATION_ENROLL = 1,
  NH_IDENTITY_OPERATION_IMPORT = 2,
  NH_IDENTITY_OPERATION_REPAIR_HOME = 3,
  NH_IDENTITY_OPERATION_PROVIDER_STAGE = 4,
  NH_IDENTITY_OPERATION_PROVIDER_ACTIVATE = 5,
  NH_IDENTITY_OPERATION_PROVIDER_DISCARD = 6,
  NH_IDENTITY_OPERATION_SET_STATUS = 7,
  NH_IDENTITY_OPERATION_REPLACE_IDENTITY = 8,
  NH_IDENTITY_OPERATION_PROVIDER_RESEAL = 9
} nh_identity_operation_type;

typedef enum nh_identity_operation_phase {
  NH_IDENTITY_PHASE_RESERVED = 1,
  NH_IDENTITY_PHASE_STAGED = 2,
  NH_IDENTITY_PHASE_INSTALLED = 3,
  NH_IDENTITY_PHASE_PROJECTED = 4,
  NH_IDENTITY_PHASE_COMPLETE = 5
} nh_identity_operation_phase;

typedef enum nh_identity_operation_outcome {
  NH_IDENTITY_OUTCOME_PENDING = 1,
  NH_IDENTITY_OUTCOME_DONE = 2,
  NH_IDENTITY_OUTCOME_REPAIR_REQUIRED = 3,
  NH_IDENTITY_OUTCOME_ABANDONED = 4
} nh_identity_operation_outcome;

typedef enum nh_identity_home_mode {
  NH_IDENTITY_HOME_CREATE = 1,
  NH_IDENTITY_HOME_ADOPT_EXISTING = 2
} nh_identity_home_mode;

typedef enum nh_identity_ownership_result {
  NH_IDENTITY_OWNERSHIP_FREE = 0,
  NH_IDENTITY_OWNERSHIP_IN_USE = 1,
  NH_IDENTITY_OWNERSHIP_ERROR = 2
} nh_identity_ownership_result;

/* Opaque handles: no SQLite or GLib types cross this boundary. */
typedef struct nh_identity_store nh_identity_store;
typedef struct nh_identity_reader nh_identity_reader;

/* Identity-specific configuration. General auth configuration belongs to B. */
typedef struct nh_identity_config {
  char authority_path[NH_IDENTITY_HOME_CAP];
  char projection_path[NH_IDENTITY_HOME_CAP];
  char home_root[NH_IDENTITY_HOME_CAP];
  char default_shell[NH_IDENTITY_SHELL_CAP];
  uint32_t uid_min;
  uint32_t uid_max;
  uint32_t domain_default_min;
  uint32_t domain_default_max;
  uint32_t domain_rid_min;
  uint32_t domain_rid_max;
  uint32_t standalone_smb_min;
  uint32_t standalone_smb_max;
} nh_identity_config;

void nh_identity_config_defaults(nh_identity_config *out);
nh_identity_rc nh_identity_config_load(const char *path,
                                       nh_identity_config *out);
nh_identity_rc nh_identity_config_validate(const nh_identity_config *config);

typedef nh_identity_ownership_result (*nh_identity_ownership_probe_fn)(
    void *context, const char *username, uint32_t uid, uint32_t gid);

#define NH_IDENTITY_STORE_CREATE 0x00000001u

typedef struct nh_identity_store_options {
  const nh_identity_config *config;
  nh_identity_ownership_probe_fn ownership_probe;
  void *ownership_probe_context;
  uint32_t flags;
} nh_identity_store_options;

typedef struct nh_identity_store_info {
  char authority_id[NH_IDENTITY_UUID_CAP];
  uint64_t authority_generation;
  uint64_t projection_generation;
  uint32_t schema_version;
} nh_identity_store_info;

/*
 * Authority record returned to the broker. All statuses are visible here.
 * Projection records intentionally use different types and contain no status,
 * account ID, key, provider, or readiness data.
 */
typedef struct nh_identity_account {
  char account_id[NH_IDENTITY_UUID_CAP];
  char username[NH_IDENTITY_USERNAME_CAP];
  char home[NH_IDENTITY_HOME_CAP];
  char shell[NH_IDENTITY_SHELL_CAP];
  char pubkey_hex[NH_IDENTITY_PUBKEY_HEX_CAP];
  uint32_t uid;
  uint32_t gid;
  nh_identity_status status;
  nh_identity_origin origin;
  uint64_t key_generation;
  uint64_t authority_generation;
  uint32_t enabled_providers;
  bool projectable;
} nh_identity_account;

typedef struct nh_identity_provider_record {
  char provider_id[NH_IDENTITY_UUID_CAP];
  char account_id[NH_IDENTITY_UUID_CAP];
  nh_identity_provider_type type;
  bool enabled;
  uint32_t format_version;
  char public_config_json[NH_IDENTITY_PROVIDER_CONFIG_CAP];
  uint8_t secret_blob[NH_IDENTITY_PROVIDER_SECRET_MAX];
  size_t secret_blob_len;
  /* Portable-home NIP-46 wrapped_home_key ciphertext. Empty (len==0)
   * when unset (default for accounts that never went through the
   * porthome enrollment flow, and for local vault providers). Public
   * metadata — not a secret; the plaintext is the 32-byte wrap seed and
   * only the signer can decrypt it via nip44_decrypt (design §4.3). */
  uint8_t wrapped_home_key[NH_IDENTITY_WRAPPED_HOME_KEY_MAX];
  size_t wrapped_home_key_len;
} nh_identity_provider_record;

/* Store lifecycle. Open holds the sole-writer lock until close. */
nh_identity_rc nh_identity_store_open(const nh_identity_store_options *options,
                                      nh_identity_store **out);
void nh_identity_store_close(nh_identity_store *store);
nh_identity_rc nh_identity_store_get_info(nh_identity_store *store,
                                          nh_identity_store_info *out);
const char *nh_identity_store_error_detail(const nh_identity_store *store);
const char *nh_identity_rc_name(nh_identity_rc result);

/* Stable broker read tier. These functions read authority.db only. */
nh_identity_rc nh_identity_store_lookup_by_name(nh_identity_store *store,
                                                const char *username,
                                                nh_identity_account *out);
nh_identity_rc nh_identity_store_lookup_by_uid(nh_identity_store *store,
                                               uint32_t uid,
                                               nh_identity_account *out);
nh_identity_rc nh_identity_store_lookup_by_id(nh_identity_store *store,
                                              const char *account_id,
                                              nh_identity_account *out);
nh_identity_rc nh_identity_store_lookup_by_pubkey(nh_identity_store *store,
                                                  const char *pubkey_hex,
                                                  nh_identity_account *out);

/*
 * Returns OK only for an active account whose key and authority generations
 * still match. status_out/current_out are optional and are filled when the
 * account exists, including on NOT_ACTIVE or STALE_GENERATION.
 */
nh_identity_rc nh_identity_store_recheck(
    nh_identity_store *store, const char *account_id,
    uint64_t expected_key_generation,
    uint64_t expected_authority_generation,
    nh_identity_status *status_out, nh_identity_account *current_out);

nh_identity_rc nh_identity_store_provider_get(
    nh_identity_store *store, const char *account_id,
    nh_identity_provider_type type, bool enabled,
    nh_identity_provider_record *out);

/* Mutation and operation records are accepted only by the broker-owned store. */
typedef struct nh_identity_enroll_request {
  const char *username;
  const char *pubkey_hex;
  const char *shell; /* NULL selects config.default_shell. */
  nh_identity_home_mode home_mode;
} nh_identity_enroll_request;

typedef struct nh_identity_home_evidence {
  uint64_t filesystem_device;
  uint64_t filesystem_inode;
} nh_identity_home_evidence;

typedef struct nh_identity_operation_state {
  char operation_id[NH_IDENTITY_UUID_CAP];
  char account_id[NH_IDENTITY_UUID_CAP];
  nh_identity_operation_type type;
  nh_identity_operation_phase phase;
  nh_identity_operation_outcome outcome;
  nh_identity_home_mode home_mode;
  nh_identity_home_evidence staged_home;
  nh_identity_home_evidence installed_home;
  bool replayed;
} nh_identity_operation_state;

nh_identity_rc nh_identity_operation_begin_enroll(
    nh_identity_store *store, const char *operation_id,
    const nh_identity_enroll_request *request,
    nh_identity_operation_state *out);
nh_identity_rc nh_identity_operation_advance_home(
    nh_identity_store *store, const char *operation_id,
    nh_identity_operation_phase expected_phase,
    nh_identity_operation_phase next_phase,
    const nh_identity_home_evidence *evidence,
    nh_identity_operation_state *out);
nh_identity_rc nh_identity_operation_activate(
    nh_identity_store *store, const char *operation_id,
    nh_identity_operation_state *out);
nh_identity_rc nh_identity_operation_fail(
    nh_identity_store *store, const char *operation_id,
    nh_identity_operation_outcome outcome, const char *reason_token,
    nh_identity_operation_state *out);
nh_identity_rc nh_identity_operation_get(nh_identity_store *store,
                                         const char *operation_id,
                                         nh_identity_operation_state *out);
nh_identity_rc nh_identity_operation_next_pending(
    nh_identity_store *store, const char *after_operation_id,
    nh_identity_operation_state *out);

nh_identity_rc nh_identity_account_set_status(
    nh_identity_store *store, const char *operation_id,
    const char *account_id, nh_identity_status target);
nh_identity_rc nh_identity_account_replace_identity(
    nh_identity_store *store, const char *operation_id,
    const char *account_id, const char *new_pubkey_hex,
    bool administrator_acknowledged);

typedef struct nh_identity_proof_attestation {
  uint8_t proof_event_id[32];
  char pubkey_hex[NH_IDENTITY_PUBKEY_HEX_CAP];
  uint64_t key_generation;
} nh_identity_proof_attestation;

nh_identity_rc nh_identity_provider_stage(
    nh_identity_store *store, const char *operation_id,
    const char *account_id, nh_identity_provider_type type,
    uint32_t format_version, const char *public_config_json,
    const uint8_t *secret_blob, size_t secret_blob_len,
    char provider_id_out[NH_IDENTITY_UUID_CAP]);
nh_identity_rc nh_identity_provider_activate(
    nh_identity_store *store, const char *operation_id,
    const char *provider_id,
    const nh_identity_proof_attestation *attestation);
/* Replaces the encrypted secret of a staged (not-yet-activated) provider. Lets
 * enrollment seal a vault bound to the provider_id the store assigned, then
 * store it, without a direct DB write. */
nh_identity_rc nh_identity_provider_reseal(
    nh_identity_store *store, const char *operation_id, const char *provider_id,
    const uint8_t *secret_blob, size_t secret_blob_len);
nh_identity_rc nh_identity_provider_discard(
    nh_identity_store *store, const char *operation_id,
    const char *provider_id);

/*
 * Persist the wrapped_home_key ciphertext for an enabled provider record
 * (portable-home NIP-46 enrollment, design §4.3). Writes are atomic;
 * `blob_len == 0` clears the field. Refuses length > NH_IDENTITY_
 * WRAPPED_HOME_KEY_MAX. No operation record is written — this is a
 * metadata refresh a client can perform on every successful login (the
 * value is idempotent under the same signer key).
 */
nh_identity_rc nh_identity_provider_set_wrapped_home_key(
    nh_identity_store *store, const char *provider_id,
    const uint8_t *blob, size_t blob_len);

/* Linux local-home operations. Require euid 0; never activate or publish an
 * account. skel_path is a root-controlled approved directory; NULL creates an
 * empty home. Symlinks, special files and nested mounts in skeletons are rejected.
 * The label hook must label via the supplied descriptor, not a guessed path.
 * If labeling_required is true, absence/failure of the hook fails preparation.
 * Ambiguous filesystem state is retained and marked repair_required. */
typedef nh_identity_rc (*nh_identity_home_label_fn)(void *context, int home_fd);
typedef struct nh_identity_home_options {
  const char *skel_path;
  bool labeling_required;
  nh_identity_home_label_fn label;
  void *label_context;
} nh_identity_home_options;
nh_identity_rc nh_identity_home_prepare(nh_identity_store *store,
    const char *operation_id, const nh_identity_home_options *options,
    nh_identity_operation_state *out);
nh_identity_rc nh_identity_home_validate(nh_identity_store *store,
    const char *account_id, const nh_identity_home_evidence *expected,
    nh_identity_home_evidence *out);

/* Complete-snapshot publication. The target comes only from identity config. */
nh_identity_rc nh_identity_store_publish_projection(
    nh_identity_store *store, uint64_t *projection_generation_out);

/*
 * Projection reader. Each lookup opens the immutable snapshot read-only. The
 * caller owns buffer; returned string pointers remain valid until buffer is
 * reused. Projection data must never be used to authorize authentication.
 */
typedef struct nh_identity_passwd_record {
  const char *name;
  const char *passwd;
  const char *gecos;
  const char *home;
  const char *shell;
  uint32_t uid;
  uint32_t gid;
} nh_identity_passwd_record;

typedef struct nh_identity_group_record {
  const char *name;
  const char *passwd;
  uint32_t gid;
} nh_identity_group_record;

nh_identity_reader_result nh_identity_reader_open(
    const char *path, nh_identity_reader **out);
void nh_identity_reader_close(nh_identity_reader *reader);
nh_identity_reader_result nh_identity_reader_getpwnam(
    nh_identity_reader *reader, const char *name,
    nh_identity_passwd_record *out, char *buffer, size_t buffer_len,
    size_t *required_len);
nh_identity_reader_result nh_identity_reader_getpwuid(
    nh_identity_reader *reader, uint32_t uid,
    nh_identity_passwd_record *out, char *buffer, size_t buffer_len,
    size_t *required_len);
nh_identity_reader_result nh_identity_reader_getgrnam(
    nh_identity_reader *reader, const char *name,
    nh_identity_group_record *out, char *buffer, size_t buffer_len,
    size_t *required_len);
nh_identity_reader_result nh_identity_reader_getgrgid(
    nh_identity_reader *reader, uint32_t gid,
    nh_identity_group_record *out, char *buffer, size_t buffer_len,
    size_t *required_len);
nh_identity_reader_result nh_identity_reader_get_generation(
    nh_identity_reader *reader, uint64_t *projection_generation_out,
    uint64_t *source_authority_generation_out);

/* Validation helpers shared by A and B; no global state and no I/O. */
bool nh_identity_username_is_valid(const char *username);
bool nh_identity_uuid_is_valid(const char *uuid);
bool nh_identity_pubkey_is_valid(const char *pubkey_hex);
const char *nh_identity_status_name(nh_identity_status status);
nh_identity_rc nh_identity_status_parse(const char *text,
                                        nh_identity_status *out);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_IDENTITY_H */
