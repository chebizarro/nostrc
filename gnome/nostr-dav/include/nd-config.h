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

typedef struct {
  NdUpstreamMode upstream_mode;
} NdConfig;

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

G_END_DECLS
#endif /* ND_CONFIG_H */
