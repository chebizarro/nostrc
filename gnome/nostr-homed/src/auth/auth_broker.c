#define _GNU_SOURCE
#include "auth_broker.h"

#include "auth_challenge.h"
#include "auth_peer.h"
#include "auth_provider.h"
#include "auth_ratelimit.h"
#include "auth_transaction.h"
#include "nostr-event.h"
#include "nostr_auth_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <jansson.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* SMB is optional at compile time. When NH_AUTH_BROKER_ENABLE_SMB is defined
 * the runtime is built together with the D5 SMB credential core and the
 * broker can mint SMB passwords on a verified SMB_CREDENTIAL proof; without
 * it the SMB path returns PROVIDER_UNAVAILABLE at request time. */
#ifdef NH_AUTH_BROKER_ENABLE_SMB
#  include "smb_credential.h"
#endif

struct nh_auth_broker {
  nh_identity_store *store;
  nh_auth_authority authority;
  nh_smb_authority *smb_authority; /* Borrowed; may be NULL. */
  nh_auth_broker_clock_fn clock_fn;
  void *clock_ctx;
  nh_auth_ratelimit *ratelimit;
  nh_auth_ratelimit_config ratelimit_config;
  char *ratelimit_path; /* NULL => in-memory only. */
};

/* Per-connection login state (single-owner, one transaction per connection). */
typedef struct conn_state {
  nh_auth_broker *broker;
  nh_auth_peer_snapshot peer;
  nh_auth_transaction *tx;
  nh_auth_provider *provider;
  nh_auth_challenge challenge;
  int challenge_built;
  char *signed_json;      /* captured provider SIGNED_EVENT */
  char *display_json;     /* captured provider DISPLAY_REQUIRED payload */
  nh_auth_result provider_result;
  nh_auth_provider_event_type provider_event;
  int greeter_artifact_published;
} conn_state;

static uint64_t default_now_ms(void *ctx) {
  (void)ctx;
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint64_t broker_now_ms(const nh_auth_broker *broker) {
  if (broker && broker->clock_fn) return broker->clock_fn(broker->clock_ctx);
  return default_now_ms(NULL);
}

nh_auth_broker *nh_auth_broker_new(nh_identity_store *store) {
  if (!store) return NULL;
  nh_auth_broker *broker = calloc(1, sizeof *broker);
  if (!broker) return NULL;
  broker->store = store;
  broker->authority = nh_auth_authority_from_store(store);
  broker->smb_authority = NULL;
  broker->clock_fn = NULL;
  broker->clock_ctx = NULL;
  nh_auth_ratelimit_config_defaults(&broker->ratelimit_config);
  broker->ratelimit_path = NULL;
  broker->ratelimit = nh_auth_ratelimit_new(&broker->ratelimit_config);
  if (!broker->ratelimit) {
    free(broker);
    return NULL;
  }
  return broker;
}

void nh_auth_broker_free(nh_auth_broker *broker) {
  if (!broker) return;
  nh_auth_ratelimit_free(broker->ratelimit);
  free(broker->ratelimit_path);
  free(broker);
}

void nh_auth_broker_set_smb_authority(nh_auth_broker *broker,
                                      nh_smb_authority *authority) {
  if (!broker) return;
  broker->smb_authority = authority;
}

void nh_auth_broker_set_clock(nh_auth_broker *broker,
                              nh_auth_broker_clock_fn fn, void *context) {
  if (!broker) return;
  broker->clock_fn = fn;
  broker->clock_ctx = context;
}

int nh_auth_broker_set_ratelimit_config(nh_auth_broker *broker,
                                        const nh_auth_ratelimit_config *config) {
  if (!broker) return -1;
  nh_auth_ratelimit_config cfg;
  if (config) {
    cfg = *config;
  } else {
    nh_auth_ratelimit_config_defaults(&cfg);
  }
  nh_auth_ratelimit *replacement = NULL;
  if (broker->ratelimit_path) {
    if (nh_auth_ratelimit_open_persistent(broker->ratelimit_path, &cfg,
                                          NULL, NULL, &replacement) != 0)
      return -1;
  } else {
    replacement = nh_auth_ratelimit_new(&cfg);
    if (!replacement) return -1;
  }
  nh_auth_ratelimit_free(broker->ratelimit);
  broker->ratelimit = replacement;
  broker->ratelimit_config = cfg;
  return 0;
}

int nh_auth_broker_set_ratelimit_path(nh_auth_broker *broker, const char *path) {
  if (!broker) return -1;
  nh_auth_ratelimit *replacement = NULL;
  char *stored = NULL;
  if (path && *path) {
    stored = strdup(path);
    if (!stored) return -1;
    if (nh_auth_ratelimit_open_persistent(stored, &broker->ratelimit_config,
                                          NULL, NULL, &replacement) != 0) {
      free(stored);
      return -1;
    }
  } else {
    replacement = nh_auth_ratelimit_new(&broker->ratelimit_config);
    if (!replacement) return -1;
  }
  nh_auth_ratelimit_free(broker->ratelimit);
  broker->ratelimit = replacement;
  free(broker->ratelimit_path);
  broker->ratelimit_path = stored;
  return 0;
}

static void read_boot_id(char out[37]) {
  out[0] = '\0';
  int fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return;
  char buf[64];
  ssize_t n = read(fd, buf, sizeof buf - 1);
  close(fd);
  if (n <= 0) return;
  buf[n] = '\0';
  size_t len = strspn(buf, "0123456789abcdef-");
  if (len == 36) { memcpy(out, buf, 36); out[36] = '\0'; }
}

/* ---- result mapping ---------------------------------------------------- */

static nh_auth_result tx_rc_result(nh_auth_transaction_rc rc) {
  switch (rc) {
    case NH_AUTH_TX_OK: return NH_AUTH_RESULT_OK;
    case NH_AUTH_TX_UNKNOWN_ACCOUNT: return NH_AUTH_RESULT_UNKNOWN_ACCOUNT;
    case NH_AUTH_TX_NOT_ACTIVE: return NH_AUTH_RESULT_NOT_READY;
    case NH_AUTH_TX_STALE: return NH_AUTH_RESULT_NOT_READY;
    case NH_AUTH_TX_UNAUTHORIZED: return NH_AUTH_RESULT_DENIED;
    case NH_AUTH_TX_REPLAY: return NH_AUTH_RESULT_DENIED;
    case NH_AUTH_TX_DEADLINE: return NH_AUTH_RESULT_EXPIRED;
    case NH_AUTH_TX_CANCELLED_RESULT: return NH_AUTH_RESULT_CANCELLED;
    case NH_AUTH_TX_INTERNAL: return NH_AUTH_RESULT_INTERNAL_ERROR;
    default: return NH_AUTH_RESULT_PROTOCOL_ERROR;
  }
}

static nh_auth_result proof_rc_result(nh_auth_proof_rc rc) {
  switch (rc) {
    case NH_AUTH_PROOF_OK: return NH_AUTH_RESULT_OK;
    case NH_AUTH_PROOF_EXPIRED: return NH_AUTH_RESULT_EXPIRED;
    case NH_AUTH_PROOF_STALE_ACCOUNT: return NH_AUTH_RESULT_NOT_READY;
    default: return NH_AUTH_RESULT_INVALID_PROOF;
  }
}

/* ---- responses --------------------------------------------------------- */

static int send_payload(int fd, const nh_auth_message *request,
                        nh_auth_operation op, json_t *payload /*stolen*/) {
  char *payload_json = payload ? json_dumps(payload, JSON_COMPACT) : NULL;
  if (payload) json_decref(payload);
  if (!payload_json) return -1;
  nh_auth_message response;
  memset(&response, 0, sizeof response);
  response.operation = op;
  if (request)
    memcpy(response.request_id, request->request_id, sizeof response.request_id);
  else {
    memset(response.request_id, '0', NH_AUTH_REQUEST_ID_HEX_LEN);
    response.request_id[NH_AUTH_REQUEST_ID_HEX_LEN] = '\0';
  }
  response.transaction_id[0] = '\0';
  response.payload_json = payload_json;
  int rc = nh_auth_send_message(fd, &response);
  /* Wipe: response.payload_json may echo the SMB password. */
  if (payload_json) {
    volatile char *w = (volatile char *)payload_json;
    for (size_t i = 0; payload_json[i]; i++) w[i] = 0;
  }
  free(payload_json);
  return rc;
}

static int respond(int fd, const nh_auth_message *request,
                   nh_auth_operation op, nh_auth_result result) {
  json_t *payload = json_object();
  if (!payload) return -1;
  if (json_object_set_new(payload, "result",
                          json_string(nh_auth_result_name(result))) != 0) {
    json_decref(payload);
    return -1;
  }
  return send_payload(fd, request, op, payload);
}

/* ---- account status (CHECK_ACCOUNT) ------------------------------------ */

static nh_auth_result account_result(nh_identity_store *store,
                                     const char *username) {
  nh_identity_account account;
  nh_identity_rc rc = nh_identity_store_lookup_by_name(store, username, &account);
  if (rc == NH_IDENTITY_NOT_FOUND) return NH_AUTH_RESULT_UNKNOWN_ACCOUNT;
  if (rc != NH_IDENTITY_OK) return NH_AUTH_RESULT_STORAGE_ERROR;
  switch (account.status) {
    case NH_IDENTITY_STATUS_ACTIVE: return NH_AUTH_RESULT_OK;
    case NH_IDENTITY_STATUS_DISABLED: return NH_AUTH_RESULT_DISABLED;
    case NH_IDENTITY_STATUS_ENROLLING:
    case NH_IDENTITY_STATUS_REPAIR_REQUIRED: return NH_AUTH_RESULT_NOT_READY;
    default: return NH_AUTH_RESULT_UNKNOWN_ACCOUNT;
  }
}

static const char *json_str(const char *payload_json, const char *key,
                            json_t **root_out) {
  if (!payload_json) return NULL;
  json_error_t e;
  json_t *root = json_loads(payload_json, JSON_REJECT_DUPLICATES, &e);
  if (!root || !json_is_object(root)) { if (root) json_decref(root); return NULL; }
  json_t *v = json_object_get(root, key);
  if (!v || !json_is_string(v)) { json_decref(root); return NULL; }
  *root_out = root;
  return json_string_value(v);
}

/* ---- provider event capture ------------------------------------------- */

static void provider_emit(void *context, const nh_auth_provider_event *event) {
  conn_state *cs = context;
  /* DISPLAY_REQUIRED does NOT overwrite the last outcome-shaped provider
   * event (READY / APPROVAL_PENDING / SIGNED_EVENT / DENIED / FAILED /...):
   * the SELECT_PROVIDER response payload is built from provider_result, and
   * DISPLAY_REQUIRED is metadata that rides alongside it. */
  if (event->type == NH_AUTH_PROVIDER_DISPLAY_REQUIRED) {
    if (event->data && event->data_len && event->data_len < 4096) {
      free(cs->display_json);
      cs->display_json = malloc(event->data_len + 1);
      if (cs->display_json) {
        memcpy(cs->display_json, event->data, event->data_len);
        cs->display_json[event->data_len] = '\0';
      }
    }
    return;
  }
  cs->provider_event = event->type;
  cs->provider_result = event->result;
  if (event->type == NH_AUTH_PROVIDER_SIGNED_EVENT && event->data &&
      event->data_len) {
    free(cs->signed_json);
    cs->signed_json = malloc(event->data_len + 1);
    if (cs->signed_json) {
      memcpy(cs->signed_json, event->data, event->data_len);
      cs->signed_json[event->data_len] = '\0';
    }
  }
}

static void conn_reset_proof(conn_state *cs) {
  if (cs->provider) { cs->provider->ops->destroy(cs->provider); cs->provider = NULL; }
  if (cs->challenge_built) { nh_auth_challenge_clear(&cs->challenge); cs->challenge_built = 0; }
  if (cs->signed_json) { free(cs->signed_json); cs->signed_json = NULL; }
  if (cs->display_json) {
    /* URI contains the connect secret — wipe before free. */
    volatile char *w = (volatile char *)cs->display_json;
    for (size_t i = 0; cs->display_json[i]; i++) w[i] = 0;
    free(cs->display_json);
    cs->display_json = NULL;
  }
  if (cs->greeter_artifact_published) {
    nh_broker_greeter_artifact_remove();
    cs->greeter_artifact_published = 0;
  }
  cs->provider_event = 0;
}

/* ---- login operations -------------------------------------------------- */

/* Runs BEGIN_LOGIN and, on OK, returns the account's enabled_providers bitmask
 * so the caller can echo the available provider set back to the client. */
static nh_auth_result do_begin_login(conn_state *cs, const char *payload_json,
                                     uint32_t *providers_out) {
  if (providers_out) *providers_out = 0;
  if (cs->tx) return NH_AUTH_RESULT_PROTOCOL_ERROR; /* one tx per connection */
  json_t *root = NULL;
  const char *username = json_str(payload_json, "username", &root);
  json_t *sroot = NULL;
  const char *service = json_str(payload_json, "service", &sroot);
  nh_auth_result result = NH_AUTH_RESULT_PROTOCOL_ERROR;
  if (username && username[0] && strlen(username) <= NH_IDENTITY_USERNAME_MAX &&
      service && service[0]) {
    uint64_t now = broker_now_ms(cs->broker);
    /* Rate-limit BEGIN_LOGIN by username. The check is a strict gate: no
     * BEGIN_LOGIN inside a cooldown reaches the SM, so an attacker cannot use
     * BEGIN_LOGIN to keep a transaction alive or observe SM state during
     * lockout. The failure counter is not ticked here — only SUBMIT_UNLOCK
     * failures increment it. Do not rate-limit CHECK_ACCOUNT (handled at the
     * dispatch site). */
    if (!nh_auth_ratelimit_check(cs->broker->ratelimit, username, now)) {
      result = NH_AUTH_RESULT_RATE_LIMITED;
    } else {
      nh_auth_begin_request request = {NH_AUTH_PURPOSE_LINUX_LOGIN, username,
                                       service, now};
      nh_auth_transaction_rc rc = nh_auth_transaction_begin(
          &cs->broker->authority, &cs->peer, &request, &cs->tx);
      result = tx_rc_result(rc);
    }
  }
  if (providers_out && result == NH_AUTH_RESULT_OK && cs->tx) {
    const nh_identity_account *account =
        nh_auth_transaction_get_account(cs->tx);
    if (account) *providers_out = account->enabled_providers;
  }
  if (root) json_decref(root);
  if (sroot) json_decref(sroot);
  return result;
}

/* Runs BEGIN_SMB_PROOF: the peer is proving its OWN identity (SO_PEERCRED uid
 * comes from cs->peer). No `username` is accepted from the client; the
 * transaction resolves the account by uid. `service` is optional and defaults
 * to "smb-credential" so the challenge machinery has a non-empty string. */
static nh_auth_result do_begin_smb_proof(conn_state *cs,
                                         const char *payload_json,
                                         uint32_t *providers_out) {
  if (providers_out) *providers_out = 0;
  if (cs->tx) return NH_AUTH_RESULT_PROTOCOL_ERROR;
#ifndef NH_AUTH_BROKER_ENABLE_SMB
  (void)payload_json;
  return NH_AUTH_RESULT_PROVIDER_UNAVAILABLE;
#else
  if (!cs->broker->smb_authority) return NH_AUTH_RESULT_PROVIDER_UNAVAILABLE;
  json_t *sroot = NULL;
  const char *service = json_str(payload_json, "service", &sroot);
  if (!service || !service[0]) service = "smb-credential";
  if (strlen(service) > 64) {
    if (sroot) json_decref(sroot);
    return NH_AUTH_RESULT_PROTOCOL_ERROR;
  }
  nh_auth_begin_request request = {NH_AUTH_PURPOSE_SMB_CREDENTIAL, NULL,
                                   service, broker_now_ms(cs->broker)};
  nh_auth_transaction_rc rc =
      nh_auth_transaction_begin(&cs->broker->authority, &cs->peer, &request,
                                &cs->tx);
  nh_auth_result result = tx_rc_result(rc);
  if (providers_out && result == NH_AUTH_RESULT_OK && cs->tx) {
    const nh_identity_account *account =
        nh_auth_transaction_get_account(cs->tx);
    if (account) *providers_out = account->enabled_providers;
  }
  if (sroot) json_decref(sroot);
  return result;
#endif
}

/* Builds a JSON array of canonical provider names ("local","nip46") from the
 * enabled_providers bitmask returned by do_begin_login. Returned array is
 * owned by the caller; must be json_decref'd or handed to json_object_set_new. */
static json_t *providers_json(uint32_t enabled_providers) {
  json_t *arr = json_array();
  if (!arr) return NULL;
  if (enabled_providers &
      NH_IDENTITY_PROVIDER_BIT(NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY))
    json_array_append_new(arr, json_string("local"));
  if (enabled_providers &
      NH_IDENTITY_PROVIDER_BIT(NH_IDENTITY_PROVIDER_NIP46_BUNKER))
    json_array_append_new(arr, json_string("nip46"));
  if (enabled_providers &
      NH_IDENTITY_PROVIDER_BIT(NH_IDENTITY_PROVIDER_NIP46_QR))
    json_array_append_new(arr, json_string("nip46qr"));
  return arr;
}

/* Map the optional "provider" payload field to a provider type. When the
 * client does not name a provider, prefer NIP-46 if the account has it
 * enabled (external signer beats a local passphrase prompt), otherwise fall
 * back to the local encrypted vault. Unknown names are a protocol error. */
static int pick_provider_type(const char *payload_json,
                              const nh_identity_account *account,
                              nh_identity_provider_type *out) {
  json_t *root = NULL;
  const char *name = payload_json ? json_str(payload_json, "provider", &root)
                                  : NULL;
  int rc = 0;
  if (name && *name) {
    if (!strcmp(name, "local") || !strcmp(name, "local_encrypted_key"))
      *out = NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY;
    else if (!strcmp(name, "nip46") || !strcmp(name, "nip46_bunker") ||
             !strcmp(name, "bunker"))
      *out = NH_IDENTITY_PROVIDER_NIP46_BUNKER;
    else if (!strcmp(name, "nip46qr") || !strcmp(name, "qr") ||
             !strcmp(name, "nip46_qr"))
      *out = NH_IDENTITY_PROVIDER_NIP46_QR;
    else
      rc = -1;
  } else if (account->enabled_providers &
             NH_IDENTITY_PROVIDER_BIT(NH_IDENTITY_PROVIDER_NIP46_BUNKER)) {
    *out = NH_IDENTITY_PROVIDER_NIP46_BUNKER;
  } else if (account->enabled_providers &
             NH_IDENTITY_PROVIDER_BIT(NH_IDENTITY_PROVIDER_NIP46_QR)) {
    *out = NH_IDENTITY_PROVIDER_NIP46_QR;
  } else {
    *out = NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY;
  }
  if (root) json_decref(root);
  return rc;
}

static nh_auth_result do_select_provider(conn_state *cs,
                                         const char *payload_json) {
  if (!cs->tx) return NH_AUTH_RESULT_PROTOCOL_ERROR;
  const nh_identity_account *account = nh_auth_transaction_get_account(cs->tx);
  if (!account) return NH_AUTH_RESULT_INTERNAL_ERROR;

  nh_identity_provider_type provider_type;
  if (pick_provider_type(payload_json, account, &provider_type) != 0)
    return NH_AUTH_RESULT_PROTOCOL_ERROR;

  uint64_t now = broker_now_ms(cs->broker);
  nh_auth_transaction_rc rc = nh_auth_transaction_select_provider(
      cs->tx, &cs->peer, provider_type, now);
  if (rc != NH_AUTH_TX_OK) return tx_rc_result(rc);
  uint64_t challenge_deadline = 0;
  rc = nh_auth_transaction_begin_proof(cs->tx, &cs->peer, now,
                                       &challenge_deadline);
  if (rc != NH_AUTH_TX_OK) return tx_rc_result(rc);

  nh_identity_store_info info;
  if (nh_identity_store_get_info(cs->broker->store, &info) != NH_IDENTITY_OK)
    return NH_AUTH_RESULT_STORAGE_ERROR;
  nh_identity_provider_record rec;
  {
    nh_identity_rc grc = nh_identity_store_provider_get(
        cs->broker->store, account->account_id, provider_type, true, &rec);
    if (grc != NH_IDENTITY_OK)
      return NH_AUTH_RESULT_PROVIDER_UNAVAILABLE;
  }

  char boot_id[37];
  read_boot_id(boot_id);
  int64_t issued = (int64_t)time(NULL);
  nh_auth_challenge_input in = {0};
  in.purpose = nh_auth_transaction_get_purpose(cs->tx);
  in.transaction_id = nh_auth_transaction_get_id(cs->tx);
  in.authority_id = info.authority_id;
  in.boot_id = boot_id[0] ? boot_id : "00000000-0000-4000-8000-000000000000";
  in.service = nh_auth_transaction_get_service(cs->tx);
  in.context_json = "";
  in.resource_json = "";
  in.account = account;
  in.issued_at = issued;
  in.expires_at = issued + (int64_t)NH_AUTH_CHALLENGE_LIFETIME_SEC;
  in.deadline_monotonic_ms = challenge_deadline;
  in.nonce32 = NULL;
  if (nh_auth_challenge_build(&in, &cs->challenge) != 0)
    return NH_AUTH_RESULT_INTERNAL_ERROR;
  cs->challenge_built = 1;

  char *unsigned_json = nostr_event_serialize_compact(cs->challenge.event);
  if (!unsigned_json) return NH_AUTH_RESULT_INTERNAL_ERROR;
  switch (provider_type) {
    case NH_IDENTITY_PROVIDER_NIP46_BUNKER:
      cs->provider = nh_auth_provider_nip46_new(provider_emit, cs);
      break;
    case NH_IDENTITY_PROVIDER_NIP46_QR:
      cs->provider = nh_auth_provider_nip46_qr_new(provider_emit, cs);
      break;
    default:
      cs->provider = nh_auth_provider_local_new(provider_emit, cs);
      break;
  }
  nh_auth_result result = NH_AUTH_RESULT_PROVIDER_UNAVAILABLE;
  if (cs->provider) {
    nh_auth_provider_snapshot snap = {0};
    snap.type = provider_type;
    snap.provider_id = rec.provider_id;
    snap.account_id = account->account_id;
    snap.pubkey_hex = account->pubkey_hex;
    snap.key_generation = account->key_generation;
    snap.deadline_monotonic_ms = challenge_deadline;
    snap.public_config_json = rec.public_config_json;
    snap.secret_blob = rec.secret_blob;
    snap.secret_blob_len = rec.secret_blob_len;
    nh_auth_immutable_challenge ic = {0};
    ic.unsigned_event_json = unsigned_json;
    ic.unsigned_event_json_len = strlen(unsigned_json);
    ic.expected_event_id = cs->challenge.expected_id;
    ic.deadline_monotonic_ms = challenge_deadline;
    if (cs->provider->ops->prepare(cs->provider, &snap) == 0 &&
        cs->provider->ops->begin_proof(cs->provider, &ic) == 0)
      result = NH_AUTH_RESULT_INTERACTION_REQUIRED;
  }
  free(unsigned_json);
  return result;
}

/* B5-profile: rate-limited fire-and-forget refresh of the account's
 * kind-0 → AccountsService state after a successful login. Runs as a
 * detached grandchild so the broker never blocks on a slow relay or a
 * hung D-Bus call. Enforces a ≤ once-per-hour throttle by mtime of
 * /var/lib/nostr-auth/profile/<user>.json; when NH_PROFILE_FETCH=off
 * (or the env override is "off") the hook is disabled entirely.
 *
 * Never returns an error to the login path — the greeter avatar is a
 * cosmetic surface that must fall back cleanly when broken. */
static void profile_refresh_hook_maybe(const char *username,
                                       const char *pubkey_hex) {
  if (!username || !*username || !pubkey_hex || strlen(pubkey_hex) != 64) return;
  const char *toggle = getenv("NH_PROFILE_FETCH");
  if (toggle && (!strcasecmp(toggle, "off") || !strcmp(toggle, "0") ||
                 !strcasecmp(toggle, "no") || !strcasecmp(toggle, "false")))
    return;

  /* Throttle: refuse if the cache mtime is < 1h old. */
  {
    const char *dir = getenv("NH_PROFILE_CACHE_DIR");
    if (!dir || !*dir) dir = "/var/lib/nostr-auth/profile";
    char path[512];
    int pn = snprintf(path, sizeof path, "%s/%s.json", dir, username);
    if (pn > 0 && (size_t)pn < sizeof path) {
      struct stat st;
      if (stat(path, &st) == 0) {
        time_t now = time(NULL);
        if (now - st.st_mtime < 3600) return;
      }
    }
  }

  /* Double-fork so we don't need to reap; init inherits the grandchild. */
  pid_t p1 = fork();
  if (p1 < 0) return;
  if (p1 == 0) {
    if (setsid() < 0) _exit(0);
    pid_t p2 = fork();
    if (p2 < 0) _exit(0);
    if (p2 > 0) _exit(0);

    /* Grandchild: exec the CLI. Redirect stdio to /dev/null so any
     * per-fetch chatter cannot pollute the broker's journal. */
    int devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (devnull >= 0) { dup2(devnull, 0); dup2(devnull, 1); dup2(devnull, 2); close(devnull); }

    char pk_arg[80];
    snprintf(pk_arg, sizeof pk_arg, "--pubkey=%s", pubkey_hex);
    /* Ensure the ≤1h throttle is honoured inside the CLI too. */
    char throttle_arg[64] = "--throttle-seconds=3600";
    const char *cli = getenv("NH_PROFILE_CLI");
    if (!cli || !*cli) cli = "nostr-homed-profile";
    char *args[] = {
      (char *)cli, (char *)"refresh", (char *)username,
      pk_arg, throttle_arg, NULL,
    };
    if (cli[0] == '/') execv(cli, args); else execvp(cli, args);
    _exit(0);
  }
  int st = 0;
  while (waitpid(p1, &st, 0) < 0) { if (errno != EINTR) break; }
}

static nh_auth_result do_submit_unlock(conn_state *cs, const char *payload_json,
                                       nh_auth_receipt *receipt_out,
                                       int *have_receipt) {
  *have_receipt = 0;
  if (!cs->tx || !cs->provider || !cs->challenge_built)
    return NH_AUTH_RESULT_PROTOCOL_ERROR;

  /* Broker-layer deadline guard: if the challenge deadline has already
   * passed we return EXPIRED without engaging the provider or the SM. This
   * mirrors the SM's own start/finish_verification checks (which also fire
   * TX_DEADLINE past the challenge deadline and are mapped to EXPIRED via
   * tx_rc_result) but avoids invoking the provider on a doomed request. */
  uint64_t now = broker_now_ms(cs->broker);
  if (cs->challenge.deadline_monotonic_ms &&
      now >= cs->challenge.deadline_monotonic_ms)
    return NH_AUTH_RESULT_EXPIRED;

  /* Rate-limit SUBMIT_UNLOCK by the account's username (same key used at
   * BEGIN_LOGIN). This is a defence-in-depth check for the case where a
   * cooldown fires between BEGIN_LOGIN and SUBMIT_UNLOCK on the same
   * connection. */
  const nh_identity_account *account = nh_auth_transaction_get_account(cs->tx);
  if (account && account->username[0] &&
      !nh_auth_ratelimit_check(cs->broker->ratelimit, account->username, now))
    return NH_AUTH_RESULT_RATE_LIMITED;

  json_t *root = NULL;
  const char *secret = json_str(payload_json, "secret", &root);
  if (!secret || !secret[0] || strlen(secret) > NH_AUTH_SECRET_MAX) {
    if (root) json_decref(root);
    return NH_AUTH_RESULT_PROTOCOL_ERROR;
  }
  cs->provider_event = 0;
  cs->provider->ops->submit_unlock(cs->provider, (const uint8_t *)secret,
                                   strlen(secret));
  if (root) { /* wipe the transient secret copy in the parsed JSON */
    volatile char *w = (volatile char *)secret;
    for (size_t i = 0; secret[i]; i++) w[i] = 0;
    json_decref(root);
  }

  nh_auth_proof_rc proof;
  if (cs->provider_event == NH_AUTH_PROVIDER_SIGNED_EVENT && cs->signed_json) {
    proof = nh_auth_challenge_verify(&cs->challenge, cs->signed_json,
                                     broker_now_ms(cs->broker), account);
  } else if (cs->provider_event == NH_AUTH_PROVIDER_DENIED) {
    proof = NH_AUTH_PROOF_INVALID; /* wrong passphrase */
  } else {
    proof = NH_AUTH_PROOF_CRYPTO_ERROR;
  }

  now = broker_now_ms(cs->broker);
  nh_auth_transaction_rc rc =
      nh_auth_transaction_start_verification(cs->tx, &cs->peer, now);
  if (rc != NH_AUTH_TX_OK) return tx_rc_result(rc);
  nh_auth_receipt receipt;
  rc = nh_auth_transaction_finish_verification(cs->tx, &cs->peer, now, proof,
                                               &receipt);
  if (proof != NH_AUTH_PROOF_OK) return proof_rc_result(proof);
  if (rc != NH_AUTH_TX_OK) return tx_rc_result(rc);
  *receipt_out = receipt;
  *have_receipt = 1;
  return NH_AUTH_RESULT_OK;
}

/* Encode a receipt token as a hex-string binding for the SMB journal.  The
 * SMB authority only records this opaque string; the raw receipt never
 * leaves the broker. */
static void hex_encode(char *out, const uint8_t *bytes, size_t len) {
  static const char d[] = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) {
    out[i * 2] = d[bytes[i] >> 4];
    out[i * 2 + 1] = d[bytes[i] & 0xf];
  }
  out[len * 2] = '\0';
}

/* Build the LINUX_LOGIN success payload (receipt hex). */
static nh_auth_result build_login_payload(const nh_auth_receipt *receipt,
                                          json_t **payload_out) {
  json_t *payload = json_object();
  if (!payload) return NH_AUTH_RESULT_INTERNAL_ERROR;
  char hex[NH_AUTH_RECEIPT_LEN * 2 + 1];
  hex_encode(hex, receipt->token, NH_AUTH_RECEIPT_LEN);
  if (json_object_set_new(payload, "result",
                          json_string(nh_auth_result_name(NH_AUTH_RESULT_OK))) != 0 ||
      json_object_set_new(payload, "receipt", json_string(hex)) != 0) {
    json_decref(payload);
    return NH_AUTH_RESULT_INTERNAL_ERROR;
  }
  *payload_out = payload;
  return NH_AUTH_RESULT_OK;
}

/* Build the SMB_CREDENTIAL success payload: mint a password against the
 * attached SMB authority and package it into the response. Wipes the mint
 * envelope after copying its contents into the JSON payload. The caller
 * (send_payload) will additionally wipe the encoded payload bytes on the
 * wire buffer. */
static nh_auth_result build_smb_payload(nh_auth_broker *broker,
                                        const nh_auth_receipt *receipt,
                                        const nh_identity_account *account,
                                        json_t **payload_out) {
#ifndef NH_AUTH_BROKER_ENABLE_SMB
  (void)broker; (void)receipt; (void)account; (void)payload_out;
  return NH_AUTH_RESULT_PROVIDER_UNAVAILABLE;
#else
  if (!broker->smb_authority) return NH_AUTH_RESULT_PROVIDER_UNAVAILABLE;
  char binding[NH_AUTH_RECEIPT_LEN * 2 + 1];
  hex_encode(binding, receipt->token, NH_AUTH_RECEIPT_LEN);
  nh_smb_issue_request req = {0};
  req.account = *account;
  req.binding = binding;
  req.now_monotonic_ms = broker_now_ms(broker);
  /* 5 minute credential lifetime by default; matches the receipt window. */
  req.lifetime_ms = 5u * 60u * 1000u;
  nh_smb_envelope env;
  memset(&env, 0, sizeof env);
  nh_smb_rc smb_rc = nh_smb_credential_issue(broker->smb_authority, &req, &env);
  /* Wipe binding: it names the receipt token. */
  volatile char *bw = (volatile char *)binding;
  for (size_t i = 0; i < sizeof binding; i++) bw[i] = 0;
  if (smb_rc != NH_SMB_OK) {
    nh_smb_envelope_clear(&env);
    switch (smb_rc) {
      case NH_SMB_INVALID: return NH_AUTH_RESULT_PROTOCOL_ERROR;
      case NH_SMB_NO_MEMORY:
      case NH_SMB_INTERNAL: return NH_AUTH_RESULT_INTERNAL_ERROR;
      case NH_SMB_STORAGE_ERROR: return NH_AUTH_RESULT_STORAGE_ERROR;
      case NH_SMB_PASSDB_ERROR: return NH_AUTH_RESULT_PROVIDER_UNAVAILABLE;
      case NH_SMB_NOT_FOUND: return NH_AUTH_RESULT_UNKNOWN_ACCOUNT;
      default: return NH_AUTH_RESULT_INTERNAL_ERROR;
    }
  }
  json_t *payload = json_object();
  json_t *cred = json_object();
  if (!payload || !cred) {
    if (payload) json_decref(payload);
    if (cred) json_decref(cred);
    nh_smb_envelope_clear(&env);
    return NH_AUTH_RESULT_INTERNAL_ERROR;
  }
  int bad =
      json_object_set_new(payload, "result",
                          json_string(nh_auth_result_name(NH_AUTH_RESULT_OK))) ||
      json_object_set_new(cred, "credential_id",
                          json_string(env.credential_id)) ||
      json_object_set_new(cred, "username", json_string(env.username)) ||
      json_object_set_new(cred, "password",
                          json_stringn((const char *)env.password.ptr,
                                       env.password_len)) ||
      json_object_set_new(cred, "issued_at_ms",
                          json_integer((json_int_t)env.issued_at_ms)) ||
      json_object_set_new(cred, "expires_at_ms",
                          json_integer((json_int_t)env.expires_at_ms)) ||
      json_object_set_new(payload, "credential", cred);
  /* Zeroize the volatile envelope once its contents are in the JSON tree;
   * the plaintext is now in the payload buffer and the response text will
   * be wiped by send_payload after transmit. */
  nh_smb_envelope_clear(&env);
  if (bad) {
    /* On failure the cred object may have been transferred via _set_new;
     * decrefing payload releases everything reachable. */
    json_decref(payload);
    return NH_AUTH_RESULT_INTERNAL_ERROR;
  }
  *payload_out = payload;
  return NH_AUTH_RESULT_OK;
#endif
}

/* ---- dispatch ---------------------------------------------------------- */

static int handle_request(conn_state *cs, int fd, const nh_auth_message *req) {
  nh_auth_operation op = req->operation;
  if (!nh_auth_operation_allowed(cs->peer.endpoint, op, cs->peer.uid,
                                 cs->tx != NULL))
    return respond(fd, req, op, NH_AUTH_RESULT_DENIED);

  switch (op) {
    case NH_AUTH_OP_CHECK_ACCOUNT: {
      json_t *root = NULL;
      const char *username = json_str(req->payload_json, "username", &root);
      nh_auth_result r = (username && username[0] &&
                          strlen(username) <= NH_IDENTITY_USERNAME_MAX)
                             ? account_result(cs->broker->store, username)
                             : NH_AUTH_RESULT_PROTOCOL_ERROR;
      if (root) json_decref(root);
      return respond(fd, req, op, r);
    }
    case NH_AUTH_OP_BEGIN_LOGIN: {
      uint32_t providers = 0;
      nh_auth_result r = do_begin_login(cs, req->payload_json, &providers);
      if (r != NH_AUTH_RESULT_OK) return respond(fd, req, op, r);
      /* Set providers BEFORE result so on any failure only one owner needs to
       * be decref'd: json_object_set_new steals its value reference even on
       * failure, so if the providers set fails the array is already gone;
       * if the result set fails the array is safely inside payload. */
      json_t *payload = json_object();
      json_t *arr = providers_json(providers);
      if (!payload || !arr ||
          json_object_set_new(payload, "providers", arr) != 0 ||
          json_object_set_new(payload, "result",
                              json_string(nh_auth_result_name(r))) != 0) {
        if (payload) json_decref(payload);
        else if (arr) json_decref(arr);
        return respond(fd, req, op, NH_AUTH_RESULT_INTERNAL_ERROR);
      }
      return send_payload(fd, req, op, payload);
    }
    case NH_AUTH_OP_BEGIN_SMB_PROOF: {
      uint32_t providers = 0;
      nh_auth_result r = do_begin_smb_proof(cs, req->payload_json, &providers);
      if (r != NH_AUTH_RESULT_OK) return respond(fd, req, op, r);
      json_t *payload = json_object();
      json_t *arr = providers_json(providers);
      if (!payload || !arr ||
          json_object_set_new(payload, "providers", arr) != 0 ||
          json_object_set_new(payload, "result",
                              json_string(nh_auth_result_name(r))) != 0) {
        if (payload) json_decref(payload);
        else if (arr) json_decref(arr);
        return respond(fd, req, op, NH_AUTH_RESULT_INTERNAL_ERROR);
      }
      return send_payload(fd, req, op, payload);
    }
    case NH_AUTH_OP_SELECT_PROVIDER: {
      nh_auth_result r = do_select_provider(cs, req->payload_json);
      /* When the provider handed back a display payload (currently only the
       * NIP-46 QR / nostrconnect flow does), publish it into the greeter-
       * artifact drop and echo it in the response for the PAM module. Old
       * clients ignore the unknown field per NH_AUTH_PROTOCOL_VERSION=1
       * additive-optional-fields rule (design §5.3, D8). */
      if (cs->display_json && cs->display_json[0]) {
        if (!cs->greeter_artifact_published) {
          const char *tx_id = cs->tx ? nh_auth_transaction_get_id(cs->tx) : "";
          if (nh_broker_greeter_artifact_write(tx_id ? tx_id : "",
                                               cs->display_json) == 0)
            cs->greeter_artifact_published = 1;
        }
        json_t *payload = json_object();
        json_error_t je;
        json_t *disp = json_loads(cs->display_json, 0, &je);
        if (payload && disp &&
            json_object_set_new(payload, "result",
                                json_string(nh_auth_result_name(r))) == 0 &&
            json_object_set_new(payload, "display", disp) == 0) {
          return send_payload(fd, req, op, payload);
        }
        if (payload) json_decref(payload);
        if (disp) json_decref(disp);
      }
      return respond(fd, req, op, r);
    }
    case NH_AUTH_OP_SUBMIT_UNLOCK: {
      nh_auth_receipt receipt;
      int have = 0;
      /* Capture the username BEFORE running the submit so that we can update
       * the rate-limit state after the SM has finalised (the SM may have
       * already zeroed its account struct on failure paths in future
       * revisions; grabbing the copy up front is the safe idiom). */
      char rl_key[NH_IDENTITY_USERNAME_CAP + 1];
      rl_key[0] = '\0';
      if (cs->tx) {
        const nh_identity_account *acct = nh_auth_transaction_get_account(cs->tx);
        if (acct && acct->username[0]) {
          size_t n = strnlen(acct->username, NH_IDENTITY_USERNAME_CAP);
          memcpy(rl_key, acct->username, n);
          rl_key[n] = '\0';
        }
      }
      nh_auth_result r = do_submit_unlock(cs, req->payload_json, &receipt, &have);
      /* Update rate-limit state:
       *   OK               → clear counter/cooldown (successful auth).
       *   INVALID_PROOF    → tick a failure. Only genuine wrong-proof outcomes
       *                      count towards the budget; protocol errors,
       *                      transport failures and internal errors do not.
       *   RATE_LIMITED     → already gated, no state change.
       *   EXPIRED/other    → no state change (deadlines are not attacker-
       *                      controlled and should not consume the budget).
       */
      if (rl_key[0]) {
        uint64_t now = broker_now_ms(cs->broker);
        if (r == NH_AUTH_RESULT_OK) {
          nh_auth_ratelimit_reset(cs->broker->ratelimit, rl_key);
        } else if (r == NH_AUTH_RESULT_INVALID_PROOF) {
          nh_auth_ratelimit_record_failure(cs->broker->ratelimit, rl_key, now);
        }
      }
      /* B5-profile: only fire on genuine LINUX_LOGIN successes. SMB
       * credential mints are a different purpose and would double-fire
       * every 30 seconds during an SMB unlock loop. */
      if (r == NH_AUTH_RESULT_OK && cs->tx) {
        nh_auth_purpose purpose = nh_auth_transaction_get_purpose(cs->tx);
        const nh_identity_account *acct = nh_auth_transaction_get_account(cs->tx);
        if (purpose == NH_AUTH_PURPOSE_LINUX_LOGIN && acct)
          profile_refresh_hook_maybe(acct->username, acct->pubkey_hex);
      }
      if (r == NH_AUTH_RESULT_OK && have) {
        nh_auth_purpose purpose = nh_auth_transaction_get_purpose(cs->tx);
        const nh_identity_account *account =
            nh_auth_transaction_get_account(cs->tx);
        json_t *payload = NULL;
        nh_auth_result build_r;
        if (purpose == NH_AUTH_PURPOSE_SMB_CREDENTIAL && account) {
          build_r = build_smb_payload(cs->broker, &receipt, account, &payload);
        } else {
          build_r = build_login_payload(&receipt, &payload);
        }
        /* Wipe the local receipt copy regardless of build outcome. */
        volatile uint8_t *rw = (volatile uint8_t *)receipt.token;
        for (size_t i = 0; i < NH_AUTH_RECEIPT_LEN; i++) rw[i] = 0;
        if (build_r != NH_AUTH_RESULT_OK) {
          if (payload) json_decref(payload);
          return respond(fd, req, op, build_r);
        }
        return send_payload(fd, req, op, payload);
      }
      return respond(fd, req, op, r);
    }
    case NH_AUTH_OP_WAIT_RESULT: {
      nh_auth_transaction_state st =
          cs->tx ? nh_auth_transaction_get_state(cs->tx) : NH_AUTH_TX_NEW;
      nh_auth_result r = (st == NH_AUTH_TX_VERIFIED ||
                          st == NH_AUTH_TX_SESSION_OPENED)
                             ? NH_AUTH_RESULT_OK
                             : (st == NH_AUTH_TX_DENIED
                                    ? NH_AUTH_RESULT_DENIED
                                    : NH_AUTH_RESULT_INTERACTION_REQUIRED);
      return respond(fd, req, op, r);
    }
    case NH_AUTH_OP_CANCEL: {
      if (cs->tx) nh_auth_transaction_cancel(cs->tx, &cs->peer);
      return respond(fd, req, op, NH_AUTH_RESULT_CANCELLED);
    }
    default:
      return respond(fd, req, op, NH_AUTH_RESULT_PROTOCOL_ERROR);
  }
}

/* Shared connection driver: peer is already populated (endpoint set by the
 * caller). Reads/dispatches until the socket closes. */
static int drive_connection(nh_auth_broker *broker, int fd, conn_state *cs) {
  int rc = 0;
  for (;;) {
    unsigned char *packet = NULL;
    size_t packet_len = 0;
    if (nh_auth_recv_packet(fd, &packet, &packet_len) != 0) break; /* closed */
    nh_auth_message request;
    int parsed = nh_auth_message_parse(packet, packet_len, &request);
    if (packet) { memset(packet, 0, packet_len); free(packet); }
    if (parsed != 0) {
      rc = respond(fd, NULL, NH_AUTH_OP_CHECK_ACCOUNT,
                   NH_AUTH_RESULT_PROTOCOL_ERROR);
    } else {
      rc = handle_request(cs, fd, &request);
      nh_auth_message_clear(&request);
    }
    if (rc != 0) break;
  }

  conn_reset_proof(cs);
  if (cs->tx) nh_auth_transaction_free(cs->tx);
  (void)broker;
  return rc;
}

int nh_auth_broker_handle_connection(nh_auth_broker *broker, int fd) {
  if (!broker) return -1;
  conn_state cs;
  memset(&cs, 0, sizeof cs);
  cs.broker = broker;
  if (nh_auth_peer_from_fd(fd, NH_AUTH_ENDPOINT_AUTH, &cs.peer) != 0) return -1;
  return drive_connection(broker, fd, &cs);
}

int nh_auth_broker_handle_user_connection(nh_auth_broker *broker, int fd) {
  if (!broker) return -1;
  conn_state cs;
  memset(&cs, 0, sizeof cs);
  cs.broker = broker;
  if (nh_auth_peer_from_fd(fd, NH_AUTH_ENDPOINT_USER, &cs.peer) != 0) return -1;
  return drive_connection(broker, fd, &cs);
}
