#ifndef NH_FSCRYPT_COPY_H
#define NH_FSCRYPT_COPY_H
/* Offline, root-only maintenance. Never follows symlinks or crosses mounts.
 * Refuses hardlinks/special files rather than losing or splitting their
 * semantics. Preserves regular files, symlinks, ownership, modes, times and
 * xattrs (including ACLs). Source/destination root fds remain caller-owned. */
int nh_fscrypt_copy_tree(int source, int destination);
int nh_fscrypt_remove_tree(int directory);
#endif
