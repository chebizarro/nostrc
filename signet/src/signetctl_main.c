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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <glib.h>
#include <json-glib/json-glib.h>

/* libnostr */
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
    NostrTags *tags = nostr_tags_new();
    if (tags) {
      const char *p_tag[] = {"p", bunker_pubkey_hex};
      nostr_tags_add(tags, p_tag, 2);
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
  } else {
    fprintf(stderr, "signetctl: unknown command '%s'\n", cmd);
    return 2;
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