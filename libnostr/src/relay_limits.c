#include "nostr-relay-limits.h"

const char *nostr_limits_reason_rl_exceeded(void) {
  return "rate-limited: too many messages";
}

const char *nostr_limits_reason_invalid_filter(void) {
  return "invalid: bad filter";
}
