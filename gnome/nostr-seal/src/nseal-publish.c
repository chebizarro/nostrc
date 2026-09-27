/* nseal-publish.c — see nseal-publish.h (nostrc-hby8).
 * SPDX-License-Identifier: MIT */
#include "nseal-publish.h"
#include "nseal-config.h"
#include "nseal-signer.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifdef NSEAL_HAVE_PUBLISH
#include "ns-blossom.h"
#include "ns-config.h"
#include "ns-event.h"
#include "ns-net.h"
#include <nostr-publish/nostr-publish.h>
#endif

#define NSEAL_1063_ALT "Sealed file (nostr-seal): only the listed recipients can open it"

gboolean nseal_publish_available(void) {
#ifdef NSEAL_HAVE_PUBLISH
  return TRUE;
#else
  return FALSE;
#endif
}

#ifdef NSEAL_HAVE_PUBLISH

char *nseal_publish_event_json(const char *pubkey_hex, gint64 created_at, const char *url,
                               const char *sha256_hex, guint64 size,
                               const char *const *recipients_hex, gboolean pretty) {
  NsBlobMeta meta = {0};
  meta.url = (char *)url;
  meta.mime = (char *)NSEAL_MIME_TYPE;
  g_strlcpy(meta.sha256, sha256_hex, sizeof(meta.sha256));
  meta.size = size;
  /* NIP-31 alt: no file name, nothing about the plaintext. */
  meta.alt = (char *)NSEAL_1063_ALT;
  g_autoptr(JsonArray) tags = json_array_new();
  ns_tags_add_file_metadata(tags, &meta);
  for (guint i = 0; recipients_hex && recipients_hex[i]; i++)
    if (!ns_tags_has(tags, "p", recipients_hex[i]))
      ns_tags_add(tags, "p", recipients_hex[i], NULL);
  return ns_event_unsigned_json(1063, created_at, pubkey_hex, tags, "", pretty);
}

static char *hex32(const uint8_t *b) {
  char *h = g_malloc(65);
  for (int i = 0; i < 32; i++) g_snprintf(h + 2 * i, 3, "%02x", b[i]);
  return h;
}

/* Kind 10002 write relays (NIP-65) from the session relay + home_relays,
 * else home_relays. Never NULL. */
static char **write_relays(NsConfig *cfg, NsNet *net, const char *pubkey_hex) {
  g_autoptr(GStrvBuilder) disc = g_strv_builder_new();
  if (net->session_socket) g_strv_builder_add(disc, NS_SESSION_RELAY_URL);
  for (guint i = 0; cfg->home_relays && cfg->home_relays[i]; i++)
    g_strv_builder_add(disc, cfg->home_relays[i]);
  g_auto(GStrv) d = g_strv_builder_end(disc);
  if (pubkey_hex && d[0]) {
    g_autofree char *rl = ns_net_fetch_replaceable(net, (const char *const *)d, 10002,
                                                   pubkey_hex, cfg->query_timeout_ms, NULL);
    char **w = rl ? nostr_publish_nip65_relays(rl, NOSTR_PUBLISH_NIP65_WRITE, NULL) : NULL;
    if (w && w[0]) return w;
    g_strfreev(w);
  }
  return g_strdupv(cfg->home_relays ? cfg->home_relays : (char *[]){NULL});
}

static const char *upstream_name(NostrPublishUpstream u) {
  switch (u) {
  case NOSTR_PUBLISH_UPSTREAM_SESSION_RELAY_ONLY: return "session_relay_only";
  case NOSTR_PUBLISH_UPSTREAM_DIRECT_ONLY: return "direct_only";
  default: return "session_relay_or_direct";
  }
}

gboolean nseal_publish_file(const char *path, const NsealPublishOptions *opts, GError **error) {
  NsealPublishOptions o = {0};
  if (opts) o = *opts;

  /* Recipients come from the sealed file itself. */
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    int e = errno;
    g_set_error(error, NSEAL_ERROR, NSEAL_ERROR_IO, "%s: %s", path, g_strerror(e));
    return FALSE;
  }
  g_autoptr(NsealHeader) h = nseal_header_read_fd(fd, error);
  close(fd);
  if (!h) return FALSE;
  g_autoptr(GStrvBuilder) rb = g_strv_builder_new();
  for (gsize i = 0; i < nseal_header_n_stanzas(h); i++) {
    g_autofree char *hx = hex32(nseal_header_stanza_recipient(h, i));
    g_strv_builder_add(rb, hx);
  }
  g_auto(GStrv) recipients = g_strv_builder_end(rb);
  if (nseal_header_is_passphrase(h))
    g_printerr("nostr-seal: note: passphrase-sealed file: the event names no recipients\n");

  g_autoptr(NsealConfig) sconf = nseal_config_load(error);
  if (!sconf) return FALSE;
  NsConfig *cfg = ns_config_load(error);
  if (!cfg) return FALSE;
  g_autoptr(GMappedFile) mf = g_mapped_file_new(path, FALSE, error);
  if (!mf) {
    ns_config_free(cfg);
    return FALSE;
  }
  g_autoptr(GBytes) bytes = g_mapped_file_get_bytes(mf);
  gsize len = g_bytes_get_size(bytes);
  if (cfg->max_upload_bytes && len > cfg->max_upload_bytes) {
    g_set_error(error, NSEAL_ERROR, NSEAL_ERROR_ARG,
                "%s is %" G_GSIZE_FORMAT " bytes; the Blossom upload limit is %" G_GUINT64_FORMAT
                " (max_upload_mib in %s)", path, len, cfg->max_upload_bytes, cfg->config_path);
    ns_config_free(cfg);
    return FALSE;
  }
  g_autofree char *sha = ns_sha256_hex(g_bytes_get_data(bytes, NULL), len);

  NsNet net;
  ns_net_init(&net);
  gboolean ok = FALSE;
  g_autofree char *pubkey_hex = NULL;
  g_auto(GStrv) servers = NULL;
  g_auto(GStrv) write = NULL;
  g_auto(GStrv) targets = NULL;
  g_autofree char *servers_src = NULL;
  NostrPublishSigner *signer = NULL;
  g_autofree char *url = NULL;
  GError *e = NULL;

  /* Identity: the signer's (or --identity). A dry run tolerates no signer. */
  {
    g_autoptr(NsealSigner) s = nseal_signer_new(o.identity, &e);
    uint8_t pk[32];
    if (s && nseal_signer_public_key(s, pk, &e)) pubkey_hex = hex32(pk);
  }
  if (!pubkey_hex) {
    if (!o.dry_run) {
      g_propagate_error(error, e);
      goto out;
    }
    g_printerr("nostr-seal: warning: %s (dry run continues without a pubkey)\n", e->message);
    g_clear_error(&e);
  }

  servers = ns_resolve_blossom_servers(cfg, &net, pubkey_hex, &servers_src, &e);
  if (!servers) {
    if (!o.dry_run) {
      g_propagate_error(error, e);
      goto out;
    }
    g_printerr("nostr-seal: warning: %s\n", e->message);
    g_clear_error(&e);
  }

  /* Targets: NIP-65 write relays, filtered by the upstream policy. */
  NostrPublishPolicy policy;
  nostr_publish_policy_init(&policy);
  G_STATIC_ASSERT(NSEAL_UPSTREAM_DIRECT_ONLY == (int)NOSTR_PUBLISH_UPSTREAM_DIRECT_ONLY &&
                  NSEAL_UPSTREAM_SESSION_RELAY_ONLY ==
                      (int)NOSTR_PUBLISH_UPSTREAM_SESSION_RELAY_ONLY);
  policy.upstream = (NostrPublishUpstream)sconf->upstream;
  policy.ok_wait_sec = cfg->ok_wait_sec;
  write = write_relays(cfg, &net, pubkey_hex);
  targets = nostr_publish_policy_select_targets(
      &policy, (const char *const *)write, net.session_socket ? NS_SESSION_RELAY_URL : NULL, &e);
  if (!targets) {
    if (!o.dry_run) {
      g_propagate_prefixed_error(error, e, "upstream_mode=%s: ", upstream_name(policy.upstream));
      goto out;
    }
    g_printerr("nostr-seal: warning: upstream_mode=%s: %s\n", upstream_name(policy.upstream),
               e->message);
    g_clear_error(&e);
  }

  {
    g_autofree char *t = targets ? g_strjoinv(", ", targets) : g_strdup("(none)");
    g_autofree char *b = servers ? g_strjoinv(", ", servers) : g_strdup("(none)");
    g_printerr("recipients: %u (p tags)\nblossom: %s%s%s%s\nupstream_mode: %s\ntargets: %s\n",
               g_strv_length(recipients), b, servers_src ? " (" : "",
               servers_src ? servers_src : "", servers_src ? ")" : "",
               upstream_name(policy.upstream), t);
  }

  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  if (o.dry_run) {
    /* Predicted URL: the first server's BUD-01 address. */
    g_autofree char *predicted = servers ? ns_blossom_blob_url(servers[0], sha, NSEAL_MIME_TYPE)
                                         : g_strdup_printf("https://<blossom-server>/%s", sha);
    g_autofree char *ev = nseal_publish_event_json(pubkey_hex, now, predicted, sha, len,
                                                   (const char *const *)recipients, TRUE);
    printf("%s\n", ev);
    g_printerr("dry run: nothing uploaded, signed or published\n");
    ok = TRUE;
    goto out;
  }

  {
    g_autoptr(GDBusConnection) bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &e);
    signer = bus ? nostr_publish_signer_new_dbus(bus, NSEAL_SIGNER_APP_ID, &e) : NULL;
    if (!signer) {
      g_propagate_prefixed_error(error, e, "signer: ");
      goto out;
    }
  }
  {
    g_printerr("uploading %" G_GSIZE_FORMAT " bytes (approve the Blossom auth in your signer)…\n",
               len);
    g_autofree char *server = NULL;
    /* Account-signed BUD-02 auth; sealed blobs are already ciphertext so a
     * throwaway key buys nothing here.  Plaintext http:// Blossom servers
     * (even loopback) are refused: the URL ends up in a public kind-1063. */
    if (!ns_blossom_upload((const char *const *)servers, signer, pubkey_hex,
                           NS_BLOSSOM_AUTH_ACCOUNT, FALSE, NSEAL_MIME_TYPE,
                           bytes, sha, &url, &server, NULL, error))
      goto out;
    g_printerr("uploaded → %s\n", url);

    g_autofree char *unsigned_json = nseal_publish_event_json(pubkey_hex, now, url, sha, len,
                                                              (const char *const *)recipients,
                                                              FALSE);
    g_autofree char *signed_json =
        nostr_publish_signer_sign_event_json(signer, unsigned_json, NULL, error);
    if (!signed_json) goto out;
    /* The signer signs with its active identity; refuse a different one. */
    {
      g_autofree char *needle = g_strdup_printf("\"pubkey\":\"%s\"", pubkey_hex);
      if (!strstr(signed_json, needle)) {
        g_set_error(error, NSEAL_ERROR, NSEAL_ERROR_SIGNER,
                    "the signer signed with a different identity than %s; select it in the "
                    "signer or drop --identity", o.identity ? o.identity : "the active one");
        goto out;
      }
    }

    NsTargets t = {0};
    t.targets = targets;
    g_autoptr(GStrvBuilder) db = g_strv_builder_new();
    for (guint i = 0; targets[i]; i++) {
      if (g_strcmp0(targets[i], NS_SESSION_RELAY_URL) == 0) t.session_included = TRUE;
      else g_strv_builder_add(db, targets[i]);
    }
    g_auto(GStrv) direct = g_strv_builder_end(db);
    t.direct = direct;
    NsPublishReport report;
    gboolean pub_ok = ns_net_publish(&net, cfg, signed_json, &t, &report, error);
    for (guint i = 0; report.lines && i < report.lines->len; i++)
      g_printerr("  %s\n", (const char *)g_ptr_array_index(report.lines, i));
    ns_publish_report_clear(&report);
    if (!pub_ok) goto out;
    printf("%s\n", signed_json);
    ok = TRUE;
  }

out:
  if (signer) nostr_publish_signer_unref(signer);
  ns_net_clear(&net);
  ns_config_free(cfg);
  return ok;
}

#else /* !NSEAL_HAVE_PUBLISH */

char *nseal_publish_event_json(const char *pubkey_hex, gint64 created_at, const char *url,
                               const char *sha256_hex, guint64 size,
                               const char *const *recipients_hex, gboolean pretty) {
  return NULL;
}

gboolean nseal_publish_file(const char *path, const NsealPublishOptions *opts, GError **error) {
  g_set_error_literal(error, NSEAL_ERROR, NSEAL_ERROR_ARG,
                      "this nostr-seal was built without publishing support "
                      "(needs nostr-share: -DENABLE_NOSTR_SHARE=ON)");
  return FALSE;
}

#endif
