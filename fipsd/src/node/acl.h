#ifndef FIPSD_NODE_ACL_H
#define FIPSD_NODE_ACL_H
#include "config/config.h"
#include <fips/identity/peer.h>
#include <jansson.h>
/* Immutable policy. No YAML ACL keys: upstream uses peers.allow, peers.deny,
 * hosts, and peers[].alias. Explicit paths are a host/test embedding seam. */
typedef struct fipsd_acl fipsd_acl;
typedef struct fipsd_acl_paths { const char *allow, *deny, *hosts; } fipsd_acl_paths;
typedef enum fipsd_acl_decision { FIPSD_ACL_ALLOW_LIST, FIPSD_ACL_DENY_LIST, FIPSD_ACL_DEFAULT_ALLOW } fipsd_acl_decision;
fipsd_acl_paths fipsd_acl_default_paths(void);
/* old==NULL: startup. A read fault starts empty/stale, as upstream does.
 * Reload faults retain the last policy, including its last-good mtimes.
 * A clean in-place emptying is held once; deletion applies immediately. */
fipsd_acl *fipsd_acl_load(const fipsd_config *, const fipsd_acl_paths *, const fipsd_acl *old);
fipsd_acl *fipsd_acl_clone(const fipsd_acl *);
void fipsd_acl_free(fipsd_acl *);
bool fipsd_acl_changed(const fipsd_acl *);
bool fipsd_acl_stale(const fipsd_acl *);
fipsd_acl_decision fipsd_acl_check(const fipsd_acl *, const fips_peer_identity *);
/* Owned upstream show_acl data (without envelope); no C-only output fields. */
json_t *fipsd_acl_status(const fipsd_acl *);
#endif
