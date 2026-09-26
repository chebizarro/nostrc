/*
 * Real passdb adapter for the SMB credential authority.  See
 * passdb_tdbsam.c for the shell-out contract and constraints.
 *
 * The core (smb_credential.c) does not link this file directly; the
 * daemon wires it in.  This header is provided so integration tests
 * running on a Samba VM can pick up the same adapter without
 * duplicating declarations.
 *
 * beads nostrc-rb0e.6.
 */
#ifndef NH_SMB_PASSDB_TDBSAM_H
#define NH_SMB_PASSDB_TDBSAM_H

#include "smb_credential.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nh_smb_passdb_tdbsam nh_smb_passdb_tdbsam;

/* Construct an adapter context.  NULL paths select the compile-time
 * defaults (/usr/bin/smbpasswd, /usr/bin/pdbedit).  Returned pointer
 * must be freed with nh_smb_passdb_tdbsam_free().  Pass the pointer as
 * the `passdb_ctx` argument to nh_smb_authority_open() alongside the
 * static ops table below.
 *
 * `smb_conf_path` (plan §4.2 B3, 2026-09-25) selects the DEDICATED
 * standalone Samba config the adapter forwards to smbpasswd (via
 * `-c <path>`) and pdbedit (via `-s <path>`) on every invocation.
 * A NULL / empty value keeps the pre-Wave-3 behaviour (no config
 * selector — backwards-compat for portable tests that never touch
 * a real Samba install).  Callers on the shipping path MUST pass
 * `/etc/nostr-auth/smb.conf` (or the operator override loaded from
 * `smb-credentiald.conf`) so the passdb writes land in the
 * dedicated tdbsam, not the host default. */
nh_smb_passdb_tdbsam *nh_smb_passdb_tdbsam_new(const char *smbpasswd_path,
                                               const char *pdbedit_path,
                                               const char *smb_conf_path);
void nh_smb_passdb_tdbsam_free(nh_smb_passdb_tdbsam *t);

extern const nh_smb_passdb_ops nh_smb_passdb_tdbsam_ops;

#ifdef __cplusplus
}
#endif

#endif /* NH_SMB_PASSDB_TDBSAM_H */
