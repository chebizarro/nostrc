/* SPDX-License-Identifier: MIT
 *
 * signetctl_main.c - Nostr-native management CLI for Signet.
 *
 * Publishes signed management events to relays and waits for ack responses.
 * Uses the provisioner's nsec (SIGNET_PROVISIONER_NSEC env var) to sign.
 */

#include "signet/signet_config.h"
#include "signet/mgmt_protocol.h"
#include "signet/relay_pool.h"
#include "signet/store.h"
#include "signet/store_leases.h"
#include "signet/store_audit.h"
#include "signet/store_secrets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <glib.h>
#include <json-glib/json-glib.h>

/* libnostr */
#include <sodium.h>
#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr/nip19/nip19.h>
#include <nostr/nip17/nip17.h>   /* NIP-59/NIP-17 gift-wrap for ContextVM intents */

#include <secure_buf.h>

#define SIGNETCTL_VERSION "0.1.0"
#define SIGNETCTL_TIMEOUT_SEC 10

/* ----------------------------- usage ------------------------------------- */

static void signetctl_usage(FILE *out) {
  fprintf(out,
    "signetctl %s - Nostr-native Signet management CLI\n"
    "\n"
    "Usage: signetctl [-c <config>] <command> [args]\n"
    "\n"
    "Commands:\n"
    "  provision <agent_id> [--deliver <bootstrap_pubkey>] [--ttl <sec>]\n"
    "                           Provision and optionally gift-wrap bunker URI\n"
    "  revoke <agent_id>        Revoke an agent identity\n"
    "  rotate <agent_id>        Rotate an agent identity key\n"
    "  adopt-existing <agent_id> --sec <nsec-or-hex|->\n"
    "                 [--expected-pubkey <hex>] [--deliver <bootstrap_pubkey>] [--ttl <sec>]\n"
    "                           Adopt an existing (BYO) keypair as an agent\n"
    "                           (--sec - reads the secret from stdin)\n"
    "  set-policy <agent_id> <policy-json>\n"
    "                           Set an agent policy\n"
    "  status                   Query daemon health status\n"
    "  list                     List managed agents\n"
    "\n"
    "  list-agents [--verify]   List agents with details (local store);\n"
    "                           --verify decrypts each key and checks it derives\n"
    "                           the stored pubkey (STATUS: ok/DECRYPT_FAIL/MISMATCH)\n"
    "  list-sessions            List active sessions (local store)\n"
    "  list-leases              List active credential leases (local store)\n"
    "  verify-audit             Verify hash-chained audit log integrity\n"
    "  rotate-credential <id>   Rotate a credential (--file or --stdin)\n"
    "  migrate-db               Migrate a legacy plaintext SQLite DB to SQLCipher\n"
    "\n"
    "Options:\n"
    "  -c <path>    Configuration file path\n"
    "  -h, --help   Show this help\n"
    "\n"
    "Environment:\n"
    "  SIGNET_PROVISIONER_NSEC  Provisioner's nsec (required, bech32 or hex)\n"
    "  SIGNET_RELAYS            Comma-separated relay URLs (or set in config)\n"
    "\n"
    "Examples:\n"
    "  signetctl provision my-agent\n"
    "  signetctl adopt-existing my-agent --sec nsec1... --expected-pubkey <hex>\n"
    "  signetctl revoke my-agent\n"
    "  signetctl status\n"
    "  signetctl list\n",
    SIGNETCTL_VERSION);
}

/* -------------------- build Cascadia ContextVM intent -------------------- */

/* Map a legacy management kind to its Cascadia ContextVM JSON-RPC method name.
 * (Kept internally keyed by SIGNET_KIND_* so the command dispatch is unchanged;
 * the wire form is now a kind-25910 ContextVM intent, gift-wrapped.) */
static const char *signetctl_contextvm_method(int kind) {
  switch (kind) {
    case SIGNET_KIND_PROVISION_AGENT: return "agent/provision";
    case SIGNET_KIND_REVOKE_AGENT:    return "agent/revoke";
    case SIGNET_KIND_SET_POLICY:      return "agent/set-policy";
    case SIGNET_KIND_GET_STATUS:      return "agent/get-status";
    case SIGNET_KIND_LIST_AGENTS:     return "agent/list";
    case SIGNET_KIND_ROTATE_KEY:      return "agent/rotate-key";
    case SIGNET_KIND_ADOPT_EXISTING:  return "agent/adopt-existing";
    default:                          return NULL;
  }
}

/* Build a Cascadia ContextVM JSON-RPC 2.0 intent:
 *   {"jsonrpc":"2.0","id":"<request_id>","method":"agent/...","params":{...}}
 * This is the kind-25910 CAS_INTENT payload the daemon parses. */
static char *signetctl_build_intent(int kind, const char *agent_id, const char *request_id,
                                    const char *policy_json, const char *bootstrap_pubkey,
                                    int delivery_ttl, const char *agent_nsec,
                                    const char *expected_pubkey) {
  const char *method = signetctl_contextvm_method(kind);
  if (!method) return NULL;

  JsonBuilder *b = json_builder_new();
  if (!b) return NULL;
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "jsonrpc"); json_builder_add_string_value(b, "2.0");
  json_builder_set_member_name(b, "id"); json_builder_add_string_value(b, request_id);
  json_builder_set_member_name(b, "method"); json_builder_add_string_value(b, method);

  json_builder_set_member_name(b, "params");
  json_builder_begin_object(b);
  if (agent_id) {
    json_builder_set_member_name(b, "agent_id");
    json_builder_add_string_value(b, agent_id);
  }
  if (agent_nsec) {
    json_builder_set_member_name(b, "agent_nsec");
    json_builder_add_string_value(b, agent_nsec);
  }
  if (expected_pubkey) {
    json_builder_set_member_name(b, "expected_pubkey");
    json_builder_add_string_value(b, expected_pubkey);
  }
  if (bootstrap_pubkey) {
    json_builder_set_member_name(b, "deliver");
    json_builder_add_boolean_value(b, TRUE);
    json_builder_set_member_name(b, "bootstrap_pubkey");
    json_builder_add_string_value(b, bootstrap_pubkey);
    json_builder_set_member_name(b, "delivery_ttl");
    json_builder_add_int_value(b, delivery_ttl > 0 ? delivery_ttl : 600);
  }
  if (policy_json) {
    g_autoptr(JsonParser) pp = json_parser_new();
    if (json_parser_load_from_data(pp, policy_json, -1, NULL)) {
      JsonNode *proot = json_parser_get_root(pp);
      if (proot && JSON_NODE_HOLDS_OBJECT(proot)) {
        json_builder_set_member_name(b, "policy");
        json_builder_add_value(b, json_node_copy(proot));
      }
    }
  }
  json_builder_end_object(b); /* params */
  json_builder_end_object(b); /* root */

  JsonGenerator *gen = json_generator_new();
  if (!gen) { g_object_unref(b); return NULL; }
  JsonNode *root = json_builder_get_root(b);
  json_generator_set_root(gen, root);
  json_generator_set_pretty(gen, FALSE);
  char *out = json_generator_to_data(gen, NULL);
  json_node_free(root);
  g_object_unref(gen);
  g_object_unref(b);
  return out;
}

/* Gift-wrap (NIP-59/NIP-17, kind 1059) the intent to the bunker pubkey, signed
 * with the provisioner key. Returns the serialized gift-wrap event JSON. */
static char *signetctl_build_gift_wrapped_intent(const char *intent_json,
                                                 const char *bunker_pubkey_hex,
                                                 const char *provisioner_sk_hex) {
  if (!intent_json || !bunker_pubkey_hex || !provisioner_sk_hex) return NULL;
  NostrEvent *gw = nostr_nip17_wrap_dm(provisioner_sk_hex, bunker_pubkey_hex, intent_json);
  if (!gw) return NULL;
  char *json = nostr_event_serialize_compact(gw);
  nostr_event_free(gw);
  return json;
}

/* Print only agent_id, pubkey, bunker_uri from an adopt-existing JSON-RPC
 * reply ({"jsonrpc":"2.0","result":{...}}). Never echoes secret material. */
static void signetctl_print_adopt_result(const char *reply_json) {
  g_autoptr(JsonParser) p = json_parser_new();
  if (!reply_json || !json_parser_load_from_data(p, reply_json, -1, NULL)) {
    if (reply_json) printf("%s\n", reply_json);
    return;
  }
  JsonNode *root = json_parser_get_root(p);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) { printf("%s\n", reply_json); return; }
  JsonObject *o = json_node_get_object(root);
  JsonObject *res = o;
  if (json_object_has_member(o, "result")) {
    JsonNode *rn = json_object_get_member(o, "result");
    if (rn && JSON_NODE_HOLDS_OBJECT(rn)) res = json_node_get_object(rn);
  }
  const char *aid = json_object_has_member(res, "agent_id") ? json_object_get_string_member(res, "agent_id") : NULL;
  const char *pk  = json_object_has_member(res, "pubkey") ? json_object_get_string_member(res, "pubkey") : NULL;
  const char *uri = json_object_has_member(res, "bunker_uri") ? json_object_get_string_member(res, "bunker_uri") : NULL;
  if (aid) printf("agent_id: %s\n", aid);
  if (pk)  printf("pubkey: %s\n", pk);
  if (uri) printf("bunker_uri: %s\n", uri);
}

/* ---------------------- ack response handling ----------------------------- */

typedef struct {
  bool received;
  bool is_error;                     /* true if the JSON-RPC reply carried an error */
  char *response_json;
  GMutex mu;
  GCond cond;
  /* Correlation / security fields. */
  char expected_request_id[17];      /* JSON-RPC id we sent */
  char expected_sender_hex[65];      /* bunker pubkey (reply's real sender) */
  char provisioner_sk_hex[65];       /* our SK for gift-wrap decrypt */
} SignetctlAckCtx;

static void signetctl_on_event(const SignetRelayEventView *ev, void *user_data) {
  SignetctlAckCtx *ctx = (SignetctlAckCtx *)user_data;
  if (!ctx || !ev) return;

  /* Replies are Cascadia ContextVM JSON-RPC responses delivered as NIP-59
   * gift-wraps (kind 1059). Ignore anything else. */
  if (ev->kind != NIP59_GIFT_WRAP || !ev->event_json) return;

  /* Unwrap the gift-wrap with our provisioner key. Only replies addressed to us
   * decrypt successfully; `sender` is the real (seal) author = the bunker. */
  char *inner = NULL;
  char *sender = NULL;
  {
    NostrEvent *gw = nostr_event_new();
    if (gw && nostr_event_deserialize_compact(gw, ev->event_json, NULL)) {
      (void)nostr_nip17_decrypt_dm(gw, ctx->provisioner_sk_hex, &inner, &sender);
    }
    if (gw) nostr_event_free(gw);
  }
  if (!inner) { free(inner); free(sender); return; }

  /* The reply must come from our bunker. */
  if (ctx->expected_sender_hex[0] &&
      (!sender || g_ascii_strcasecmp(sender, ctx->expected_sender_hex) != 0)) {
    free(inner); free(sender);
    return;
  }

  /* Correlate by JSON-RPC id == our request id, and note error replies. An
   * error reply from our bunker with a null/absent id (e.g. unauthorized or
   * unknown-method) is still ours only when we did not have a request id to
   * correlate. A non-matching or missing id must not be allowed to poison a
   * later command from relay history. */
  bool is_error = false;
  bool id_ok = (ctx->expected_request_id[0] == '\0');
  {
    g_autoptr(JsonParser) p = json_parser_new();
    if (json_parser_load_from_data(p, inner, -1, NULL)) {
      JsonNode *root = json_parser_get_root(p);
      if (root && JSON_NODE_HOLDS_OBJECT(root)) {
        JsonObject *o = json_node_get_object(root);
        if (json_object_has_member(o, "error")) is_error = true;
        if (json_object_has_member(o, "id")) {
          JsonNode *idn = json_object_get_member(o, "id");
          const char *rid = (JSON_NODE_HOLDS_VALUE(idn) &&
                             json_node_get_value_type(idn) == G_TYPE_STRING)
                              ? json_node_get_string(idn) : NULL;
          if (ctx->expected_request_id[0] && rid &&
              strcmp(rid, ctx->expected_request_id) == 0)
            id_ok = true;
        }
      }
    }
  }
  if (is_error && ctx->expected_request_id[0] && !id_ok) {
    free(inner); free(sender);
    return;
  }
  if (!id_ok && !is_error) { free(inner); free(sender); return; }

  g_mutex_lock(&ctx->mu);
  if (!ctx->received) {
    ctx->received = true;
    ctx->is_error = is_error;
    ctx->response_json = g_strdup(inner);
    g_cond_signal(&ctx->cond);
  }
  g_mutex_unlock(&ctx->mu);
  free(inner);
  free(sender);
}

/* ---------------------- resolve provisioner key --------------------------- */

static bool signetctl_resolve_provisioner_key(char *out_sk_hex, size_t sz) {
  const char *nsec = g_getenv("SIGNET_PROVISIONER_NSEC");
  if (!nsec || !nsec[0]) return false;

  if (g_ascii_strncasecmp(nsec, "nsec1", 5) == 0) {
    uint8_t sk[32];
    if (nostr_nip19_decode_nsec(nsec, sk) != 0) return false;
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
      out_sk_hex[i*2]   = hex[(sk[i] >> 4) & 0xF];
      out_sk_hex[i*2+1] = hex[sk[i] & 0xF];
    }
    out_sk_hex[64] = '\0';
    secure_wipe(sk, 32);
    return true;
  }

  /* Assume hex */
  if (strlen(nsec) == 64) {
    memcpy(out_sk_hex, nsec, 64);
    out_sk_hex[64] = '\0';
    return true;
  }

  return false;
}

/* ------------------------------ main ------------------------------------- */

int main(int argc, char **argv) {
  const char *config_path = NULL;
  int argi = 1;

  while (argi < argc) {
    if (strcmp(argv[argi], "-c") == 0 && (argi + 1) < argc) {
      config_path = argv[argi + 1];
      argi += 2;
    } else if (strcmp(argv[argi], "-h") == 0 || strcmp(argv[argi], "--help") == 0) {
      signetctl_usage(stdout);
      return 0;
    } else {
      break;
    }
  }

  if (argi >= argc) {
    signetctl_usage(stderr);
    return 2;
  }

  const char *cmd = argv[argi++];

  /* Determine event kind and agent_id. */
  int kind = 0;
  const char *agent_id = NULL;
  const char *deliver_bootstrap_pubkey = NULL;
  int delivery_ttl = 600;
  const char *adopt_sec = NULL;
  const char *adopt_expected_pubkey = NULL;

  if (strcmp(cmd, "provision") == 0) {
    kind = SIGNET_KIND_PROVISION_AGENT;
    if (argi >= argc) {
      fprintf(stderr, "signetctl: provision requires <agent_id>\n");
      return 2;
    }
    agent_id = argv[argi++];
    while (argi < argc) {
      if (strcmp(argv[argi], "--deliver") == 0 && (argi + 1) < argc) {
        deliver_bootstrap_pubkey = argv[argi + 1];
        argi += 2;
      } else if (strcmp(argv[argi], "--ttl") == 0 && (argi + 1) < argc) {
        delivery_ttl = atoi(argv[argi + 1]);
        argi += 2;
      } else {
        fprintf(stderr, "signetctl: unknown provision option '%s'\n", argv[argi]);
        return 2;
      }
    }
  } else if (strcmp(cmd, "revoke") == 0) {
    kind = SIGNET_KIND_REVOKE_AGENT;
    if (argi >= argc) {
      fprintf(stderr, "signetctl: revoke requires <agent_id>\n");
      return 2;
    }
    agent_id = argv[argi++];
  } else if (strcmp(cmd, "rotate") == 0) {
    kind = SIGNET_KIND_ROTATE_KEY;
    if (argi >= argc) {
      fprintf(stderr, "signetctl: rotate requires <agent_id>\n");
      return 2;
    }
    agent_id = argv[argi++];
  } else if (strcmp(cmd, "adopt-existing") == 0) {
    kind = SIGNET_KIND_ADOPT_EXISTING;
    if (argi >= argc) {
      fprintf(stderr, "signetctl: adopt-existing requires <agent_id>\n");
      return 2;
    }
    agent_id = argv[argi++];
    while (argi < argc) {
      if (strcmp(argv[argi], "--sec") == 0 && (argi + 1) < argc) {
        adopt_sec = argv[argi + 1];
        argi += 2;
      } else if (strcmp(argv[argi], "--expected-pubkey") == 0 && (argi + 1) < argc) {
        adopt_expected_pubkey = argv[argi + 1];
        argi += 2;
      } else if (strcmp(argv[argi], "--deliver") == 0 && (argi + 1) < argc) {
        deliver_bootstrap_pubkey = argv[argi + 1];
        argi += 2;
      } else if (strcmp(argv[argi], "--ttl") == 0 && (argi + 1) < argc) {
        delivery_ttl = atoi(argv[argi + 1]);
        argi += 2;
      } else {
        fprintf(stderr, "signetctl: unknown adopt-existing option '%s'\n", argv[argi]);
        return 2;
      }
    }
    if (!adopt_sec || !adopt_sec[0]) {
      fprintf(stderr, "signetctl: adopt-existing requires --sec <nsec-or-hex>\n");
      return 2;
    }
    if (strcmp(adopt_sec, "-") == 0) {
      /* Read the secret from stdin so it never appears in argv / shell history. */
      static char sec_stdin[256];
      if (!fgets(sec_stdin, sizeof(sec_stdin), stdin)) {
        fprintf(stderr, "signetctl: failed to read --sec from stdin\n");
        return 2;
      }
      sec_stdin[strcspn(sec_stdin, "\r\n")] = '\0';
      if (!sec_stdin[0]) {
        fprintf(stderr, "signetctl: empty --sec read from stdin\n");
        return 2;
      }
      adopt_sec = sec_stdin;
    }
  } else if (strcmp(cmd, "set-policy") == 0) {
    kind = SIGNET_KIND_SET_POLICY;
    if ((argi + 1) >= argc) {
      fprintf(stderr, "signetctl: set-policy requires <agent_id> <policy-json>\n");
      return 2;
    }
    agent_id = argv[argi++];
  } else if (strcmp(cmd, "status") == 0) {
    kind = SIGNET_KIND_GET_STATUS;
  } else if (strcmp(cmd, "list") == 0) {
    kind = SIGNET_KIND_LIST_AGENTS;
  } else if (strcmp(cmd, "migrate-db") == 0) {
    /* Migrate a legacy plaintext SQLite database to SQLCipher in place. */
    SignetConfig lcfg;
    if (signet_config_load(config_path, &lcfg) != 0) {
      fprintf(stderr, "signetctl: failed to load config\n");
      return 1;
    }
    const char *db_key = g_getenv("SIGNET_DB_KEY");
    int ret = 1;
    if (!signet_store_sqlcipher_available()) {
      fprintf(stderr, "signetctl: this build is not linked against SQLCipher; "
              "cannot migrate. Rebuild with -Dsignet_use_sqlcipher=true.\n");
    } else if (!db_key || !db_key[0]) {
      fprintf(stderr, "signetctl: SIGNET_DB_KEY is required to migrate.\n");
    } else if (!signet_store_file_is_plaintext_sqlite(lcfg.db_path)) {
      fprintf(stdout, "signetctl: '%s' is not a legacy plaintext SQLite database; "
              "nothing to migrate.\n", lcfg.db_path);
      ret = 0;
    } else if (signet_store_migrate_plaintext_to_sqlcipher(lcfg.db_path, db_key) == 0) {
      fprintf(stdout, "signetctl: migrated '%s' to SQLCipher.\n"
              "  A plaintext backup remains at '%s.plaintext-backup' and still "
              "contains cleartext secrets;\n  securely delete it once you have "
              "verified the migration.\n", lcfg.db_path, lcfg.db_path);
      ret = 0;
    } else {
      fprintf(stderr, "signetctl: migration of '%s' failed (original left intact).\n",
              lcfg.db_path);
    }
    signet_config_clear(&lcfg);
    return ret;
  } else if (strcmp(cmd, "list-agents") == 0 ||
             strcmp(cmd, "list-sessions") == 0 ||
             strcmp(cmd, "list-leases") == 0 ||
             strcmp(cmd, "verify-audit") == 0 ||
             strcmp(cmd, "rotate-credential") == 0) {
    /* Local store introspection — handled separately below. */
    kind = -1;
  } else {
    fprintf(stderr, "signetctl: unknown command '%s'\n", cmd);
    return 2;
  }

  /* --- Local store introspection commands --- */
  if (kind == -1) {
    /* Optional flags for introspection commands. */
    bool verify_agents = false;
    for (int ai = argi; ai < argc; ai++) {
      if (strcmp(argv[ai], "--verify") == 0) verify_agents = true;
    }
    SignetConfig lcfg;
    if (signet_config_load(config_path, &lcfg) != 0) {
      fprintf(stderr, "signetctl: failed to load config\n");
      return 1;
    }
    const char *db_key = g_getenv("SIGNET_DB_KEY");
    /* Pure-read introspection commands open the store READ-ONLY so they never
     * run schema/migration writes and can coexist with a live daemon holding
     * the same DB open. rotate-credential mutates, so it opens read-write. */
    bool ro = (strcmp(cmd, "rotate-credential") != 0);
    SignetStoreConfig scfg = {
      .db_path = lcfg.db_path,
      .master_key = db_key ? db_key : "",
      .read_only = ro,
    };
    SignetStore *store = signet_store_open(&scfg);
    if (!store) {
      fprintf(stderr, "signetctl: failed to open store at %s "
              "(see the [signet] log line above for the underlying error)\n",
              lcfg.db_path);
      signet_config_clear(&lcfg);
      return 1;
    }

    if (strcmp(cmd, "list-agents") == 0) {
      char **ids = NULL;
      size_t count = 0;
      if (signet_store_list_agents(store, &ids, &count) == 0) {
        if (verify_agents)
          printf("%-24s %-11s %-64s %-13s %s\n",
                 "AGENT_ID", "PROVENANCE", "PUBKEY", "STATUS", "CREATED_AT");
        else
          printf("%-24s %-11s %-64s %s\n",
                 "AGENT_ID", "PROVENANCE", "PUBKEY", "CREATED_AT");
        size_t failed = 0;
        for (size_t i = 0; i < count; i++) {
          /* Render from non-secret metadata so a row still shows even when its
           * custody key cannot be decrypted (wrong/rotated DEK, corrupt blob). */
          SignetAgentMeta meta;
          memset(&meta, 0, sizeof(meta));
          if (signet_store_get_agent_meta(store, ids[i], &meta) != 0) {
            printf("%-24s (error)\n", ids[i]);
            failed++;
            continue;
          }
          if (verify_agents) {
            /* --verify: attempt to decrypt the custody key and confirm it
             * derives the stored pubkey. */
            const char *status = "ok";
            SignetAgentRecord rec;
            memset(&rec, 0, sizeof(rec));
            int grc = signet_store_get_agent(store, ids[i], &rec);
            if (grc != 0) {
              status = "DECRYPT_FAIL";
              failed++;
            } else {
              char skx[65];
              for (size_t b = 0; b < rec.secret_key_len && b < 32; b++)
                snprintf(skx + b * 2, 3, "%02x", rec.secret_key[b]);
              skx[64] = '\0';
              char *dpk = nostr_key_get_public(skx);
              sodium_memzero(skx, sizeof(skx));
              if (!dpk) {
                status = "BAD_KEY";
                failed++;
              } else if (meta.pubkey && g_ascii_strcasecmp(dpk, meta.pubkey) != 0) {
                status = "MISMATCH";
                failed++;
              }
              free(dpk);
              signet_agent_record_clear(&rec);
            }
            printf("%-24s %-11s %-64s %-13s %" G_GINT64_FORMAT "\n",
                   ids[i],
                   meta.provenance ? meta.provenance : "provisioned",
                   meta.pubkey ? meta.pubkey : "(unknown)",
                   status, meta.created_at);
          } else {
            printf("%-24s %-11s %-64s %" G_GINT64_FORMAT "\n",
                   ids[i],
                   meta.provenance ? meta.provenance : "provisioned",
                   meta.pubkey ? meta.pubkey : "(unknown)",
                   meta.created_at);
          }
          signet_agent_meta_clear(&meta);
        }
        if (verify_agents)
          printf("\nTotal: %zu agents, %zu failed verification\n", count, failed);
        else
          printf("\nTotal: %zu agents\n", count);
        signet_store_free_agent_ids(ids, count);
      } else {
        fprintf(stderr, "signetctl: failed to list agents\n");
      }
    } else if (strcmp(cmd, "list-leases") == 0) {
      int64_t now = (int64_t)time(NULL);
      /* List all active leases (pass NULL agent_id for all). */
      SignetLeaseRecord *leases = NULL;
      size_t count = 0;
      if (signet_store_list_active_leases(store, NULL, now, &leases, &count) == 0) {
        printf("%-16s %-16s %-16s %-12s %-12s\n",
               "LEASE_ID", "AGENT_ID", "SECRET_ID", "ISSUED_AT", "EXPIRES_AT");
        for (size_t i = 0; i < count; i++) {
          printf("%-16s %-16s %-16s %-12" G_GINT64_FORMAT " %-12" G_GINT64_FORMAT "\n",
                 leases[i].lease_id ? leases[i].lease_id : "",
                 leases[i].agent_id ? leases[i].agent_id : "",
                 leases[i].secret_id ? leases[i].secret_id : "",
                 leases[i].issued_at,
                 leases[i].expires_at);
        }
        printf("\nTotal: %zu active leases\n", count);
        signet_lease_list_free(leases, count);
      } else {
        fprintf(stderr, "signetctl: failed to list leases\n");
      }
    } else if (strcmp(cmd, "list-sessions") == 0) {
      /* Sessions are tracked as leases with secret_id='session'. */
      int64_t now = (int64_t)time(NULL);
      SignetLeaseRecord *leases = NULL;
      size_t count = 0;
      if (signet_store_list_active_leases(store, NULL, now, &leases, &count) == 0) {
        printf("%-16s %-16s %-12s %-12s\n",
               "SESSION_ID", "AGENT_ID", "ISSUED_AT", "EXPIRES_AT");
        size_t session_count = 0;
        for (size_t i = 0; i < count; i++) {
          if (leases[i].secret_id && strcmp(leases[i].secret_id, "session") == 0) {
            printf("%-16s %-16s %-12" G_GINT64_FORMAT " %-12" G_GINT64_FORMAT "\n",
                   leases[i].lease_id ? leases[i].lease_id : "",
                   leases[i].agent_id ? leases[i].agent_id : "",
                   leases[i].issued_at,
                   leases[i].expires_at);
            session_count++;
          }
        }
        printf("\nTotal: %zu active sessions\n", session_count);
        signet_lease_list_free(leases, count);
      } else {
        fprintf(stderr, "signetctl: failed to list sessions\n");
      }
    } else if (strcmp(cmd, "verify-audit") == 0) {
      int64_t broken_id = 0;
      int vrc = signet_audit_verify_chain(store, 1, 0, &broken_id);
      if (vrc == 0) {
        int64_t total = signet_audit_log_count(store);
        printf("Audit log integrity: OK\n");
        printf("Total entries: %" G_GINT64_FORMAT "\n", total);
      } else if (vrc == 1) {
        fprintf(stderr, "Audit log integrity: BROKEN at entry %" G_GINT64_FORMAT "\n", broken_id);
      } else {
        fprintf(stderr, "signetctl: failed to verify audit chain\n");
      }
    } else if (strcmp(cmd, "rotate-credential") == 0) {
      if (argi >= argc) {
        fprintf(stderr, "signetctl: rotate-credential requires <id> [--file <path> | --stdin]\n");
        signet_store_close(store);
        signet_config_clear(&lcfg);
        return 2;
      }
      const char *cred_id = argv[argi++];

      /* Read new credential payload from --file or --stdin. */
      uint8_t *new_payload = NULL;
      size_t new_payload_len = 0;
      const char *input_file = NULL;
      bool from_stdin = false;

      for (int ai = argi; ai < argc; ai++) {
        if (strcmp(argv[ai], "--file") == 0 && (ai + 1) < argc) {
          input_file = argv[++ai];
        } else if (strcmp(argv[ai], "--stdin") == 0) {
          from_stdin = true;
        }
      }

      if (!input_file && !from_stdin) {
        fprintf(stderr, "signetctl: rotate-credential requires --file <path> or --stdin\n"
                "  Example: signetctl rotate-credential my-cred --file /path/to/secret\n"
                "  Example: echo -n 'newsecret' | signetctl rotate-credential my-cred --stdin\n");
        signet_store_close(store);
        signet_config_clear(&lcfg);
        return 2;
      }

      if (input_file) {
        /* Read credential from file. */
        gchar *contents = NULL;
        gsize length = 0;
        GError *ferr = NULL;
        if (!g_file_get_contents(input_file, &contents, &length, &ferr)) {
          fprintf(stderr, "signetctl: failed to read '%s': %s\n",
                  input_file, ferr ? ferr->message : "unknown error");
          if (ferr) g_error_free(ferr);
          signet_store_close(store);
          signet_config_clear(&lcfg);
          return 1;
        }
        new_payload = (uint8_t *)contents;
        new_payload_len = length;
      } else {
        /* Read credential from stdin. */
        GString *buf = g_string_sized_new(4096);
        char chunk[4096];
        size_t nread;
        while ((nread = fread(chunk, 1, sizeof(chunk), stdin)) > 0) {
          g_string_append_len(buf, chunk, (gssize)nread);
        }
        if (buf->len == 0) {
          fprintf(stderr, "signetctl: no data received on stdin\n");
          g_string_free(buf, TRUE);
          signet_store_close(store);
          signet_config_clear(&lcfg);
          return 1;
        }
        new_payload_len = buf->len;
        new_payload = (uint8_t *)g_string_free(buf, FALSE);
      }

      int64_t now = (int64_t)time(NULL);
      int rrc = signet_store_rotate_secret(store, cred_id, new_payload,
                                             new_payload_len, now);
      sodium_memzero(new_payload, new_payload_len);
      g_free(new_payload);

      if (rrc == 0) {
        printf("Credential '%s' rotated successfully (old version archived).\n", cred_id);
      } else {
        fprintf(stderr, "signetctl: failed to rotate credential '%s'\n", cred_id);
      }
    }

    signet_store_close(store);
    signet_config_clear(&lcfg);
    return 0;
  }

  /* Resolve provisioner secret key. */
  char provisioner_sk_hex[65];
  if (!signetctl_resolve_provisioner_key(provisioner_sk_hex, sizeof(provisioner_sk_hex))) {
    fprintf(stderr, "signetctl: SIGNET_PROVISIONER_NSEC not set or invalid\n");
    return 1;
  }

  /* Load config for relay list and bunker pubkey. */
  SignetConfig cfg;
  if (signet_config_load(config_path, &cfg) != 0) {
    fprintf(stderr, "signetctl: failed to load config\n");
    secure_wipe(provisioner_sk_hex, 64);
    return 1;
  }

  if (cfg.n_relays == 0) {
    fprintf(stderr, "signetctl: no relays configured\n");
    signet_config_clear(&cfg);
    secure_wipe(provisioner_sk_hex, 64);
    return 1;
  }

  /* Generate a request ID for correlation. */
  char request_id[17];
  uint64_t rid = (uint64_t)g_get_real_time();
  snprintf(request_id, sizeof(request_id), "%016" G_GINT64_MODIFIER "x", rid);

  const char *policy_json = NULL;
  if (kind == SIGNET_KIND_SET_POLICY) {
    policy_json = argv[argi++];
  }

  /* Build the Cascadia ContextVM intent (JSON-RPC 2.0, kind-25910 payload). */
  char *intent = signetctl_build_intent(kind, agent_id, request_id, policy_json,
                                        deliver_bootstrap_pubkey, delivery_ttl,
                                        adopt_sec, adopt_expected_pubkey);
  if (!intent) {
    fprintf(stderr, "signetctl: failed to build intent\n");
    signet_config_clear(&cfg);
    secure_wipe(provisioner_sk_hex, 64);
    return 1;
  }

  /* Gift-wrap (NIP-59/NIP-17, kind 1059) the intent to the bunker pubkey. */
  const char *bunker_pk = cfg.remote_signer_pubkey_hex[0]
                            ? cfg.remote_signer_pubkey_hex
                            : NULL;
  if (!bunker_pk) {
    fprintf(stderr, "signetctl: bunker pubkey not configured (set SIGNET_BUNKER_NSEC / [nostr] identity)\n");
    sodium_memzero(intent, strlen(intent));
    g_free(intent);
    signet_config_clear(&cfg);
    secure_wipe(provisioner_sk_hex, 64);
    return 1;
  }
  char *event_json = signetctl_build_gift_wrapped_intent(intent, bunker_pk, provisioner_sk_hex);
  sodium_memzero(intent, strlen(intent));
  g_free(intent);

  if (!event_json) {
    fprintf(stderr, "signetctl: failed to build gift-wrapped intent\n");
    signet_config_clear(&cfg);
    secure_wipe(provisioner_sk_hex, 64);
    return 1;
  }

  /* Set up relay pool to publish and listen for ack. */
  SignetctlAckCtx ack_ctx;
  memset(&ack_ctx, 0, sizeof(ack_ctx));
  g_mutex_init(&ack_ctx.mu);
  g_cond_init(&ack_ctx.cond);

  /* Populate correlation fields for ack validation.
   * Must copy SK before wiping it — needed for NIP-44 decrypt of acks. */
  memcpy(ack_ctx.expected_request_id, request_id, sizeof(ack_ctx.expected_request_id));
  if (cfg.remote_signer_pubkey_hex[0])
    memcpy(ack_ctx.expected_sender_hex, cfg.remote_signer_pubkey_hex, 65);
  memcpy(ack_ctx.provisioner_sk_hex, provisioner_sk_hex, 65);
  secure_wipe(provisioner_sk_hex, 64);

  /* signetctl signs AUTH challenges using the provisioner's own key,
   * allowing it to connect to relays that require NIP-42 auth. */
  SignetRelayPoolConfig rp_cfg = {
    .relays = (const char *const *)cfg.relays,
    .n_relays = cfg.n_relays,
    .on_event = signetctl_on_event,
    .user_data = &ack_ctx,
    .auth_sk_hex = ack_ctx.provisioner_sk_hex[0] ? ack_ctx.provisioner_sk_hex : NULL,
    .auth_relay_tag_url = g_getenv("SIGNET_AUTH_RELAY_URL"),
  };
  SignetRelayPool *rp = signet_relay_pool_new(&rp_cfg);

  int exit_code = 1;

  if (!rp || signet_relay_pool_start(rp) != 0) {
    fprintf(stderr, "signetctl: failed to start relay pool\n");
    goto cleanup;
  }

  /* Wait for relay connections, pumping the GLib main context.
   *
   * WHY: signet_relay_auth_callback schedules NIP-42 AUTH via g_idle_add().
   * Idle callbacks only execute when a GMainLoop is running or the context
   * is polled explicitly.  A plain g_usleep() here means AUTH never fires,
   * so auth-required relays (armada) always time out.
   *
   * Pumping the context also drains the post-auth re-subscribe chain
   * (signet_post_auth_resubscribe), ensuring kind:28090 subscriptions are
   * live before we publish the management event.
   *
   * We give up to 5s: connection RTT + NIP-42 challenge/response (2 relay
   * round trips) + re-subscribe EOSE.  In practice this completes in <200ms
   * on a LAN relay. */
  {
    int64_t connect_deadline = g_get_monotonic_time() + 5000000LL; /* 5s */
    gboolean connected = FALSE;
    while (g_get_monotonic_time() < connect_deadline) {
      /* Drain all pending GLib sources (idle, I/O ready, timeouts). */
      while (g_main_context_iteration(NULL, FALSE))
        ;
      if (signet_relay_pool_is_connected(rp)) {
        connected = TRUE;
        break;
      }
      g_usleep(10 * 1000); /* 10ms poll */
    }

    if (!connected) {
      fprintf(stderr, "signetctl: no relay connected after 5s\n");
      goto cleanup;
    }

    /* Subscribe for gift-wrapped ContextVM replies (kind 1059) addressed to us,
     * only AFTER the relay is connected. nostr_simple_pool_ensure_relay
     * disconnects+reconnects a relay that is not yet connected, so subscribing
     * before the initial async handshake completes churns the connection and the
     * subsequent publish can race a torn-down socket (nostrc-7k6). NIP-59
     * randomizes the gift-wrap created_at up to ~2 days into the past, so the
     * `since` window reaches back beyond that or the reply would be filtered. */
    {
      int reply_kinds[] = { NIP59_GIFT_WRAP };
      int64_t reply_since = (int64_t)time(NULL) - (2 * 24 * 3600) - 3600;
      if (reply_since < 0) reply_since = 0;
      char *prov_pub = nostr_key_get_public(ack_ctx.provisioner_sk_hex);
      signet_relay_pool_subscribe_scoped(rp, reply_kinds, 1, prov_pub, reply_since);
      free(prov_pub);
    }

    /* NPA-10: Wait for subscription EOSE instead of fixed 2s drain.
     * On non-AUTH relays, EOSE arrives within ~1 RTT of connect.
     * On AUTH relays, EOSE arrives after: AUTH challenge → response →
     * OK → post-auth resubscribe → EOSE. Timeout after 5s. */
    int64_t auth_deadline = g_get_monotonic_time() + 5000000LL; /* 5s */
    while (g_get_monotonic_time() < auth_deadline) {
      while (g_main_context_iteration(NULL, FALSE))
        ;
      if (signet_relay_pool_is_subscribed(rp)) break;
      g_usleep(10 * 1000); /* 10ms poll */
    }
  }

  /* Publish the gift-wrapped ContextVM intent. */
  if (signet_relay_pool_publish_event_json(rp, event_json) != 0) {
    fprintf(stderr, "signetctl: failed to publish event\n");
    goto cleanup;
  }

  printf("Published %s ContextVM intent (gift-wrapped). Waiting for reply...\n",
         signet_mgmt_op_to_string(signet_mgmt_op_from_kind(kind)));

  /* Wait for ack with timeout, pumping the GLib main context.
   *
   * The ack event arrives on the relay worker thread and is dispatched via
   * signet_pool_event_middleware → signetctl_on_event → g_mutex/g_cond signal.
   * g_cond_wait_until would work for the signal, BUT we must keep pumping the
   * main context in case additional idle callbacks are still pending (e.g.
   * a second auth round-trip after a relay reconnect).
   *
   * Pattern: unlock → pump → re-lock → check condition → repeat. */
  {
    int64_t end_time = g_get_monotonic_time() + (SIGNETCTL_TIMEOUT_SEC * G_USEC_PER_SEC);
    g_mutex_lock(&ack_ctx.mu);
    while (!ack_ctx.received) {
      if (g_get_monotonic_time() >= end_time) break;
      g_mutex_unlock(&ack_ctx.mu);
      while (g_main_context_iteration(NULL, FALSE))
        ;
      g_usleep(5 * 1000); /* 5ms */
      g_mutex_lock(&ack_ctx.mu);
    }
    g_mutex_unlock(&ack_ctx.mu);
  }

  if (ack_ctx.received && ack_ctx.response_json) {
    if (ack_ctx.is_error) {
      fprintf(stderr, "Error reply:\n%s\n", ack_ctx.response_json);
      exit_code = 1;
    } else {
      if (kind == SIGNET_KIND_ADOPT_EXISTING) {
        signetctl_print_adopt_result(ack_ctx.response_json);
      } else {
        printf("Reply received:\n%s\n", ack_ctx.response_json);
      }
      exit_code = 0;
    }
  } else {
    fprintf(stderr, "signetctl: timeout waiting for reply (%ds)\n", SIGNETCTL_TIMEOUT_SEC);
  }

cleanup:
  free(event_json);
  if (rp) {
    signet_relay_pool_stop(rp);
    signet_relay_pool_free(rp);
  }
  g_free(ack_ctx.response_json);
  secure_wipe(ack_ctx.provisioner_sk_hex, sizeof(ack_ctx.provisioner_sk_hex));
  g_mutex_clear(&ack_ctx.mu);
  g_cond_clear(&ack_ctx.cond);
  signet_config_clear(&cfg);
  return exit_code;
}
