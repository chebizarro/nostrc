/* Greeter-artifact confidentiality (beads nostrc-3c7n, references
 * nostrc-zcll.6).
 *
 * Assert that nh_broker_greeter_artifact_write() publishes the drop
 * directory and every file it lays down with the confidentiality
 * posture documented in
 * docs/reviews/greeter-artifact-secret-confidentiality-2026-09-22.md:
 *
 *   /run/nostr-auth/greeter/           0750  root:nostr-auth-greeter
 *   /run/nostr-auth/greeter/current.json  0640  root:<group>
 *   /run/nostr-auth/greeter/current.png   0640  root:<group>
 *   /run/nostr-auth/greeter/avatar.png    0640  root:<group>
 *
 * Runs headlessly with an unprivileged uid — no chown to a foreign
 * gid is required. We pick a group the running process already
 * belongs to via getgroups(2) and pin the broker to it with the
 * nh_broker_greeter_artifact_set_group() test seam. If the process
 * has no supplementary groups (should not happen on Linux — the
 * primary group counts) the test falls back to asserting the
 * mode-only invariant.
 *
 * Confidentiality regression: the last check re-executes the write
 * with a DELIBERATELY unresolvable group name so the "group not
 * found" fallback path is exercised too. In that case the files
 * must still be mode 0640 (root:root); the pairing secret is never
 * left world-readable. */
#define _GNU_SOURCE
#include "auth_broker.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define OK(x) do { if (!(x)) { \
  fprintf(stderr, "FAIL: %s @ %s:%d (errno=%d %s)\n", \
          #x, __FILE__, __LINE__, errno, strerror(errno)); exit(1); \
} } while (0)

static int rmtree(const char *dir) {
  char cmd[600];
  snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
  return system(cmd);
}

static const char *sample_display_json(void) {
  return
    "{"
    "\"uri\":\"nostrconnect://deadbeef?relay=wss%3A%2F%2Fnos.lol"
    "&secret=cafef00dcafef00dcafef00dcafef00d&name=GNOME\","
    "\"pairing_code\":\"A1B2-C3D4\","
    "\"expires_at\":1758560000,"
    "\"hint\":\"Scan with your Nostr signer\""
    "}";
}

/* Pick a group the caller already belongs to. Preference order:
 * (1) a supplementary group whose gid != egid (so the fchown call is
 *     observable — chown'ing a file to a group we already implicitly
 *     own via egid would look like a no-op),
 * (2) any supplementary group,
 * (3) egid.
 * Returns the group name in *out_name (getgrgid) and its gid in
 * *out_gid; NULL name if no group could be resolved. */
static void pick_test_group(char *out_name, size_t cap, gid_t *out_gid) {
  out_name[0] = '\0';
  *out_gid = (gid_t)-1;
  int n = getgroups(0, NULL);
  if (n < 0) return;
  gid_t *gs = calloc(n > 0 ? n : 1, sizeof(gid_t));
  OK(gs);
  int got = getgroups(n, gs);
  OK(got >= 0);
  gid_t egid = getegid();
  gid_t pick = (gid_t)-1;
  for (int i = 0; i < got; i++) {
    if (gs[i] != egid) { pick = gs[i]; break; }
  }
  if (pick == (gid_t)-1) {
    if (got > 0) pick = gs[0];
    else pick = egid;
  }
  free(gs);
  struct group *gr = getgrgid(pick);
  if (gr && gr->gr_name) {
    snprintf(out_name, cap, "%s", gr->gr_name);
    *out_gid = pick;
  }
}

int main(void) {
  /* Live-rig acceptance override: NH_GREETER_PERMS_DIR pins the drop
   * dir (usually /run/nostr-auth/greeter) and NH_GREETER_PERMS_GROUP
   * pins the group. Both must be set together; when they are, we
   * skip the mkdtemp+chown-to-self path and the group-lookup-failure
   * regression at the bottom (the caller is responsible for cleanup
   * because /run is not ours). Absent → normal headless test. */
  const char *env_dir = getenv("NH_GREETER_PERMS_DIR");
  const char *env_grp = getenv("NH_GREETER_PERMS_GROUP");
  int live_rig = (env_dir && env_dir[0] && env_grp && env_grp[0]);

  char dir_buf[] = "/tmp/nh-greeter-perms-XXXXXX";
  const char *dir;
  if (live_rig) {
    dir = env_dir;
  } else {
    OK(mkdtemp(dir_buf));
    dir = dir_buf;
  }

  nh_broker_greeter_artifact_set_dir(dir);

  char groupname[64] = "";
  gid_t want_gid = (gid_t)-1;
  if (live_rig) {
    snprintf(groupname, sizeof groupname, "%s", env_grp);
    nh_broker_greeter_artifact_set_group(groupname);
    struct group *gr = getgrnam(groupname);
    OK(gr);
    want_gid = gr->gr_gid;
  } else {
    pick_test_group(groupname, sizeof groupname, &want_gid);
    if (groupname[0]) {
      nh_broker_greeter_artifact_set_group(groupname);
    } else {
      /* Extremely unusual — fall through and just assert mode bits. */
      nh_broker_greeter_artifact_set_group("");
    }
  }

  /* Build the avatar source so the write path exercises the
   * avatar-copy branch too (which used to also emit mode 0644). */
  char avatar_src[512];
  snprintf(avatar_src, sizeof avatar_src, "%s/src-avatar.png", dir);
  FILE *af = fopen(avatar_src, "wb");
  OK(af);
  static const unsigned char png_stub[] = {
    /* 8-byte PNG signature — good enough for the copy path; the
     * broker never decodes the source, it just streams bytes. */
    0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a,
    'p', 'a', 'y', 'l', 'o', 'a', 'd'
  };
  OK(fwrite(png_stub, 1, sizeof png_stub, af) == sizeof png_stub);
  fclose(af);

  nh_broker_greeter_account acct = {
    .username = "n_bizarro",
    .display_name = "Biz",
    .identifier = "chebizarro@coinos.io",
    .icon_source_path = avatar_src,
  };

  OK(nh_broker_greeter_artifact_write("tx-perm", sample_display_json(),
                                      &acct) == 0);

  /* Directory posture. */
  struct stat sd;
  OK(stat(dir, &sd) == 0);
  OK(S_ISDIR(sd.st_mode));
  mode_t dbits = sd.st_mode & 07777;
  if (dbits != 0750) {
    fprintf(stderr, "FAIL: dir mode = 0%o, want 0750\n", dbits);
    exit(1);
  }
  if (want_gid != (gid_t)-1 && sd.st_gid != want_gid) {
    fprintf(stderr,
            "FAIL: dir gid = %u, want %u (group '%s')\n",
            (unsigned)sd.st_gid, (unsigned)want_gid, groupname);
    exit(1);
  }

  /* Manifest + QR + avatar posture. */
  const char *names[] = {"current.json", "current.png", "avatar.png"};
  for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
    char path[600];
    snprintf(path, sizeof path, "%s/%s", dir, names[i]);
    struct stat st;
    OK(stat(path, &st) == 0);
    mode_t bits = st.st_mode & 07777;
    if (bits != 0640) {
      fprintf(stderr, "FAIL: %s mode = 0%o, want 0640\n", names[i], bits);
      exit(1);
    }
    /* World bits must be strictly zero — the whole point of the
     * fix. This is a stronger check than the mask above (which
     * would also permit 0644 by mistake if the compare was ==
     * against 0644 by an editor slip). */
    OK((st.st_mode & (S_IROTH | S_IWOTH | S_IXOTH)) == 0);
    if (want_gid != (gid_t)-1 && st.st_gid != want_gid) {
      fprintf(stderr,
              "FAIL: %s gid = %u, want %u (group '%s')\n",
              names[i], (unsigned)st.st_gid, (unsigned)want_gid, groupname);
      exit(1);
    }
  }

  /* No leaked .tmp files. */
  const char *tmps[] = {"current.json.tmp", "current.png.tmp", "avatar.png.tmp"};
  for (size_t i = 0; i < sizeof tmps / sizeof tmps[0]; i++) {
    char path[600];
    snprintf(path, sizeof path, "%s/%s", dir, tmps[i]);
    struct stat st;
    OK(stat(path, &st) != 0);
  }

  /* Group-lookup failure regression: force an unresolvable group
   * name and assert the fallback still produces 0640 (never widens
   * to 0644). This is the property that keeps the pairing secret
   * confidential on a host where the sysusers.d snippet has not
   * been applied yet. Skipped on the live rig — we don't want to
   * leave the real greeter dir in the "fallback" ownership state
   * for the next real login attempt. */
  if (!live_rig) {
    nh_broker_greeter_artifact_remove();
    nh_broker_greeter_artifact_set_group(
        "nh-definitely-does-not-exist-x9q7z-3c7n");
    OK(nh_broker_greeter_artifact_write("tx-fallback", sample_display_json(),
                                        &acct) == 0);
    {
      char path[600];
      snprintf(path, sizeof path, "%s/current.json", dir);
      struct stat st;
      OK(stat(path, &st) == 0);
      OK((st.st_mode & 07777) == 0640);
      OK((st.st_mode & (S_IROTH | S_IWOTH | S_IXOTH)) == 0);
    }
  }

  /* Live-rig keep-artifact mode: leave current.{json,png} +
   * avatar.png on disk so the caller can run the deny/allow
   * permission check as a non-member vs member user. Absent →
   * clean up at exit as usual. */
  if (!(live_rig && getenv("NH_GREETER_PERMS_KEEP") &&
        getenv("NH_GREETER_PERMS_KEEP")[0])) {
    nh_broker_greeter_artifact_remove();
    if (!live_rig) rmtree(dir);
  }
  printf("ok test_greeter_artifact_perms\n");
  return 0;
}
