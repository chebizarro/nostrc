#ifndef FIPSD_DNS_HOSTS_H
#define FIPSD_DNS_HOSTS_H
/* Host-to-npub static mapping, the port of upstream src/hosts.rs
 * (nostrc-9nxwk.3). A host map resolves human-readable hostnames to Nostr
 * public keys (npubs), populated from two sources: peer `alias` fields in the
 * YAML configuration, and an operator-maintained hosts file. The DNS resolver
 * checks the host map before falling back to direct npub resolution, so
 * `gateway.fips` works instead of `npub1...xyz.fips`. The reloader tracks the
 * hosts file's modification time and re-reads it on change; a file that does
 * not read leaves the names last read in place and is read again on the
 * next check. */
#include "config/config.h"
#include <fips/identity/encoding.h>
#include <fips/identity/peer.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Default path of the hosts file. macOS and FreeBSD packaging ships config
 * under /usr/local/etc/fips/; Linux keeps /etc/fips/ (the same literals as
 * fipsd_acl_default_paths, which reads the file for ACL aliases). */
#if defined(__APPLE__) || defined(__FreeBSD__)
#define FIPSD_HOSTS_DEFAULT_PATH "/usr/local/etc/fips/hosts"
#else
#define FIPSD_HOSTS_DEFAULT_PATH "/etc/fips/hosts"
#endif

typedef struct fipsd_hosts_entry {
    char name[64];                   /* lowercase hostname */
    char npub[FIPS_NIP19_CAPACITY];  /* as given */
    fips_node_addr node_addr;        /* for reverse display lookups */
} fipsd_hosts_entry;
/* Entries in insertion order; a reinserted name moves to the end. */
typedef struct fipsd_hosts_map {
    fipsd_hosts_entry *entries;
    size_t count, capacity;
} fipsd_hosts_map;

void fipsd_hosts_map_init(fipsd_hosts_map *m);
void fipsd_hosts_map_clear(fipsd_hosts_map *m);
/* dst must be initialized; its entries are replaced. False on allocation failure. */
bool fipsd_hosts_map_copy(fipsd_hosts_map *dst, const fipsd_hosts_map *src);
/* dst must be initialized; its entries are replaced; src is left empty. */
void fipsd_hosts_map_move(fipsd_hosts_map *dst, fipsd_hosts_map *src);
/* ASCII alphanumerics and hyphens only, 1-63 characters, not starting or
 * ending with a hyphen, not starting with `npub1` (ambiguous with npub
 * resolution). The reason of a refusal is written to reason when given. */
bool fipsd_hosts_validate_hostname(const char *hostname, char *reason, size_t cap);
/* Validates the hostname and the npub, then stores the hostname lowercase for
 * case-insensitive matching. 0 on success; -1 with errno EINVAL and the
 * reason, or ENOMEM. */
int fipsd_hosts_map_insert(fipsd_hosts_map *m, const char *hostname, const char *npub, const fips_secp *secp,
                           char *reason, size_t reason_cap);
const char *fipsd_hosts_map_lookup_npub(const fipsd_hosts_map *m, const char *hostname);
const char *fipsd_hosts_map_lookup_hostname(const fipsd_hosts_map *m, const fips_node_addr *node_addr);
size_t fipsd_hosts_map_len(const fipsd_hosts_map *m);
/* Merge other into m; other wins on conflicts. False on allocation failure. */
bool fipsd_hosts_map_merge(fipsd_hosts_map *m, const fipsd_hosts_map *other);
/* The host map of the configured peers' alias fields; invalid ones are
 * skipped, warned about unless quiet. out must be initialized. */
bool fipsd_hosts_map_from_config(fipsd_hosts_map *out, const fipsd_config *config, const fips_secp *secp, bool quiet);
/* Load a hosts file into out (initialized; replaced), reporting read
 * failures: an absent file is a policy, not a fault, and gives an empty map
 * and 0. Anything else (no read permission, an I/O error, non-UTF-8 content,
 * or a NotFound that contradicts a successful stat and so means the file is
 * being rewritten under us) returns -1 with errno so the caller keeps
 * whatever it loaded last. Lines that are not `hostname npub` are warned
 * about and skipped. */
int fipsd_hosts_map_load_file(fipsd_hosts_map *out, const char *path, const fips_secp *secp);
/* As load_file, but a read failure is logged and also yields an empty map. */
void fipsd_hosts_map_load(fipsd_hosts_map *out, const char *path, const fips_secp *secp);

/* A file's modification time; present false when it does not exist or its
 * metadata cannot be read. */
typedef struct fipsd_hosts_stamp { bool present; int64_t secs, nanos; } fipsd_hosts_stamp;
fipsd_hosts_stamp fipsd_hosts_file_mtime(const char *path);
bool fipsd_hosts_stamp_equal(fipsd_hosts_stamp a, fipsd_hosts_stamp b);

/* Tracks a hosts file and reloads it when the modification time changes.
 * Holds the base map (peer aliases), the hosts file as last read and the
 * current effective map (base merged with the file; the file wins). */
typedef struct fipsd_hosts_reloader {
    fipsd_hosts_map base, file, effective;
    char *path;
    fipsd_hosts_stamp last_mtime;  /* absent when the file did not exist or did not read */
    bool failing;                  /* the last read failed: warn once per run of failures */
    const fips_secp *secp;
} fipsd_hosts_reloader;
/* Performs the initial load of the hosts file and merges it over base, which
 * is moved into the reloader. False on allocation failure (base is cleared). */
bool fipsd_hosts_reloader_init(fipsd_hosts_reloader *r, fipsd_hosts_map *base, const char *path, const fips_secp *secp);
void fipsd_hosts_reloader_clear(fipsd_hosts_reloader *r);
const fipsd_hosts_map *fipsd_hosts_reloader_hosts(const fipsd_hosts_reloader *r);
const char *fipsd_hosts_reloader_path(const fipsd_hosts_reloader *r);
/* Check whether the hosts file has been modified and reload if so. A read
 * that fails keeps the map last read and records no mtime, so the next call
 * reads the file again: a permission fix, which leaves the mtime alone, is
 * picked up. The first failure of a run is logged at warn and the rest at
 * debug; the read that ends the run is logged at info. True if reloaded. */
bool fipsd_hosts_reloader_check_reload(fipsd_hosts_reloader *r);
/* The same check, reporting read failures: 1 reloaded, 0 unchanged, -1 with
 * errno on a failed read (state untouched, so the next call retries). */
int fipsd_hosts_reloader_try_check_reload(fipsd_hosts_reloader *r);
/* Replace the peer-alias base (moved in) and rebuild the effective map over
 * the hosts file as last read, which still wins. Reads no file and leaves
 * the recorded mtime alone. False on allocation failure. */
bool fipsd_hosts_reloader_set_base(fipsd_hosts_reloader *r, fipsd_hosts_map *base);
#endif
