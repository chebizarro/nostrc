#define _GNU_SOURCE
#include "fscrypt_kernel.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/fscrypt.h>
#include <linux/magic.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <openssl/kdf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

static struct fscrypt_key_specifier spec(const uint8_t id[16]) {
  struct fscrypt_key_specifier s = {.type = FSCRYPT_KEY_SPEC_TYPE_IDENTIFIER};
  memcpy(s.u.identifier, id, 16);
  return s;
}
/* Linux fscrypt v2 key identifier: HKDF-SHA512, empty salt, context 1.
 * Compute it before ADD so a durable intent can identify even a crash between
 * the ioctl and its return. Every ADD is checked against the kernel result. */
int nh_fscrypt_identifier(const uint8_t master[64], uint8_t id[16]) {
  static const uint8_t info[] = {'f','s','c','r','y','p','t',0,1};
  EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, NULL);
  size_t n = 16;
  int ok = ctx && EVP_PKEY_derive_init(ctx) > 0 &&
      EVP_PKEY_CTX_set_hkdf_md(ctx, EVP_sha512()) > 0 &&
      EVP_PKEY_CTX_set1_hkdf_key(ctx, master, 64) > 0 &&
      EVP_PKEY_CTX_add1_hkdf_info(ctx, info, sizeof info) > 0 &&
      EVP_PKEY_derive(ctx, id, &n) > 0 && n == 16;
  EVP_PKEY_CTX_free(ctx);
  if (!ok) OPENSSL_cleanse(id, 16);
  return ok ? 0 : -1;
}
int nh_fscrypt_add(int fd, const uint8_t master[64], uint8_t id[16]) {
  size_t size = sizeof(struct fscrypt_add_key_arg) + 64;
  struct fscrypt_add_key_arg *a = mmap(NULL, size, PROT_READ | PROT_WRITE,
      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (a == MAP_FAILED) return -1;
  int result = -1;
  if (mlock(a, size)) goto out;
  if (madvise(a, size, MADV_DONTDUMP)) goto out;
  a->key_spec.type = FSCRYPT_KEY_SPEC_TYPE_IDENTIFIER;
  a->raw_size = 64;
  memcpy(a->raw, master, 64);
  result = ioctl(fd, FS_IOC_ADD_ENCRYPTION_KEY, a);
  if (!result) memcpy(id, a->key_spec.u.identifier, 16);
out:;
  int saved = errno;
  OPENSSL_cleanse(a, size);
  munlock(a, size);
  munmap(a, size);
  errno = saved;
  return result;
}
static struct fscrypt_policy_v2 policy(const uint8_t id[16]) {
  struct fscrypt_policy_v2 p = {
    .version = FSCRYPT_POLICY_V2,
    .contents_encryption_mode = FSCRYPT_MODE_AES_256_XTS,
    .filenames_encryption_mode = FSCRYPT_MODE_AES_256_CTS,
    .flags = FSCRYPT_POLICY_FLAGS_PAD_32
  };
  memcpy(p.master_key_identifier, id, 16);
  return p;
}
int nh_fscrypt_has_policy(int fd) {
  struct fscrypt_get_policy_ex_arg a = {.policy_size = sizeof a.policy};
  if (!ioctl(fd, FS_IOC_GET_ENCRYPTION_POLICY_EX, &a)) return 1;
  return errno == ENODATA || errno == ENOTTY || errno == EOPNOTSUPP ? 0 : -1;
}
int nh_fscrypt_verify_policy(int fd, const uint8_t id[16]) {
  struct fscrypt_get_policy_ex_arg a = {.policy_size = sizeof a.policy};
  struct fscrypt_policy_v2 expected = policy(id);
  if (ioctl(fd, FS_IOC_GET_ENCRYPTION_POLICY_EX, &a)) return -1;
  if (a.policy_size != sizeof expected || memcmp(&a.policy.v2, &expected, sizeof expected)) {
    errno = EKEYREJECTED;
    return -1;
  }
  return 0;
}
int nh_fscrypt_set_policy(int fd, const uint8_t id[16]) {
  struct fscrypt_policy_v2 p = policy(id);
  if (ioctl(fd, FS_IOC_SET_ENCRYPTION_POLICY, &p)) return -1;
  return nh_fscrypt_verify_policy(fd, id);
}
nh_fscrypt_key_state nh_fscrypt_key_status(int fd, const uint8_t id[16]) {
  struct fscrypt_get_key_status_arg a = {.key_spec = spec(id)};
  if (ioctl(fd, FS_IOC_GET_ENCRYPTION_KEY_STATUS, &a)) return NH_FSCRYPT_ERROR;
  switch (a.status) {
    case FSCRYPT_KEY_STATUS_ABSENT: return NH_FSCRYPT_ABSENT;
    case FSCRYPT_KEY_STATUS_PRESENT: return NH_FSCRYPT_PRESENT;
    case FSCRYPT_KEY_STATUS_INCOMPLETELY_REMOVED: return NH_FSCRYPT_BUSY;
    default: errno = EPROTO; return NH_FSCRYPT_ERROR;
  }
}
nh_fscrypt_key_state nh_fscrypt_remove(int fd, const uint8_t id[16]) {
  struct fscrypt_remove_key_arg a = {.key_spec = spec(id)};
  if (ioctl(fd, FS_IOC_REMOVE_ENCRYPTION_KEY, &a) && errno != ENOKEY)
    return NH_FSCRYPT_ERROR;
  nh_fscrypt_key_state state = nh_fscrypt_key_status(fd, id);
  /* The ioctl may have invalidated only some cached inodes. Never label that
   * success, even if a subsequent status read races with their final close. */
  if (a.removal_status_flags) return NH_FSCRYPT_BUSY;
  return state;
}
int nh_fscrypt_probe(int parent) {
  struct statfs fs;
  if (fstatfs(parent, &fs)) return -1;
  if (fs.f_type != EXT4_SUPER_MAGIC && fs.f_type != F2FS_SUPER_MAGIC) {
    errno = EOPNOTSUPP;
    return -1;
  }
  uint8_t *master = mmap(NULL, 64, PROT_READ | PROT_WRITE,
      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (master == MAP_FAILED) return -1;
  int rc = -1, d = -1, f = -1, added = 0, created = 0;
  uint8_t id[16], nonce[16];
  char name[64] = {0};
  if (mlock(master, 64) || madvise(master, 64, MADV_DONTDUMP) ||
      RAND_bytes(master, 64) != 1 || RAND_bytes(nonce, 16) != 1) goto out;
  if (nh_fscrypt_add(parent, master, id)) goto out;
  added = 1;
  memcpy(name, ".fscrypt-probe-", 14);
  for (unsigned i = 0; i < 16; i++) snprintf(name + 14 + 2*i, 3, "%02x", nonce[i]);
  if (mkdirat(parent, name, 0700)) goto out;
  created = 1;
  d = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (d < 0 || nh_fscrypt_set_policy(d, id)) goto out;
  f = openat(d, "probe", O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  static const char data[] = "fscrypt-v2-AES256-XTS-CTS-PAD32";
  char readback[sizeof data];
  if (f < 0 || write(f, data, sizeof data) != sizeof data || fsync(f) ||
      pread(f, readback, sizeof readback, 0) != sizeof data ||
      memcmp(data, readback, sizeof data)) goto out;
  rc = 0;
out:;
  int saved = errno ? errno : EIO;
  if (f >= 0) close(f);
  if (d >= 0) {
    if (unlinkat(d, "probe", 0) && errno != ENOENT) rc = -1;
    close(d);
  }
  if (created && unlinkat(parent, name, AT_REMOVEDIR)) rc = -1;
  if (added && nh_fscrypt_remove(parent, id) != NH_FSCRYPT_ABSENT) rc = -1;
  OPENSSL_cleanse(master, 64);
  munlock(master, 64);
  munmap(master, 64);
  errno = saved;
  return rc;
}
