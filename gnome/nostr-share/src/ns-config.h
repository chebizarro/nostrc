/* ns-config.h - nostr-share configuration
 *
 * SPDX-License-Identifier: MIT
 *
 * $NOSTR_SHARE_CONFIG, else $XDG_CONFIG_HOME/nostr-share/nostr-share.conf
 * (GKeyFile, group [nostr-share]). Every key is optional; see
 * data/nostr-share.conf.example for documentation.
 */
#ifndef NS_CONFIG_H
#define NS_CONFIG_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  /* Default. Publish to the NIP-65 write relays; when the session relay
   * socket exists it also receives a copy so local apps see the event
   * immediately. Success is judged on the direct relays only, because
   * the session relay does not federate upstream yet. */
  NS_UPSTREAM_SESSION_RELAY_AND_DIRECT = 0,
  /* Session relay when present, else direct. Only meaningful once the
   * session relay federates writes upstream. */
  NS_UPSTREAM_SESSION_RELAY_OR_DIRECT,
  /* Session relay only; never contact home relays from this process. */
  NS_UPSTREAM_SESSION_RELAY_ONLY,
  /* Never use the session relay. */
  NS_UPSTREAM_DIRECT_ONLY,
} NsUpstreamMode;

typedef struct {
  gchar         **home_relays;        /* discovery + fallback write relays */
  gchar         **blossom_servers;    /* fallback when no kind 10063 */
  NsUpstreamMode  upstream;
  guint64         max_upload_bytes;
  guint           ok_wait_sec;
  guint           query_timeout_ms;
  gchar          *dav_url;            /* NULL: ask `nostr-dav --show-credentials` */
  gint            text_kind;          /* default kind for plain text: 0 (= 1) | 1 | 30023 */
  gboolean        keep_metadata;      /* default for --keep-metadata */
  gchar          *config_path;        /* for messages */
} NsConfig;

NsConfig *ns_config_load(GError **error);
void      ns_config_free(NsConfig *cfg);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(NsConfig, ns_config_free)

const gchar *ns_upstream_mode_name(NsUpstreamMode mode);
gboolean     ns_upstream_mode_parse(const gchar *s, NsUpstreamMode *out);

G_END_DECLS

#endif /* NS_CONFIG_H */
