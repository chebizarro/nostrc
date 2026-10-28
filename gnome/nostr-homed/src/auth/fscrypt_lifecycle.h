#ifndef NH_FSCRYPT_LIFECYCLE_H
#define NH_FSCRYPT_LIFECYCLE_H
#include "nostr_identity.h"
#include <sys/types.h>

typedef struct nh_fscrypt nh_fscrypt;
/* Root-only database below authority_dir, outside the encrypted home. Raw keys
 * are never persisted. A compiled-in feature continues to unlock existing
 * encrypted accounts even when enable_new is disabled. */
int nh_fscrypt_open(const char *authority_dir, int enable_new, int allow_without_luks,
                    nh_fscrypt **out);
void nh_fscrypt_close(nh_fscrypt *ctx);
int nh_fscrypt_maintenance_main(int argc, char **argv);
int nh_fscrypt_required(nh_fscrypt *ctx, const char *account_id, int new_home);
/* Authenticate/unwrap before calling. Missing seed always refuses an encrypted
 * account, even if its key is already present. peer is SO_PEERCRED's PAM PID;
 * a durable admission bridges the pre-pam_systemd gap, not socket lifetime. */
int nh_fscrypt_admit(nh_fscrypt *ctx, const nh_identity_account *account,
    const uint8_t seed[32], pid_t peer, const char *operation_id, int new_home);
int nh_fscrypt_stage(nh_fscrypt *ctx, const char *account_id, int fd, int initialize);
int nh_fscrypt_finished(nh_fscrypt *ctx, const char *account_id, int success);

/* Used by the watcher and unit tests. Unknown observations block removal. A
 * retry retains its epoch; a new admission invalidates it before key-add. */
typedef struct {
  uint64_t epoch;
  unsigned admissions;
  int sessions;              /* -1: observation unavailable */
  int manager_inactive;      /* 1 only after a positive observation */
  int provisioning;
  int maintenance;
} nh_fscrypt_lifecycle;
static inline int nh_fscrypt_lock_eligible(const nh_fscrypt_lifecycle *s, uint64_t epoch) {
  return s->epoch == epoch && !s->admissions && s->sessions == 0 &&
      s->manager_inactive == 1 && !s->provisioning && !s->maintenance;
}
#endif
