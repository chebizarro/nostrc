#ifndef RELAYD_PROTOCOL_NIP50_H
#define RELAYD_PROTOCOL_NIP50_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <libwebsockets.h>
#include "relayd_ctx.h"
#include "relayd_conn.h"
#include "nostr-filter.h"

/* If any filter has a search query and storage supports search, start iterator.
 * Returns 1 if handled (either started iterator or sent CLOSED), 0 otherwise.
 */
int relayd_nip50_maybe_start_search(struct lws *wsi, ConnState *cs, const RelaydCtx *ctx,
                                    const char *sub, size_t sub_len,
                                    NostrFilter **arr, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* RELAYD_PROTOCOL_NIP50_H */
