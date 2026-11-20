#ifndef FIPSD_NOSTR_RELAY_FIXTURE_H
#define FIPSD_NOSTR_RELAY_FIXTURE_H
#include <stdbool.h>
#include <stddef.h>
typedef struct fixture fixture;
typedef enum { INJECT, SEED, CLEAR, CHALLENGE, CLOSE_SUBS, REJECT_PUBLISH, AUTH_CLOSED, DISCONNECT, HOLD_EOSE, RELEASE_EOSE, STOP, QUERY,
    REJECT_KIND, NIP11_MODE, COUNTS } operation;
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
/* NIP-11 at relay `index`: fail the next `failures` fetches with 503; when
 * `unsupported`, serve a document without NIP-40. Set before a client starts. */
void fixture_nip11(fixture *, size_t index, int failures, bool unsupported);
/* At relay `index`: EVENTs refused by fixture_reject, NIP-11 requests seen.
 * Ordered after every earlier frame the relay handled. */
void fixture_counts(fixture *, size_t index, size_t *rejected, size_t *nip11);
#endif
