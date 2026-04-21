/*
 * NIP-46 Default Relay Configuration
 *
 * Provides a configurable list of fallback NIP-46 relays used when no
 * relay is specified in a bunker URI or stored in GSettings.
 *
 * Configuration priority:
 *   1. GNOSTR_NIP46_RELAY environment variable (single URL)
 *   2. Built-in fallback list (multiple relays for redundancy)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NIP46_RELAY_DEFAULTS_H
#define NIP46_RELAY_DEFAULTS_H

#include <glib.h>
#include <stdlib.h>

/* Well-known NIP-46 relays. Multiple entries provide redundancy if one
 * goes offline. The first entry is preferred for nostrconnect:// URIs. */
static const char *const NIP46_FALLBACK_RELAYS[] = {
  "wss://relay.nsec.app",
  "wss://relay.damus.io",
  "wss://nos.lol",
  NULL  /* sentinel */
};

#define NIP46_FALLBACK_RELAY_COUNT 3

/**
 * nip46_get_default_relay:
 *
 * Returns the preferred NIP-46 relay URL. Checks the GNOSTR_NIP46_RELAY
 * environment variable first, then falls back to the first entry in the
 * built-in relay list.
 *
 * The returned string must not be freed.
 *
 * Returns: (transfer none): A relay URL string
 */
static inline const char *
nip46_get_default_relay(void)
{
  const char *env = g_getenv("GNOSTR_NIP46_RELAY");
  if (env && *env)
    return env;
  return NIP46_FALLBACK_RELAYS[0];
}

/**
 * nip46_get_fallback_relays:
 * @out_count: (out): Number of relay URLs returned
 *
 * Returns the full list of fallback NIP-46 relay URLs. If the
 * GNOSTR_NIP46_RELAY environment variable is set, returns only that
 * single relay; otherwise returns the built-in list.
 *
 * The returned array and strings must not be freed.
 *
 * Returns: (transfer none): A NULL-terminated array of relay URL strings
 */
static inline const char *const *
nip46_get_fallback_relays(gsize *out_count)
{
  const char *env = g_getenv("GNOSTR_NIP46_RELAY");
  if (env && *env) {
    /* Return a static single-element array for the env override */
    static const char *env_relay[2] = { NULL, NULL };
    env_relay[0] = env;
    if (out_count) *out_count = 1;
    return env_relay;
  }
  if (out_count) *out_count = NIP46_FALLBACK_RELAY_COUNT;
  return NIP46_FALLBACK_RELAYS;
}

#endif /* NIP46_RELAY_DEFAULTS_H */
