#define _GNU_SOURCE
#include "fscrypt_kernel.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv) {
  assert(argc == 2 || argc == 3);
  int root = open(argv[1], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  assert(root >= 0);
  if (argc == 3) {
    assert(!strcmp(argv[2], "--unsupported"));
    assert(nh_fscrypt_probe(root) == -1);
    close(root);
    puts("unsupported filesystem refused: PASS");
    return 0;
  }
  assert(nh_fscrypt_probe(root) == 0);
  uint8_t key[64], id[16], again[16], wrong[16] = {0};
  for (unsigned i = 0; i < 64; i++) key[i] = (uint8_t)i;
  assert(nh_fscrypt_identifier(key, again) == 0);
  assert(nh_fscrypt_add(root, key, id) == 0 && !memcmp(id, again, 16));
  int ready[2], release[2]; assert(!pipe(ready) && !pipe(release));
  pid_t child = fork(); assert(child >= 0);
  if (!child) {
    close(ready[0]); close(release[1]);
    assert(!setgid(1000) && !setuid(1000));
    assert(!nh_fscrypt_add(root, key, again));
    assert(write(ready[1], "R", 1) == 1);
    char token; assert(read(release[0], &token, 1) == 1);
    assert(nh_fscrypt_remove(root, id) == NH_FSCRYPT_ABSENT);
    _exit(0);
  }
  close(ready[1]); close(release[0]);
  char token; assert(read(ready[0], &token, 1) == 1);
  assert(nh_fscrypt_remove(root, id) == NH_FSCRYPT_BUSY);
  assert(nh_fscrypt_key_status(root, id) == NH_FSCRYPT_PRESENT);
  assert(write(release[1], "R", 1) == 1);
  int status; assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
  close(ready[0]); close(release[1]);
  assert(!nh_fscrypt_add(root, key, id));
  assert(mkdirat(root, "encrypted", 0700) == 0);
  int home = openat(root, "encrypted", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  assert(home >= 0 && nh_fscrypt_set_policy(home, id) == 0);
  assert(nh_fscrypt_verify_policy(home, id) == 0);
  assert(nh_fscrypt_verify_policy(home, wrong) == -1);
  int file = openat(home, "private", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  assert(file >= 0 && write(file, "secret", 6) == 6 && fsync(file) == 0);
  close(home);
  assert(nh_fscrypt_remove(root, id) == NH_FSCRYPT_BUSY);
  assert(nh_fscrypt_key_status(root, id) == NH_FSCRYPT_BUSY);
  /* Relogin while an old file remains open restores the same policy key. */
  assert(nh_fscrypt_add(root, key, again) == 0 && !memcmp(id, again, 16));
  assert(nh_fscrypt_key_status(root, id) == NH_FSCRYPT_PRESENT);
  close(file);
  assert(nh_fscrypt_remove(root, id) == NH_FSCRYPT_ABSENT);
  home = openat(root, "encrypted", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  assert(home >= 0 && nh_fscrypt_verify_policy(home, id) == 0);
  assert(openat(home, "new", O_WRONLY | O_CREAT | O_EXCL, 0600) == -1 && errno == ENOKEY);
  close(home);
  assert(nh_fscrypt_add(root, key, again) == 0);
  home = openat(root, "encrypted", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  assert(home >= 0);
  file = openat(home, "private", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  char buf[6];
  assert(file >= 0 && read(file, buf, 6) == 6 && !memcmp(buf, "secret", 6));
  close(file);
  assert(unlinkat(home, "private", 0) == 0);
  close(home);
  assert(unlinkat(root, "encrypted", AT_REMOVEDIR) == 0);
  assert(nh_fscrypt_remove(root, id) == NH_FSCRYPT_ABSENT);
  close(root);
  puts("fscrypt real file roundtrip, policy, partial removal and relogin: PASS");
  return 0;
}
