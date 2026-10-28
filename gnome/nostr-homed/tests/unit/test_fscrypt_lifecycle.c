#include "fscrypt_lifecycle.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
  nh_fscrypt_lifecycle s = {.epoch = 1, .sessions = 0, .manager_inactive = 1};
  assert(nh_fscrypt_lock_eligible(&s, 1));
  s.admissions = 1; /* PAM has returned but pam_systemd has not run yet. */
  assert(!nh_fscrypt_lock_eligible(&s, 1));
  s.sessions = 1; s.manager_inactive = 0; s.admissions = 0;
  assert(!nh_fscrypt_lock_eligible(&s, 1));
  s.sessions = 0; /* session removed, user manager still stopping */
  assert(!nh_fscrypt_lock_eligible(&s, 1));
  s.manager_inactive = 1;
  assert(nh_fscrypt_lock_eligible(&s, 1));
  uint64_t busy_retry = s.epoch;
  ++s.epoch; s.admissions = 1; /* new login cancels a partial-lock retry */
  assert(!nh_fscrypt_lock_eligible(&s, busy_retry));
  s.admissions = 0;
  assert(!nh_fscrypt_lock_eligible(&s, busy_retry));
  assert(nh_fscrypt_lock_eligible(&s, s.epoch));
  s.sessions = -1; assert(!nh_fscrypt_lock_eligible(&s, s.epoch));
  s.sessions = 0; s.manager_inactive = -1;
  assert(!nh_fscrypt_lock_eligible(&s, s.epoch));
  s.manager_inactive = 1; s.provisioning = 1;
  assert(!nh_fscrypt_lock_eligible(&s, s.epoch));
  s.provisioning = 0; s.maintenance = 1;
  assert(!nh_fscrypt_lock_eligible(&s, s.epoch));
  puts("fscrypt lifecycle blockers and epoch fence: PASS");
  return 0;
}
