#include <fips/proto/rekey/proof.h>
#include <string.h>
fips_node_addr fips_rekey_bond_addr(const fips_rekey_bond *b) {
    if (b->kind == FIPS_REKEY_BOND_Claimed)
        return b->claimed;
    fips_xonly_pubkey p;
    memcpy(p.bytes, b->key.bytes, 32);
    return fips_node_addr_from_pubkey(&p);
}
bool fips_rekey_bond_check(const fips_rekey_bond *b, const fips_rekey_completed *d) {
    switch (b->kind) {
    case FIPS_REKEY_BOND_Key:
        return memcmp(b->key.bytes, d->remote.bytes, 32) == 0;
    case FIPS_REKEY_BOND_Claimed: {
        fips_xonly_pubkey p;
        memcpy(p.bytes, d->remote.bytes, 32);
        fips_node_addr addr = fips_node_addr_from_pubkey(&p);
        return memcmp(&addr, &b->claimed, sizeof addr) == 0;
    }
    }
    return false;
}
