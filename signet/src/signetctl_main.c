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
    "  provision <agent_id>     Provision a new agent identity\n"
    "  revoke <agent_id>        Revoke an agent identity\n"
    "  status                   Query daemon health status\n"
    "  list                     List managed agents\n"
    "\n"
    "  list-agents              List agents with details (local store)\n"
    "  list-sessions            List active sessions (local store)\n"
    "  list-leases              List active credential leases (local store)\n"
    "  verify-audit             Verify hash-chained audit log integrity\n"
    "  rotate-credential <id>   Rotate a credential (archives old version)\n"
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
    "  signetctl revoke my-agent\n"
    "  signetctl status\n"
    "  signetctl list\n",
    SIGNETCTL_VERSION);
}

/* -------------------- build management event JSON ------------------------ */

static char *signetctl_build_content(const char *agent_id, const char *request_id) {
  JsonBuilder *b = json_builder_new();
  if (!b) return NULL;

  json_builder_begin_object(b);

  if (agent_id) {
    json_builder_set_member_name(b, "agent_id");
    json_builder_add_string_value(b, agent_id);
  }

  if (request_id) {
    json_builder_set_member_name(b, "request_id");
    json_builder_add_string_value(b, request_id);
  }

  json_builder_end_object(b);

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

static char *signetctl_build_signed_event(int kind,
                                          const char *content,
                                          const char *bunker_pubkey_hex,
                                          const char *provisioner_sk_hex) {
  NostrEvent *evt = nostr_event_new();
  if (!evt) return NULL;

  nostr_event_set_kind(evt, kind);
  nostr_event_set_created_at(evt, (int64_t)time(NULL));
  nostr_event_set_content(evt, content ? content : "");

  /* Tag the bunker pubkey so it knows this is for it. */
  if (bunker_pubkey_hex) {
    NostrTags *tags = nostr_tags_new(0);
    if (tags) {
      NostrTag *p_tag = nostr_tag_new("p", bunker_pubkey_hex, NULL);
      if (p_tag) {
        nostr_tags_append(tags, p_tag);
      }
      nostr_event_set_tags(evt, tags);
    }
  }

  if (nostr_event_sign(evt, provisioner_sk_hex) != 0) {
    nostr_event_free(evt);
    return NULL;
  }

  char *json = nostr_event_serialize_compact(evt);
  nostr_event_free(evt);
  return json;
}

/* ---------------------- ack response handling ----------------------------- */

typedef struct {
  bool received;
  char *response_json;
  GMutex mu;
  GCond cond;
} SignetctlAckCtx;

static void signetctl_on_event(const SignetRelayEventView *ev, void *user_data) {
  SignetctlAckCtx *ctx = (SignetctlAckCtx *)user_data;
  if (!ctx || !ev) return;

  /* Look for ack events (kind 28090). */
  if (ev->kind == SIGNET_KIND_MGMT_ACK && ev->content) {
    g_mutex_lock(&ctx->mu);
    if (!ctx->received) {
      ctx->received = true;
      ctx->response_json = g_strdup(ev->content);
      g_cond_signal(&ctx->cond);
    }
    g_mutex_unlock(&ctx->mu);
  }
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

  if (strcmp(cmd, "provision") == 0) {
    kind = SIGNET_KIND_PROVISION_AGENT;
    if (argi >= argc) {
      fprintf(stderr, "signetctl: provision requires <agent_id>\n");
      return 2;
    }
    agent_id = argv[argi++];
  } else if (strcmp(cmd, "revoke") == 0) {
    kind = SIGNET_KIND_REVOKE_AGENT;
    if (argi >= argc) {
      fprintf(stderr, "signetctl: revoke requires <agent_id>\n");
      return 2;
    }
    agent_id = argv[argi++];
  } else if (strcmp(cmd, "status") == 0) {
    kind = SIGNET_KIND_GET_STATUS;
  } else if (strcmp(cmd, "list") == 0) {
    kind = SIGNET_KIND_LIST_AGENTS;
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
    SignetConfig lcfg;
    if (signet_config_load(config_path, &lcfg) != 0) {
      fprintf(stderr, "signetctl: failed to load config\n");
      return 1;
    }
    const char *db_key = g_getenv("SIGNET_DB_KEY");
    SignetStoreConfig scfg = { .db_path = lcfg.db_path, .master_key = db_key ? db_key : "" };
    SignetStore *store = signet_store_open(&scfg);
    if (!store) {
      fprintf(stderr, "signetctl: failed to open store at %s\n", lcfg.db_path);
      signet_config_clear(&lcfg);
      return 1;
    }

    if (strcmp(cmd, "list-agents") == 0) {
      char **ids = NULL;
      size_t count = 0;
      if (signet_store_list_agents(store, &ids, &count) == 0) {
        printf("%-20s %-10s\n", "AGENT_ID", "CREATED_AT");
        for (size_t i = 0; i < count; i++) {
          SignetAgentRecord rec;
          memset(&rec, 0, sizeof(rec));
          if (signet_store_get_agent(store, ids[i], &rec) == 0) {
            printf("%-20s %-10" G_GINT64_FORMAT "\n", ids[i], rec.created_at);
            signet_agent_record_clear(&rec);
          } else {
            printf("%-20s (error)\n", ids[i]);
          }
        }
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
        fprintf(stderr, "signetctl: rotate-credential requires <id>\n");
        signet_store_close(store);
        signet_config_clear(&lcfg);
        return 2;
      }
      const char *cred_id = argv[argi];
      /* Generate new payload (placeholder — real rotation needs new credential). */
      uint8_t new_payload[32];
      randombytes_buf(new_payload, sizeof(new_payload));
      int64_t now = (int64_t)time(NULL);
      int rrc = signet_store_rotate_secret(store, cred_id, new_payload,
                                             sizeof(new_payload), now);
      sodium_memzero(new_payload, sizeof(new_payload));
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

  /* Build event content. */
  char *content = signetctl_build_content(agent_id, request_id);
  if (!content) {
    fprintf(stderr, "signetctl: failed to build content\n");
    signet_config_clear(&cfg);
    secure_wipe(provisioner_sk_hex, 64);
    return 1;
  }

  /* Build and sign the event. */
  const char *bunker_pk = cfg.remote_signer_pubkey_hex[0]
                            ? cfg.remote_signer_pubkey_hex
                            : NULL;
  char *event_json = signetctl_build_signed_event(kind, content, bunker_pk, provisioner_sk_hex);
  g_free(content);
  secure_wipe(provisioner_sk_hex, 64);

  if (!event_json) {
    fprintf(stderr, "signetctl: failed to sign event\n");
    signet_config_clear(&cfg);
    return 1;
  }

  /* Set up relay pool to publish and listen for ack. */
  SignetctlAckCtx ack_ctx;
  memset(&ack_ctx, 0, sizeof(ack_ctx));
  g_mutex_init(&ack_ctx.mu);
  g_cond_init(&ack_ctx.cond);

  SignetRelayPoolConfig rp_cfg = {
    .relays = (const char *const *)cfg.relays,
    .n_relays = cfg.n_relays,
    .on_event = signetctl_on_event,
    .user_data = &ack_ctx,
  };
  SignetRelayPool *rp = signet_relay_pool_new(&rp_cfg);

  int exit_code = 1;

  if (!rp || signet_relay_pool_start(rp) != 0) {
    fprintf(stderr, "signetctl: failed to start relay pool\n");
    goto cleanup;
  }

  /* Subscribe for ack events. */
  int ack_kinds[] = { SIGNET_KIND_MGMT_ACK };
  signet_relay_pool_subscribe_kinds(rp, ack_kinds, 1);

  /* Wait briefly for relay connections. */
  g_usleep(500 * 1000);

  /* Publish the management event. */
  if (signet_relay_pool_publish_event_json(rp, event_json) != 0) {
    fprintf(stderr, "signetctl: failed to publish event\n");
    goto cleanup;
  }

  printf("Published %s event. Waiting for ack...\n", signet_mgmt_op_to_string(signet_mgmt_op_from_kind(kind)));

  /* Wait for ack with timeout. */
  g_mutex_lock(&ack_ctx.mu);
  if (!ack_ctx.received) {
    int64_t end_time = g_get_monotonic_time() + (SIGNETCTL_TIMEOUT_SEC * G_USEC_PER_SEC);
    while (!ack_ctx.received) {
      if (!g_cond_wait_until(&ack_ctx.cond, &ack_ctx.mu, end_time)) {
        break; /* timeout */
      }
    }
  }
  g_mutex_unlock(&ack_ctx.mu);

  if (ack_ctx.received && ack_ctx.response_json) {
    printf("Ack received:\n%s\n", ack_ctx.response_json);
    exit_code = 0;
  } else {
    fprintf(stderr, "signetctl: timeout waiting for ack (%ds)\n", SIGNETCTL_TIMEOUT_SEC);
  }

cleanup:
  free(event_json);
  if (rp) {
    signet_relay_pool_stop(rp);
    signet_relay_pool_free(rp);
  }
  g_free(ack_ctx.response_json);
  g_mutex_clear(&ack_ctx.mu);
  g_cond_clear(&ack_ctx.cond);
  signet_config_clear(&cfg);
  return exit_code;
}