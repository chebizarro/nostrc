#include <fips/proto/rekey/profile.h>
#include <sodium.h>
bool fips_rekey_profile_valid(const fips_rekey_profile *p) {
    if (!p || !p->keys || !p->initiate || !p->reply || !p->prompt || !p->is_probe ||
        !p->keys->open || !p->keys->fresh || !p->keys->mark || !p->keys->sent ||
        !p->keys->reserve || !p->keys->copy || p->candidates > FIPS_REKEY_CANDIDATES_MAX ||
        p->per_carrier > p->candidates)
        return false;
    if (p->indexed && !p->index)
        return false;
    if (p->messages == 2)
        return p->msg1_answer && !p->candidates;
    return p->messages == 3 && p->answer && p->valid && p->complete;
}
void fips_rekey_hs_clear(fips_rekey_hs *hs) {
    fips_handshake_destroy(&hs->noise);
    sodium_memzero(hs, sizeof *hs);
}
void fips_rekey_completed_clear(fips_rekey_completed *d) {
    fips_noise_session_clear_slot(&d->session);
    sodium_memzero(d, sizeof *d);
}
void fips_rekey_recv_copy_clear(fips_rekey_recv_copy *c) {
    fips_cipher_state_destroy(&c->key);
    sodium_memzero(c, sizeof *c);
}
