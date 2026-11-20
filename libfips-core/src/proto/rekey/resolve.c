#include <fips/proto/rekey/resolve.h>
#include <string.h>
fips_rekey_verdict fips_rekey_resolve(fips_rekey_commit commit, fips_rekey_own own,
                                      const fips_node_addr *me, const fips_node_addr *peer,
                                      bool restart) {
    switch (own) {
    case FIPS_REKEY_OWN_None:
        return FIPS_REKEY_Accept;
    case FIPS_REKEY_OWN_Unanswered:
        if (restart || commit == FIPS_REKEY_COMMIT_AtFinal)
            return FIPS_REKEY_YieldOwn;
        break;
    case FIPS_REKEY_OWN_Completed:
        if (restart)
            return FIPS_REKEY_YieldOwn;
        if (commit == FIPS_REKEY_COMMIT_AtAnswer)
            return FIPS_REKEY_DiscardCompeting;
        break;
    }
    return memcmp(me, peer, sizeof *me) < 0 ? FIPS_REKEY_KeepOwn : FIPS_REKEY_YieldOwn;
}
