#define _GNU_SOURCE
#include "auth_porthome_stage.h"
#include "auth_porthome_fetch.h"
#include <fcntl.h>
#include <dirent.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { perror(#x); abort(); } } while (0)
static int remove_entry(const char *p, const struct stat *s, int t, struct FTW *f) {
  (void)s; (void)t; (void)f; return remove(p);
}
static int new_stage(char *path, int *work) {
  CHECK(mkdtemp(path));
  int stage = open(path, O_RDONLY | O_DIRECTORY);
  CHECK(stage >= 0);
  *work = nh_auth_porthome_stage_open(stage);
  CHECK(*work >= 0);
  return stage;
}
static void cleanup(char *path, int stage, int work) {
  close(work); close(stage);
  CHECK(nftw(path, remove_entry, 16, FTW_DEPTH | FTW_PHYS) == 0);
}
static int file_at(int fd, const char *name) {
  int f = openat(fd, name, O_RDWR | O_CREAT | O_EXCL, 0640);
  CHECK(f >= 0); CHECK(write(f, "home\n", 5) == 5); return f;
}
static void real_fetch(char **argv) {
  char path[] = "/home/.nh-fetch-test-XXXXXX";
  int work, stage = new_stage(path, &work);
  const char *relays[] = {argv[5]}, *servers[] = {argv[6]};
  nh_auth_porthome_fetch_args args = {0};
  args.account_pubkey_hex = argv[3];
  args.home_root_id_hex = argv[4]; /* opaque, not used to authorize fetch */
  args.home_key_hex = argv[4];
  args.d_tag = "nostr-homed.home.v1:personal";
  args.relays = relays; args.relays_count = 1;
  args.blossom_servers = servers; args.blossom_servers_count = 1;
  args.allow_insecure = 1;
  args.staging_fd = work; args.private_staging = 1;
  args.relay_timeout_ms = 3000; args.total_timeout_ms = 10000;
  nh_auth_porthome_fetch_set_helper_path(argv[2]);
  int ec = -1;
  nh_auth_porthome_fetch_result result = nh_auth_porthome_fetch_spawn(&args, NULL, &ec);
  printf("real fetch: result=%d exit=%d\n", result, ec);
  CHECK(nh_auth_porthome_stage_reclaim(work) == 0);
  CHECK(result == NH_PORTHOME_FETCH_RES_OK && ec == 0);
  CHECK(nh_auth_porthome_stage_import(stage, work, 200001, 200002, 1048576) == 0);
  struct stat st;
  CHECK(fstat(stage, &st) == 0 && st.st_uid == 0 && (st.st_mode & 07777) == 0700);
  /* The fixture contains one regular file; inspect its actual bytes and
   * ownership, not merely the helper's success result. */
  DIR *dir = fdopendir(dup(stage)); CHECK(dir);
  unsigned files = 0;
  struct dirent *e;
  while ((e = readdir(dir))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    CHECK(fstatat(stage, e->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0);
    CHECK(S_ISREG(st.st_mode) && st.st_uid == 200001 && st.st_gid == 200002);
    int f = openat(stage, e->d_name, O_RDONLY | O_NOFOLLOW); CHECK(f >= 0);
    char bytes[64]; ssize_t n = read(f, bytes, sizeof bytes); close(f);
    CHECK(n == 26 && !memcmp(bytes, "first login portable home\n", 26)); files++;
  }
  closedir(dir); CHECK(files == 1);
  cleanup(path, stage, work);
  puts("OK: real network fetch, protected stage, account-owned content");
}

int main(int argc, char **argv) {
  if (geteuid() != 0) { puts("SKIP: stage ownership test requires root"); return 77; }
  if (argc == 7 && !strcmp(argv[1], "--fetch")) { real_fetch(argv); return 0; }
  CHECK(argc == 1);
  char path[] = "/tmp/nh-stage-XXXXXX";
  int work, stage = new_stage(path, &work);
  struct stat before, after, source, dest;
  CHECK(fstat(stage, &before) == 0);
  CHECK(nh_auth_porthome_stage_open(stage) < 0); /* never adopt a collision */
  CHECK(fchown(work, 65534, 65534) == 0);
  CHECK(mkdirat(work, "Documents", 0750) == 0);
  int dir = openat(work, "Documents", O_RDONLY | O_DIRECTORY);
  CHECK(dir >= 0);
  int retained = file_at(dir, "hello");
  const struct timespec times[2] = {{123456789, 123}, {123456790, 456}};
  CHECK(futimens(retained, times) == 0);
  CHECK(futimens(dir, times) == 0);
  CHECK(fstat(retained, &source) == 0);
  close(dir);
  CHECK(symlinkat("Documents/hello", work, "shortcut") == 0);
  CHECK(utimensat(work, "shortcut", times, AT_SYMLINK_NOFOLLOW) == 0);
  CHECK(nh_auth_porthome_stage_import(stage, work, 200001, 200002, 100) == 0);
  CHECK(fstat(stage, &after) == 0);
  CHECK(after.st_ino == before.st_ino && after.st_uid == 0 &&
        (after.st_mode & 07777) == 0700);
  CHECK(fstatat(stage, ".nostr-fetch", &after, AT_SYMLINK_NOFOLLOW) != 0);
  CHECK(fstatat(stage, "Documents", &after, 0) == 0);
  CHECK(after.st_uid == 200001 && after.st_gid == 200002);
  CHECK(after.st_mtim.tv_sec == times[1].tv_sec && after.st_mtim.tv_nsec == times[1].tv_nsec);
  CHECK(fstatat(stage, "shortcut", &after, AT_SYMLINK_NOFOLLOW) == 0);
  CHECK(S_ISLNK(after.st_mode) && after.st_uid == 200001);
  CHECK(after.st_mtim.tv_sec == times[1].tv_sec && after.st_mtim.tv_nsec == times[1].tv_nsec);
  CHECK(fstatat(stage, "Documents/hello", &dest, 0) == 0);
  CHECK(dest.st_uid == 200001 && dest.st_gid == 200002 && dest.st_ino != source.st_ino);
  CHECK(dest.st_mtim.tv_sec == times[1].tv_sec && dest.st_mtim.tv_nsec == times[1].tv_nsec);
  CHECK(pwrite(retained, "evil\n", 5, 0) == 5);
  close(retained);
  int installed = openat(stage, "Documents/hello", O_RDONLY);
  char bytes[5]; CHECK(read(installed, bytes, sizeof bytes) == 5);
  CHECK(!memcmp(bytes, "home\n", 5)); close(installed);
  cleanup(path, stage, work);

  for (int mode = 0; mode < 5; mode++) {
    char bad[] = "/tmp/nh-stage-bad-XXXXXX";
    stage = new_stage(bad, &work);
    if (mode == 0) CHECK(mkfifoat(work, "fifo", 0600) == 0);
    if (mode == 1) CHECK(symlinkat("/etc/passwd", work, "absolute") == 0);
    if (mode >= 2) {
      int file = file_at(work, "file"); close(file);
      if (mode == 2) CHECK(linkat(work, "file", work, "hardlink", 0) == 0);
      if (mode == 3) { file = file_at(stage, "file"); close(file); }
    }
    CHECK(nh_auth_porthome_stage_import(stage, work, 200001, 200002,
                                      mode == 4 ? 1 : 100) != 0);
    CHECK(fstat(stage, &after) == 0 && after.st_uid == 0);
    cleanup(bad, stage, work);
  }
  puts("OK: private stage import, account ownership, independent inodes, refusal paths");
  return 0;
}
