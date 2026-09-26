/* nd-config.h - nostr-dav daemon configuration
 *
 * SPDX-License-Identifier: MIT
 *
 * Optional keyfile at $XDG_CONFIG_HOME/nostr-dav/nostr-dav.conf:
 *
 *   [nostr-dav]
 *   nostr_dav_upstream_mode=session_relay_or_direct
 *
 * A missing file yields defaults. An unrecognized value is an error:
 * upstream mode is a privacy control, so a typo must not silently widen
 * it to the default.
 */
#ifndef ND_CONFIG_H
#define ND_CONFIG_H

#include <glib.h>

G_BEGIN_DECLS

#define ND_CONFIG_GROUP "nostr-dav"

#define ND_CONFIG_ERROR (nd_config_error_quark())
GQuark nd_config_error_quark(void);

typedef enum {
  ND_CONFIG_ERROR_INVALID_VALUE = 1
} NdConfigError;

/**
 * NdUpstreamMode:
 * @ND_UPSTREAM_MODE_SESSION_RELAY_ONLY: only talk to the session-local
 *   relay; never fall back to the account's home relays.
 * @ND_UPSTREAM_MODE_SESSION_RELAY_OR_DIRECT: prefer the session relay;
 *   fall back to home relays once reconnect backoff is exhausted.
 * @ND_UPSTREAM_MODE_DIRECT_ONLY: talk to home relays directly.
 *
 * Relay upstream policy (plan Track 2 D4). Consumed by the relay sync
 * layer.
 */
typedef enum {
  ND_UPSTREAM_MODE_SESSION_RELAY_ONLY,
  ND_UPSTREAM_MODE_SESSION_RELAY_OR_DIRECT,
  ND_UPSTREAM_MODE_DIRECT_ONLY
} NdUpstreamMode;

#define ND_UPSTREAM_MODE_DEFAULT ND_UPSTREAM_MODE_SESSION_RELAY_OR_DIRECT

/**
 * NdPublishQuorum:
 *
 * How many relays in the resolved target set (NIP-65 write list, or
 * home_relays fallback) must ACK before a pending row transitions to
 * `published`. `all` (default) enforces the NIP-65 outbox commit;
 * numeric values are for operator overrides only.
 */
typedef struct {
  gboolean all;      /* TRUE = every relay in the target set */
  guint    count;    /* used when all == FALSE (>= 1) */
} NdPublishQuorum;

#define ND_PUBLISH_QUORUM_DEFAULT ((NdPublishQuorum){ .all = TRUE, .count = 0 })

typedef struct {
  NdUpstreamMode   upstream_mode;
  NdPublishQuorum  publish_quorum;

  /* Account identity + relay set (v1: single account). Both are optional
   * because a headless enrollment tool may populate the store first and
   * fill these in later. The publish worker requires @account_pubkey to
   * be set (hex64) and at least one entry in @home_relays before it will
   * dispatch anything. */
  gchar   *account_pubkey;   /* owned; hex64 or NULL */
  GStrv    home_relays;      /* owned NULL-terminated array; NULL when
                              * empty (matches g_key_file_get_string_list). */

  /* Gate for the relay subscribe + publish stack (plan Track 2 D4/D5).
   * Defaults to TRUE now that bead nostrc-tu6y has wired a real
   * libsoup 3 WebSocket transport. Setting it FALSE in nostr-dav.conf
   * makes the daemon serve DAV locally and stop staging outbox rows —
   * useful for offline-first workflows or air-gapped smoke testing. */
  gboolean enable_publish;
} NdConfig;

void nd_config_clear(NdConfig *config);

/** Returns: (transfer full): $XDG_CONFIG_HOME/nostr-dav/nostr-dav.conf */
gchar *nd_config_default_path(void);

void nd_config_init_defaults(NdConfig *config);

/**
 * nd_config_load:
 * @path: keyfile path; a missing file leaves defaults in place
 * @config: (out caller-allocates): filled with defaults, then file values
 *
 * Returns: FALSE if the file exists but cannot be parsed or holds an
 *   invalid value.
 */
gboolean nd_config_load(const gchar *path, NdConfig *config, GError **error);

const gchar *nd_upstream_mode_to_string(NdUpstreamMode mode);
gboolean nd_upstream_mode_from_string(const gchar    *str,
                                      NdUpstreamMode *out_mode);

/**
 * nd_publish_quorum_parse:
 * @str: value from config file ("all" or a positive integer)
 * @out_quorum: (out): filled on success
 *
 * Returns: FALSE with @error set if @str is neither `"all"` nor a
 *   positive integer.
 */
gboolean nd_publish_quorum_parse(const gchar     *str,
                                 NdPublishQuorum *out_quorum,
                                 GError         **error);

G_END_DECLS
#endif /* ND_CONFIG_H */
