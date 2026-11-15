#include <fips/proto/rekey/reason.h>
fips_rekey_attribution fips_rekey_reason_attribution(fips_rekey_reason r) {
    switch (r) {
    case FIPS_REKEY_REASON_IndexExhausted:
    case FIPS_REKEY_REASON_HandshakeWrite:
    case FIPS_REKEY_REASON_StaleTag:
    case FIPS_REKEY_REASON_NotDelegated:
    case FIPS_REKEY_REASON_AlreadyDelegated:
    case FIPS_REKEY_REASON_NotOffered:
    case FIPS_REKEY_REASON_AttemptInFlight:
    case FIPS_REKEY_REASON_AlreadyEstablished:
    case FIPS_REKEY_REASON_NotRekey:
    case FIPS_REKEY_REASON_DelegatedEpoch:
        return FIPS_REKEY_ATTRIBUTION_Local;
    case FIPS_REKEY_REASON_EpochChanged:
    case FIPS_REKEY_REASON_PendingHeld:
    case FIPS_REKEY_REASON_DualInitWon:
    case FIPS_REKEY_REASON_TieBreakKept:
    case FIPS_REKEY_REASON_StaleIncarnation:
    case FIPS_REKEY_REASON_ShortPlaintext:
        return FIPS_REKEY_ATTRIBUTION_Peer;
    case FIPS_REKEY_REASON_OffLink:
    case FIPS_REKEY_REASON_KeyMismatch:
    case FIPS_REKEY_REASON_AnsweredBefore:
    case FIPS_REKEY_REASON_ReplyUnauthenticated:
    case FIPS_REKEY_REASON_LateReply:
    case FIPS_REKEY_REASON_UnknownAttempt:
    case FIPS_REKEY_REASON_UnknownIndex:
    case FIPS_REKEY_REASON_SetupMalformed:
    case FIPS_REKEY_REASON_CandidatesFull:
    case FIPS_REKEY_REASON_FinalNoAttempt:
    case FIPS_REKEY_REASON_FinalUnauthenticated:
    case FIPS_REKEY_REASON_Replay:
    case FIPS_REKEY_REASON_FrameUnauthenticated:
    case FIPS_REKEY_REASON_NotEstablished:
    case FIPS_REKEY_REASON_CandidateExpired:
        return FIPS_REKEY_ATTRIBUTION_Network;
    }
    return FIPS_REKEY_ATTRIBUTION_Local;
}
bool fips_rekey_reason_unauthenticated_reachable(fips_rekey_reason r) {
    return fips_rekey_reason_attribution(r) == FIPS_REKEY_ATTRIBUTION_Network ||
           r == FIPS_REKEY_REASON_TieBreakKept;
}
