/* profile_image.c — parent side of the unprivileged image download.
 *
 * The parent (root) fork/execs nostr-homed-profile-image after
 * setresgid/setresuid to the drop_user. If drop_user is missing from
 * /etc/passwd the download is refused: we never dial the network as
 * root, even for a "safe" URL, because the picture bytes eventually
 * flow through GdkPixbuf loaders whose fuzz history is not our
 * concern to relitigate.
 *
 * The helper's exit code is mapped to nh_profile_rc so callers can
 * distinguish the fail modes documented in nostr_profile.h. */
#define _GNU_SOURCE
#include "nostr_profile.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

/* Best-effort minimal PATH for the exec'd helper. */
static char *const helper_envp[] = {
  (char *)"PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
  NULL,
};

nh_profile_rc nh_profile_download_image(const char *helper_path,
                                        const char *url,
                                        const char *drop_user,
                                        const char *out_png_path) {
  if (!url || !out_png_path) return NH_PROFILE_ERR_ARG;
  if (nh_profile_validate_picture_url(url) != 0) return NH_PROFILE_ERR_URL;

  const char *helper = helper_path && *helper_path
                       ? helper_path
                       : "nostr-homed-profile-image";
  const char *duser = drop_user && *drop_user ? drop_user : "nobody";

  struct passwd *pw = getpwnam(duser);
  if (!pw) return NH_PROFILE_ERR_ARG;
  if (pw->pw_uid == 0) return NH_PROFILE_ERR_ARG;

  /* Ensure the target directory exists (root-owned; the helper writes
   * as unprivileged so the destination directory must be world-writable
   * OR we pre-create with helper's uid). Simplest: write to a temp
   * file in a fixed dir we chown to the drop user, then move it. But
   * accountsservice needs a stable path. Solution: the helper writes
   * to <out>.tmp then renames — the containing directory must be
   * writable by @duser. The convention (docs) is
   * /run/nostr-auth/profile/, which we own+create here mode 0755 and
   * chown to @duser so writes succeed. */
  {
    char *dup = strdup(out_png_path);
    if (!dup) return NH_PROFILE_ERR_IO;
    char *slash = strrchr(dup, '/');
    if (slash) {
      *slash = '\0';
      struct stat st;
      if (stat(dup, &st) != 0) {
        if (mkdir(dup, 0755) != 0 && errno != EEXIST) {
          free(dup); return NH_PROFILE_ERR_IO;
        }
      }
      /* Make writable by the drop user without opening it to the world. */
      if (chown(dup, pw->pw_uid, pw->pw_gid) != 0) {
        /* not fatal — dir may already be writable by that uid; keep going */
      }
      if (chmod(dup, 0755) != 0) { /* best-effort */ }
    }
    free(dup);
  }

  pid_t pid = fork();
  if (pid < 0) return NH_PROFILE_ERR_IO;
  if (pid == 0) {
    /* Child: drop groups + privileges, then exec. */
    if (setgroups(1, &pw->pw_gid) != 0) _exit(70);
    if (setgid(pw->pw_gid) != 0) _exit(70);
    if (setuid(pw->pw_uid) != 0) _exit(70);
    if (getuid() == 0 || geteuid() == 0) _exit(70);

    /* Redirect stdin from /dev/null; leave stdout/stderr as-is so the
     * parent's journal sees any complaints. */
    int devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (devnull >= 0) { dup2(devnull, 0); close(devnull); }

    char *args[] = {
      (char *)helper,
      (char *)url,
      (char *)out_png_path,
      NULL,
    };
    /* If helper_path was absolute, use execve; otherwise execvpe (PATH). */
    if (helper[0] == '/') execve(helper, args, helper_envp);
    else                   execvpe(helper, args, helper_envp);
    _exit(70);
  }

  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno == EINTR) continue;
    return NH_PROFILE_ERR_IO;
  }
  if (!WIFEXITED(status)) return NH_PROFILE_ERR_DOWNLOAD;
  int ec = WEXITSTATUS(status);
  switch (ec) {
    case 0:  return NH_PROFILE_OK;
    case 64: return NH_PROFILE_ERR_URL;
    case 65: return NH_PROFILE_ERR_URL;      /* SSRF peer refused */
    case 66: return NH_PROFILE_ERR_DOWNLOAD;
    case 67: return NH_PROFILE_ERR_DOWNLOAD; /* wrong content-type */
    case 68: return NH_PROFILE_ERR_DECODE;
    case 69: return NH_PROFILE_ERR_IO;
    case 70: return NH_PROFILE_ERR_ARG;      /* couldn't drop privileges */
    default: return NH_PROFILE_ERR_DOWNLOAD;
  }
}
