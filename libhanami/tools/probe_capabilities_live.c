/*
 * probe_capabilities_live.c
 *
 * Standalone smoke test for hanami_server_probe_capabilities against
 * real Blossom servers. Intended to be run manually on a workstation
 * with outbound HTTPS (bizarro@192.168.64.3 — see follow-up bead
 * nostrc-ypn2). Not compiled into any installable target.
 *
 * Usage: probe_capabilities_live <endpoint> [<endpoint> ...]
 *
 * Prints one line per (endpoint, capability) so the output can be
 * diffed against the D8 evidence matrix
 * (docs/reviews/porthome-blossom-batch-auth-2026-09-24.md).
 *
 * SPDX-License-Identifier: MIT
 */

#include "hanami/hanami-blossom-client.h"
#include "hanami/hanami-server-capability.h"
#include "hanami/hanami-types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <endpoint> [<endpoint> ...]\n", argv[0]);
        return 2;
    }

    fprintf(stdout, "# hanami capability probe — %ld\n", (long)time(NULL));
    fprintf(stdout, "# ephemeral keys, 1 KiB session-random blobs, ≤4 PUTs/server\n");

    int overall = 0;

    for (int i = 1; i < argc; i++) {
        const char *ep = argv[i];
        hanami_blossom_client_opts_t opts = {
            .endpoint = ep,
            .timeout_seconds = 15,
            .user_agent = "libhanami-probe/0.1 (nostrc-ypn2)"
        };
        hanami_blossom_client_t *cli = NULL;
        hanami_error_t rc = hanami_blossom_client_new(&opts, NULL, &cli);
        if (rc != HANAMI_OK) {
            fprintf(stdout, "%s\tPROBE_ERROR\tclient_new failed rc=%d\n", ep, rc);
            overall = 1;
            continue;
        }

        rc = hanami_server_probe_capabilities(cli);
        const hanami_server_capabilities_t *caps = hanami_blossom_get_capabilities(cli);
        if (!caps) {
            fprintf(stdout, "%s\tPROBE_ERROR\tno caps\n", ep);
            hanami_blossom_client_free(cli);
            overall = 1;
            continue;
        }

        fprintf(stdout, "%s\trc=%d\treachable=%s\tserver_tag_ok=%s\tbatch_ok=%s\tstrict_x=%s\tlast_probe_ts=%lld\tlast_401_ts=%lld\n",
                ep,
                (int)rc,
                caps->reachable ? "true" : "false",
                hanami_capability_state_str(caps->server_tag_ok),
                hanami_capability_state_str(caps->batch_ok),
                hanami_capability_state_str(caps->strict_x_binding),
                (long long)caps->last_probe_ts,
                (long long)caps->last_401_ts);

        hanami_blossom_client_free(cli);
    }
    return overall;
}
