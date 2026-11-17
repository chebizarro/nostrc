#ifndef FIPSD_NOSTR_RELAY_FIXTURE_H
#define FIPSD_NOSTR_RELAY_FIXTURE_H
#include <stddef.h>
typedef struct fixture fixture;
typedef enum { INJECT, SEED, CLEAR, CHALLENGE, CLOSE_SUBS, REJECT_PUBLISH, AUTH_CLOSED, DISCONNECT, HOLD_EOSE, RELEASE_EOSE, STOP, QUERY } operation;
fixture *fixture_start(void);
const char *fixture_url(fixture *, size_t index);
void fixture_call(fixture *, operation, int relay, const char *json);
void fixture_destroy(fixture *);
/* Every accepted EVENT of `kind` by `author` (x-only hex) at relay `index`,
 * replaceable ones included (stored on each publish): how many, and the last
 * as JSON (owned; NULL when none). Ordered after every earlier OK it sent. */
char *fixture_latest(fixture *, size_t index, int kind, const char *author, size_t *count);
#endif
