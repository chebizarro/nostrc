/*
 * nh_syncd_interlocks.c — refuse-to-push checks (§6.4).
 *
 * SPDX-License-Identifier: MIT
 *
 * The snapshot-base check is deferred to the pusher (needs the state
 * pointer). Everything else is a stateless probe: is the limited-mode
 * file present? is NOSTR_HOME_STATE=partial?
 */

#include "nh_syncd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int nh_syncd_interlocks_check(const nh_syncd_interlocks *ilk) {
    if (!ilk || !ilk->home_dir) return NH_SYNCD_ERR_ARG;

    /* ~/.nostr-home-limited (design §5.3). */
    size_t n = strlen(ilk->home_dir) + strlen("/.nostr-home-limited") + 1;
    char *p = malloc(n);
    if (!p) return NH_SYNCD_ERR_OOM;
    snprintf(p, n, "%s/.nostr-home-limited", ilk->home_dir);
    struct stat st;
    int limited = (stat(p, &st) == 0);
    free(p);
    if (limited) return NH_SYNCD_ERR_LIMITED_MODE;

    /* NOSTR_HOME_STATE=partial (design §6.4 bullet 2). */
    if (ilk->nostr_home_state && !strcmp(ilk->nostr_home_state, "partial"))
        return NH_SYNCD_ERR_PARTIAL_STATE;

    return NH_SYNCD_OK;
}
