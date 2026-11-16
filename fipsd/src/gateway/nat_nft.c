/* The libnftables backend of the NAT table (Linux).
 *
 * libnftables over libnftnl/libmnl: the rebuild is one JSON document that
 * nft_run_cmd_from_buffer parses, evaluates and sends as a single netlink
 * batch, which the kernel applies as one transaction, the same guarantee
 * upstream's hand-built rustables batch relies on. libnftables sizes the
 * socket buffers to the batch and collects every acknowledgement itself, so
 * the batch-size and ack bookkeeping upstream does by hand is the library's.
 * A fresh context per batch keeps no ruleset cache between rebuilds. */
#include "gateway/nat.h"
#include <nftables/libnftables.h>
#include <stdio.h>
#include <string.h>

/* The kernel's or the parser's reason, without the echo of the command that
 * failed: only the "Error:" lines, which do not change from batch to batch
 * for one cause, so the latches can compare them. */
static void first_errors(const char *buffer, char *err, size_t cap)
{
    size_t at = 0;
    if (cap) err[0] = 0;
    for (const char *line = buffer; line && *line;) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        if (len >= 6 && !strncmp(line, "Error:", 6) && at + 2 < cap) {
            const char *text = line + 6;
            while (len > 6 && *text == ' ') { ++text; --len; }
            size_t n = len - 6;
            if (at) { err[at++] = ';'; err[at++] = ' '; }
            if (n > cap - at - 1) n = cap - at - 1;
            memcpy(err + at, text, n); at += n; err[at] = 0;
        }
        line = end ? end + 1 : NULL;
    }
    if (!at && buffer && *buffer) snprintf(err, cap, "%.*s", (int)strcspn(buffer, "\n"), buffer);
}
static int apply(void *user, const char *batch, char *err, size_t cap)
{
    (void)user;
    struct nft_ctx *ctx = nft_ctx_new(NFT_CTX_DEFAULT);
    if (!ctx) { snprintf(err, cap, "cannot create an nftables context"); return -1; }
    /* The JSON output flag makes nft_run_cmd_from_buffer parse JSON, as
     * `nft -j -f` does, on every libnftables that has a JSON parser. */
    nft_ctx_output_set_flags(ctx, NFT_CTX_OUTPUT_JSON);
    nft_ctx_buffer_output(ctx);
    nft_ctx_buffer_error(ctx);
    int rc = nft_run_cmd_from_buffer(ctx, batch);
    if (rc != 0) {
        const char *buffer = nft_ctx_get_error_buffer(ctx);
        first_errors(buffer, err, cap);
        if (!*err) snprintf(err, cap, "the kernel refused the batch");
    }
    nft_ctx_free(ctx);
    return rc == 0 ? 0 : -1;
}
fipsd_gw_nft_backend fipsd_gw_nft_system_backend(void) { return (fipsd_gw_nft_backend){apply, NULL}; }
