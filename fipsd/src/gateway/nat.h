#ifndef FIPSD_GATEWAY_NAT_H
#define FIPSD_GATEWAY_NAT_H
/* NAT rule management, the port of upstream src/gateway/nat.rs
 * (nostrc-iq1n9.3).
 *
 * All gateway rules live in one nftables table, `inet fips_gateway`, rebuilt
 * whole on every change in one transaction: add the table, delete it, add it
 * again, then the chains, the mesh-source set and the full rule set. Because
 * the delete and the recreate share one transaction the table never leaves
 * the packet path, and a batch the kernel refuses leaves the previous table
 * in place. Upstream builds the batch with rustables (libnftnl's netlink
 * encoding); this port renders it as libnftables JSON and hands it to
 * nft_run_cmd_from_buffer, which sends it as one netlink batch. The renderer
 * is pure and portable, so the rule shapes are golden-tested anywhere; only
 * the libnftables backend (nat_nft.c) is Linux.
 *
 * Rebuilds run on a NAT worker thread that owns the table, never on the
 * fiber that answers `.fips` queries: the worker drains every queued change
 * into one rebuild, retries a refused rebuild every ten seconds, and reports
 * removals the kernel has applied. */
#include "gateway/types.h"
#include <stdatomic.h>

#define FIPSD_GW_NAT_TABLE "fips_gateway"
#define FIPSD_GW_NAT_MESH_SOURCE_SET "fips_mesh_sources"
/* How long shutdown waits for the table to be deleted, and how often the
 * worker retries a refused rebuild. */
#define FIPSD_GW_NAT_STOP_TIMEOUT_MS 10000u
#define FIPSD_GW_NAT_RETRY_INTERVAL_MS 10000u

typedef enum fipsd_gw_proto { FIPSD_GW_PROTO_TCP, FIPSD_GW_PROTO_UDP } fipsd_gw_proto;
/* An inbound forward: fips0:listen_port/proto to [target]:target_port. */
typedef struct fipsd_gw_port_forward {
    uint16_t listen_port;
    fipsd_gw_proto proto;
    struct in6_addr target;
    uint16_t target_port;
} fipsd_gw_port_forward;
/* Parse `[ipv6]:port`. */
bool fipsd_gw_parse_target(const char *text, struct in6_addr *addr, uint16_t *port);

typedef enum fipsd_gw_nat_error_kind {
    FIPSD_GW_NAT_OK = 0,
    FIPSD_GW_NAT_NFTABLES,       /* the backend refused the batch; message holds why */
    FIPSD_GW_NAT_RULE_NOT_FOUND, /* addr has no mapping (to the expected node) */
    FIPSD_GW_NAT_WORKER_STOPPED
} fipsd_gw_nat_error_kind;
typedef struct fipsd_gw_nat_error {
    fipsd_gw_nat_error_kind kind;
    struct in6_addr addr;
    char message[512];
} fipsd_gw_nat_error;
/* Upstream's Display text. */
const char *fipsd_gw_nat_error_text(const fipsd_gw_nat_error *e, char *buf, size_t cap);

/* Applies a rendered batch: 0, or -1 with the reason in err. */
typedef int (*fipsd_gw_nft_apply_fn)(void *user, const char *batch, char *err, size_t cap);
typedef struct fipsd_gw_nft_backend { fipsd_gw_nft_apply_fn apply; void *user; } fipsd_gw_nft_backend;
/* The libnftables backend (nat_nft.c, Linux builds of fips-gateway only). */
fipsd_gw_nft_backend fipsd_gw_nft_system_backend(void);

typedef enum fipsd_gw_nat_command_kind { FIPSD_GW_NAT_ADD, FIPSD_GW_NAT_REMOVE } fipsd_gw_nat_command_kind;
typedef struct fipsd_gw_nat_command {
    fipsd_gw_nat_command_kind kind;
    struct in6_addr virtual_ip, mesh_addr;
} fipsd_gw_nat_command;
typedef struct fipsd_gw_ip_pair { struct in6_addr virtual_ip, mesh_addr; } fipsd_gw_ip_pair;
typedef struct fipsd_gw_pairs { fipsd_gw_ip_pair *items; size_t len, cap; } fipsd_gw_pairs;
int fipsd_gw_pairs_push(fipsd_gw_pairs *pairs, const struct in6_addr *virtual_ip, const struct in6_addr *mesh_addr);
void fipsd_gw_pairs_clear(fipsd_gw_pairs *pairs);
/* What applying commands did: each command's outcome, in order, and the
 * pairs whose rules are now gone from the kernel table. */
typedef struct fipsd_gw_nat_applied {
    fipsd_gw_nat_command *commands;
    fipsd_gw_nat_error *outcomes;
    size_t len;
    fipsd_gw_pairs removed;
} fipsd_gw_nat_applied;
void fipsd_gw_nat_applied_clear(fipsd_gw_nat_applied *applied);

/* ---- the table manager ------------------------------------------------ */
typedef struct fipsd_gw_nat fipsd_gw_nat;
/* The manager's state without touching the backend (upstream with_state_in). */
fipsd_gw_nat *fipsd_gw_nat_new_state(const char *table, const char *lan_interface, const char *tun_interface,
                                     const struct in6_addr *pool_network, uint8_t prefix_len, fipsd_gw_nft_backend backend);
/* Create the table: new_state and a first rebuild. NULL with err on failure. */
fipsd_gw_nat *fipsd_gw_nat_new(const char *lan_interface, const char *tun_interface, const struct in6_addr *pool_network,
                               uint8_t prefix_len, fipsd_gw_nft_backend backend, fipsd_gw_nat_error *err);
void fipsd_gw_nat_free(fipsd_gw_nat *nat);
/* Replace the inbound forwards and rebuild. */
int fipsd_gw_nat_set_port_forwards(fipsd_gw_nat *nat, const fipsd_gw_port_forward *forwards, size_t count, fipsd_gw_nat_error *err);
/* A failed allocation lost a change or a removal report: the desired state
 * no longer matches what the pool was told, so the table must not be used
 * further (upstream aborts the process on allocation failure). */
#define FIPSD_GW_NAT_FATAL (-2)
/* Apply every command to the desired state, then rebuild once: 0, or -1 when
 * memory ran out (fatal as above; *out still holds what was applied and is
 * the caller's to clear). */
int fipsd_gw_nat_apply(fipsd_gw_nat *nat, const fipsd_gw_nat_command *commands, size_t count, fipsd_gw_nat_applied *out);
/* Retry a refused rebuild with the latest desired state: 1 applied, 0 nothing
 * pending, -1 failed (err), FIPSD_GW_NAT_FATAL when memory ran out. Confirmed
 * removals are appended to removed. */
int fipsd_gw_nat_retry_pending(fipsd_gw_nat *nat, fipsd_gw_pairs *removed, fipsd_gw_nat_error *err);
size_t fipsd_gw_nat_mapping_count(const fipsd_gw_nat *nat);
bool fipsd_gw_nat_rebuild_pending(const fipsd_gw_nat *nat);
/* Delete the table. */
int fipsd_gw_nat_cleanup(fipsd_gw_nat *nat, fipsd_gw_nat_error *err);
/* The batch a rebuild sends, and the one a cleanup sends; malloc'd JSON. */
char *fipsd_gw_nat_render(const fipsd_gw_nat *nat);
char *fipsd_gw_nat_render_cleanup(const fipsd_gw_nat *nat);
/* Test seams: map a pair in the desired state, and rebuild with a stand-in. */
void fipsd_gw_nat_test_set_mapping(fipsd_gw_nat *nat, const struct in6_addr *virtual_ip, const struct in6_addr *mesh_addr);
int fipsd_gw_nat_test_remove_mapping(fipsd_gw_nat *nat, const struct in6_addr *virtual_ip, fipsd_gw_nat_error *err);

/* ---- the table as the driver sees it ----------------------------------- */
/* The real manager, or a test fake. */
typedef struct fipsd_gw_nat_table {
    int (*apply)(void *self, const fipsd_gw_nat_command *commands, size_t count, fipsd_gw_nat_applied *out);
    int (*retry_pending)(void *self, fipsd_gw_pairs *removed, fipsd_gw_nat_error *err);
    size_t (*mapping_count)(void *self);
    int (*cleanup)(void *self, fipsd_gw_nat_error *err);
    void (*destroy)(void *self);
    void *self;
} fipsd_gw_nat_table;
/* A table over a manager, which it takes. */
fipsd_gw_nat_table fipsd_gw_nat_table_of(fipsd_gw_nat *nat);

/* ---- outcome latches --------------------------------------------------- */
typedef enum fipsd_gw_retry_report {
    FIPSD_GW_RETRY_FAILED,    /* a failure that differs from the last outcome, or the first */
    FIPSD_GW_RETRY_REPEATED,  /* the same failure as the last */
    FIPSD_GW_RETRY_RECOVERED, /* the first success after a failure */
    FIPSD_GW_RETRY_CLEAN      /* a success with no failure before it */
} fipsd_gw_retry_report;
typedef struct fipsd_gw_retry_log { bool failed; char last[512]; } fipsd_gw_retry_log;
/* failure NULL is a success. */
fipsd_gw_retry_report fipsd_gw_retry_log_observe(fipsd_gw_retry_log *log, const char *failure);
fipsd_gw_retry_report fipsd_gw_retry_log_unchanged(const fipsd_gw_retry_log *log);
/* The failure as the latches compare it. */
const char *fipsd_gw_nat_latch_key(const fipsd_gw_nat_error *e, char *buf, size_t cap);
fipsd_gw_retry_report fipsd_gw_nat_report_apply(fipsd_gw_retry_log *log, const fipsd_gw_nat_command *commands, size_t count,
                                                const fipsd_gw_nat_applied *applied);
void fipsd_gw_nat_report_retry(fipsd_gw_retry_log *log, int result, const fipsd_gw_nat_error *err);

/* ---- the worker -------------------------------------------------------- */
typedef struct fipsd_gw_nat_driver fipsd_gw_nat_driver;
/* Called once the worker stops applying changes without a shutdown or drop
 * having asked it to: on the worker thread, or at once from start when the
 * worker cannot be started. A worker that outlives a shutdown's timeout
 * never calls it, so its target may be freed once shutdown returns. */
typedef void (*fipsd_gw_nat_exit_fn)(void *user);
/* Take ownership of table and start the worker. When the thread cannot be
 * started the exit callback runs at once and every submit fails. The worker
 * stops by itself when apply fails or retry_pending is FIPSD_GW_NAT_FATAL. */
fipsd_gw_nat_driver *fipsd_gw_nat_driver_start(fipsd_gw_nat_table table, fipsd_gw_nat_exit_fn on_exit, void *user);
/* Queue one change: 0, or -1 when the worker has stopped. */
int fipsd_gw_nat_driver_submit(fipsd_gw_nat_driver *driver, const fipsd_gw_nat_command *command);
size_t fipsd_gw_nat_driver_mapping_count(const fipsd_gw_nat_driver *driver);
/* Move the removals the kernel has applied into out. */
void fipsd_gw_nat_driver_take_removed(fipsd_gw_nat_driver *driver, fipsd_gw_pairs *out);
/* Stop and delete the table, waiting at most timeout_ms: 0, -1 with err, or 1
 * when the timeout left the worker behind (with the driver leaked to it); a
 * timeout is not an error, as upstream's, but that worker may still log.
 * Frees the driver. */
int fipsd_gw_nat_driver_shutdown(fipsd_gw_nat_driver *driver, unsigned timeout_ms, fipsd_gw_nat_error *err);
/* End the worker without deleting the table (a driver dropped without a
 * shutdown), and free the driver. */
void fipsd_gw_nat_driver_drop(fipsd_gw_nat_driver *driver);
#endif
