/*
 * probe_capabilities_live.c
 *
 * Standalone smoke test for blossom_probe_capabilities against
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

#include "blossom/client.h"
#include "blossom/capability.h"
#include <blossom/types.h>

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

    fprintf(stdout, "# libblossom capability probe — %ld\n", (long)time(NULL));
    fprintf(stdout, "# ephemeral keys, 1 KiB session-random blobs, 6 PUTs/server (plus best-effort deletes)\n");

    int overall = 0;

    for (int i = 1; i < argc; i++) {
        const char *ep = argv[i];
        blossom_client_opts_t opts = {
            .endpoint = ep,
            .timeout_seconds = 15,
            .user_agent = "libblossom-probe/0.1 (nostrc-ypn2)"
        };
        blossom_client_t *cli = NULL;
        blossom_error_t rc = blossom_client_new(&opts, NULL, &cli);
        if (rc != BLOSSOM_OK) {
            fprintf(stdout, "%s\tPROBE_ERROR\tclient_new failed rc=%d\n", ep, rc);
            overall = 1;
            continue;
        }

        rc = blossom_probe_capabilities(cli);
        const blossom_server_capabilities_t *caps = blossom_client_get_capabilities(cli);
        if (!caps) {
            fprintf(stdout, "%s\tPROBE_ERROR\tno caps\n", ep);
            blossom_client_free(cli);
            overall = 1;
            continue;
        }

        fprintf(stdout, "%s\trc=%d\treachable=%s\tserver_tag_ok=%s\tbatch_ok=%s\tstrict_x=%s\tlast_probe_ts=%lld\tlast_401_ts=%lld\n",
                ep,
                (int)rc,
                caps->reachable ? "true" : "false",
                blossom_capability_state_str(caps->server_tag_ok),
                blossom_capability_state_str(caps->batch_ok),
                blossom_capability_state_str(caps->strict_x_binding),
                (long long)caps->last_probe_ts,
                (long long)caps->last_401_ts);

        fprintf(stdout, "%s\traw=%s\tpng=%s\tprivate=%s\tmax_size=%llu\n", ep,
                blossom_capability_state_str(caps->raw_random_ok),
                blossom_capability_state_str(caps->png_shim_ok),
                blossom_capability_state_str(caps->private_server),
                (unsigned long long)caps->max_size);
        if (rc != BLOSSOM_OK) overall = 1;
        blossom_client_free(cli);
    }
    return overall;
}
