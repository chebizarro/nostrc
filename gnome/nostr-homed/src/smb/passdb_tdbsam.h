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
 * static ops table below. */
nh_smb_passdb_tdbsam *nh_smb_passdb_tdbsam_new(const char *smbpasswd_path,
                                               const char *pdbedit_path);
void nh_smb_passdb_tdbsam_free(nh_smb_passdb_tdbsam *t);

extern const nh_smb_passdb_ops nh_smb_passdb_tdbsam_ops;

#ifdef __cplusplus
}
#endif

#endif /* NH_SMB_PASSDB_TDBSAM_H */
