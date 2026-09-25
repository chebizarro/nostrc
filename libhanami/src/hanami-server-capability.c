/*
 * hanami-server-capability.c
 *
 * SPDX-License-Identifier: MIT
 */

#include "hanami/hanami-server-capability.h"

#include <string.h>

void hanami_server_capabilities_init(hanami_server_capabilities_t *caps)
{
    if (!caps) return;
    memset(caps, 0, sizeof(*caps));
    caps->batch_ok          = HANAMI_CAP_UNKNOWN;
    caps->server_tag_ok     = HANAMI_CAP_UNKNOWN;
    caps->strict_x_binding  = HANAMI_CAP_UNKNOWN;
    caps->raw_random_ok     = HANAMI_CAP_UNKNOWN;
    caps->png_shim_ok       = HANAMI_CAP_UNKNOWN;
    caps->last_probe_ts     = 0;
    caps->last_401_ts       = 0;
    caps->reachable         = false;
}

const char *hanami_capability_state_str(hanami_capability_state_t s)
{
    switch (s) {
    case HANAMI_CAP_UNKNOWN: return "unknown";
    case HANAMI_CAP_YES:     return "yes";
    case HANAMI_CAP_NO:      return "no";
    default:                 return "?";
    }
}
