#ifndef GH_TEST_EXPIRY_SEND_H
#define GH_TEST_EXPIRY_SEND_H

/* The EX-1 cases of test_expiry.c (outgoing expiration through real seals
 * and the durable outbox), in their own translation unit: gh-outbox.h's
 * GhMessageStatus cannot meet the conversation model's (see gh-app-outbox.h),
 * and the mock signer header is included once per executable. */
#include <glib.h>

void test_expiry_ex1_envelope(void);
void test_expiry_ex1_outbox(void);
/* Stops the private bus the cases above started, if any. */
void test_expiry_send_cleanup(void);

#endif
