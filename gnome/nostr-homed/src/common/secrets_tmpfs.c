#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

int nh_secrets_mount_tmpfs(const char *path){
  if (!path) return -1;
  if (mkdir(path, 0700) != 0 && errno != EEXIST) return -1;
  /* Best-effort tmpfs mount; on systems without mount perms, fallback to chmod */
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
  if (mount("tmpfs", path, 0, NULL) != 0) {
#else
  if (mount("tmpfs", path, "tmpfs", 0, "size=64M,mode=0700") != 0) {
#endif
    /* Fallback: ensure perms */
    (void)chmod(path, 0700);
  }
  return 0;
}
