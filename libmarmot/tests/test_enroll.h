/*
 * libmarmot tests: enroll a Marmot instance's account proof (nostrc-7vyi)
 * with the account's secret key, as a signer would: sign the kind:450
 * template of marmot_account_proof_template() and hand it back.  Since
 * 0.10.0 marmot_create_group() needs it (outside legacy mode).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MARMOT_TEST_ENROLL_H
#define MARMOT_TEST_ENROLL_H

#include <marmot/marmot.h>
#include <nostr-event.h>
#include <stdio.h>
#include <stdlib.h>

static inline MarmotError
test_enroll(Marmot *m, const uint8_t pk[32], const uint8_t sk[32])
{
    char *tmpl = NULL;
    MarmotError err = marmot_account_proof_template(m, pk, &tmpl);
    if (err != MARMOT_OK) return err;
    char sk_hex[65];
    for (int i = 0; i < 32; i++) snprintf(sk_hex + 2 * i, 3, "%02x", sk[i]);
    NostrEvent *ev = nostr_event_new();
    char *signed_json = NULL;
    err = MARMOT_ERR_CRYPTO;
    if (ev && nostr_event_deserialize_compact(ev, tmpl, NULL) && nostr_event_sign(ev, sk_hex) == 0)
        signed_json = nostr_event_serialize_compact(ev);
    if (signed_json) err = marmot_set_account_proof(m, pk, signed_json);
    free(signed_json);
    if (ev) nostr_event_free(ev);
    free(tmpl);
    for (int i = 0; i < 65; i++) sk_hex[i] = 0;
    return err;
}

#endif /* MARMOT_TEST_ENROLL_H */
