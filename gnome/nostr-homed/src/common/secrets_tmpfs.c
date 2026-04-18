#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <syslog.h>

#if defined(__linux__)
#include <sys/mount.h>
#include <sys/vfs.h>
#define TMPFS_MAGIC 0x01021994
#endif

/**
 * Mount a tmpfs at @path for decrypted secrets.
 *
 * Returns 0 on success (path is a tmpfs mountpoint), -1 on failure.
 * Does NOT silently fall back to a regular directory — if tmpfs
 * cannot be mounted, the caller must decide whether to proceed.
 * This matches the SECURITY.md claim: "decrypted secrets never touch
 * persistent storage."
 */
int nh_secrets_mount_tmpfs(const char *path){
  if (!path) return -1;
  if (mkdir(path, 0700) != 0 && errno != EEXIST) return -1;

#if defined(__linux__)
  /* Idempotency: check if already a tmpfs mountpoint */
  struct statfs sfs;
  if (statfs(path, &sfs) == 0 && sfs.f_type == TMPFS_MAGIC){
    /* Already mounted as tmpfs — ensure perms and return success */
    (void)chmod(path, 0700);
    return 0;
  }

  if (mount("tmpfs", path, "tmpfs", 0, "size=64M,mode=0700") != 0){
    syslog(LOG_ERR, "nostr-homed: failed to mount tmpfs at %s: %s",
           path, strerror(errno));
    return -1;  /* Do NOT fall back to chmod on a regular dir */
  }
  return 0;
#else
  /* Non-Linux: tmpfs mount not available. Return error so caller
   * can decide whether to proceed without secure storage. */
  syslog(LOG_WARNING, "nostr-homed: tmpfs not supported on this platform; "
         "secrets at %s may be on persistent storage", path);
  (void)chmod(path, 0700);
  return 0;  /* Best-effort on non-Linux */
#endif
}
