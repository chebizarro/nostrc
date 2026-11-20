#ifndef FIPS_REKEY_RESOLVE_H
#define FIPS_REKEY_RESOLVE_H
#include <fips/identity/node_addr.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum fips_rekey_own {
    FIPS_REKEY_OWN_None,
    FIPS_REKEY_OWN_Unanswered,
    FIPS_REKEY_OWN_Completed
} fips_rekey_own;
typedef enum fips_rekey_commit {
    FIPS_REKEY_COMMIT_AtAnswer,
    FIPS_REKEY_COMMIT_AtFinal
} fips_rekey_commit;
typedef enum fips_rekey_verdict {
    FIPS_REKEY_Accept,
    FIPS_REKEY_YieldOwn,
    FIPS_REKEY_KeepOwn,
    FIPS_REKEY_DiscardCompeting
} fips_rekey_verdict;
FIPS_CORE_API fips_rekey_verdict fips_rekey_resolve(fips_rekey_commit commit, fips_rekey_own own,
                                                    const fips_node_addr *me,
                                                    const fips_node_addr *peer,
                                                    bool peer_restarted);
#ifdef __cplusplus
}
#endif

#endif /* FIPS_REKEY_RESOLVE_H */
