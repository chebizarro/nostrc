#ifndef NH_AUTH_FSCRYPT_H
#define NH_AUTH_FSCRYPT_H
#include "nostr_identity.h"
#include <sys/types.h>
struct nh_fscrypt;
/* Broker/worker hooks are installed only by nostr-authd. Keeping the adapter
 * out of nostr_auth_runtime's link closure leaves PAM completely unchanged. */
typedef struct {
  struct nh_fscrypt *context;
  int (*required)(struct nh_fscrypt *, const char *, int);
  int (*admit)(struct nh_fscrypt *, const nh_identity_account *, const uint8_t *, pid_t, const char *, int);
  int (*stage)(struct nh_fscrypt *, const char *, int, int);
  int (*finished)(struct nh_fscrypt *, const char *, int);
} nh_auth_fscrypt_hooks;
#endif
