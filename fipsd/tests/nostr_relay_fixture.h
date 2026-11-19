#ifndef FIPSD_NOSTR_RELAY_FIXTURE_H
#define FIPSD_NOSTR_RELAY_FIXTURE_H
#include <stddef.h>
typedef struct fixture fixture;
typedef enum { INJECT, SEED, CLEAR, CHALLENGE, CLOSE_SUBS, REJECT_PUBLISH, AUTH_CLOSED, DISCONNECT, HOLD_EOSE, RELEASE_EOSE, STOP, QUERY,
    REJECT_KIND, COUNTS } operation;
fixture *fixture_start(void);
const char *fixture_url(fixture *, size_t index);
void fixture_call(fixture *, operation, int relay, const char *json);
void fixture_destroy(fixture *);
/* Every accepted EVENT of `kind` by `author` (x-only hex) at relay `index`,
 * replaceable ones included (stored on each publish): how many, and the last
 * as JSON (owned; NULL when none). Ordered after every earlier OK it sent. */
char *fixture_latest(fixture *, size_t index, int kind, const char *author, size_t *count);
/* Answer the next `count` EVENTs of `kind` at relay `index` with OK false and
 * `reason` (a NIP-01 prefixed string); count < 0: every one. */
void fixture_reject(fixture *, size_t index, int kind, int count, const char *reason);
/* EVENTs refused by fixture_reject at relay `index`. Ordered after every
 * earlier frame the relay handled. */
void fixture_counts(fixture *, size_t index, size_t *rejected);
#endif
