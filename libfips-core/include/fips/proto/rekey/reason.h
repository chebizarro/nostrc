#ifndef FIPS_REKEY_REASON_H
#define FIPS_REKEY_REASON_H
#include <fips/host.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum fips_rekey_attribution {
    FIPS_REKEY_ATTRIBUTION_Peer,
    FIPS_REKEY_ATTRIBUTION_Network,
    FIPS_REKEY_ATTRIBUTION_Local
} fips_rekey_attribution;
typedef enum fips_rekey_reason {
    FIPS_REKEY_REASON_EpochChanged,
    FIPS_REKEY_REASON_AnsweredBefore,
    FIPS_REKEY_REASON_OffLink,
    FIPS_REKEY_REASON_PendingHeld,
    FIPS_REKEY_REASON_DualInitWon,
    FIPS_REKEY_REASON_IndexExhausted,
    FIPS_REKEY_REASON_HandshakeWrite,
    FIPS_REKEY_REASON_ReplyUnauthenticated,
    FIPS_REKEY_REASON_LateReply,
    FIPS_REKEY_REASON_UnknownAttempt,
    FIPS_REKEY_REASON_UnknownIndex,
    FIPS_REKEY_REASON_SetupMalformed,
    FIPS_REKEY_REASON_TieBreakKept,
    FIPS_REKEY_REASON_CandidatesFull,
    FIPS_REKEY_REASON_FinalNoAttempt,
    FIPS_REKEY_REASON_FinalUnauthenticated,
    FIPS_REKEY_REASON_KeyMismatch,
    FIPS_REKEY_REASON_StaleIncarnation,
    FIPS_REKEY_REASON_Replay,
    FIPS_REKEY_REASON_FrameUnauthenticated,
    FIPS_REKEY_REASON_ShortPlaintext,
    FIPS_REKEY_REASON_DelegatedEpoch,
    FIPS_REKEY_REASON_NotEstablished,
    FIPS_REKEY_REASON_StaleTag,
    FIPS_REKEY_REASON_NotDelegated,
    FIPS_REKEY_REASON_AlreadyDelegated,
    FIPS_REKEY_REASON_NotOffered,
    FIPS_REKEY_REASON_AttemptInFlight,
    FIPS_REKEY_REASON_AlreadyEstablished,
    FIPS_REKEY_REASON_CandidateExpired,
    FIPS_REKEY_REASON_NotRekey
} fips_rekey_reason;
typedef enum fips_rekey_abandon {
    FIPS_REKEY_ABANDON_AdoptionNotSeen,
    FIPS_REKEY_ABANDON_Unadopted,
    FIPS_REKEY_ABANDON_PeerStayedOnOld,
    FIPS_REKEY_ABANDON_RevertUnconfirmed,
    FIPS_REKEY_ABANDON_Unanswered,
    FIPS_REKEY_ABANDON_Yielded,
    FIPS_REKEY_ABANDON_Reverted,
    FIPS_REKEY_ABANDON_Superseded,
    FIPS_REKEY_ABANDON_Closed
} fips_rekey_abandon;
typedef enum fips_rekey_teardown {
    FIPS_REKEY_TEARDOWN_OpeningIdle,
    FIPS_REKEY_TEARDOWN_OpeningFailed,
    FIPS_REKEY_TEARDOWN_CounterExhausted
} fips_rekey_teardown;
typedef enum fips_rekey_close {
    FIPS_REKEY_CLOSE_Restart,
    FIPS_REKEY_CLOSE_Disconnect,
    FIPS_REKEY_CLOSE_LinkDead,
    FIPS_REKEY_CLOSE_IdlePurge,
    FIPS_REKEY_CLOSE_Admin,
    FIPS_REKEY_CLOSE_CrossConnectionMove,
    FIPS_REKEY_CLOSE_OpeningTeardown,
    FIPS_REKEY_CLOSE_Shutdown
} fips_rekey_close;
FIPS_CORE_API fips_rekey_attribution fips_rekey_reason_attribution(fips_rekey_reason reason);
FIPS_CORE_API bool fips_rekey_reason_unauthenticated_reachable(fips_rekey_reason reason);
#ifdef __cplusplus
}
#endif

#endif /* FIPS_REKEY_REASON_H */
