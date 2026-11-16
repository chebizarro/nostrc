#ifndef FIPSD_NOSTR_RELAY_FIXTURE_H
#define FIPSD_NOSTR_RELAY_FIXTURE_H
#include <stddef.h>
typedef struct fixture fixture;
typedef enum { INJECT, SEED, CLEAR, CHALLENGE, CLOSE_SUBS, REJECT_PUBLISH, AUTH_CLOSED, DISCONNECT, HOLD_EOSE, RELEASE_EOSE, STOP } operation;
fixture *fixture_start(void);
const char *fixture_url(fixture *, size_t index);
void fixture_call(fixture *, operation, int relay, const char *json);
void fixture_destroy(fixture *);
#endif
