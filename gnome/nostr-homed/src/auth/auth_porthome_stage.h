#ifndef NH_AUTH_PORTHOME_STAGE_H
#define NH_AUTH_PORTHOME_STAGE_H
#include <stdint.h>
#include <sys/types.h>

/* The installer keeps ownership and inode custody of stage_fd. Only the
 * workspace returned here is handed to the unprivileged helper. */
int nh_auth_porthome_stage_open(int stage_fd);
/* Reclaim workspace on every helper outcome. Import only after successful
 * fetch, copying to new account-owned inodes rather than installing inodes
 * to which the helper may retain writable descriptors. Failure preserves
 * the partial stage for the identity layer's repair-required handling. */
int nh_auth_porthome_stage_reclaim(int workspace_fd);
int nh_auth_porthome_stage_import(int stage_fd, int workspace_fd,
                                  uid_t uid, gid_t gid, uint64_t max_bytes);
#endif
