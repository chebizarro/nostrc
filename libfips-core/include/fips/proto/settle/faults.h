#ifndef FIPS_PROTO_SETTLE_FAULTS_H
#define FIPS_PROTO_SETTLE_FAULTS_H
#include <fips/proto/settle/settle.h>
/* Catalog: core/src/proto/settle/faults.rs at
 * 64f5b4d482cfe14abad108ffc2d92128f7658311 (all147 names).
 * Mutations are test-only. Production macro expands to literal false: no TLS,
 * configuration, environment reads or mutable state reaches fips_core.
 * Each test thread borrows its caller-owned scope; restore the previous pointer
 * before freeing a scope. Fault activation must not overlap pending borrowed
 * resend outputs. No fault changes any public production structure layout. */
#ifdef FIPS_TEST_HOOKS
#ifdef __cplusplus
extern "C" {
#endif
typedef enum fips_settle_fault {
    FIPS_SETTLE_FAULT_IndexStale,
    FIPS_SETTLE_FAULT_CloseByPeerAddress,
    FIPS_SETTLE_FAULT_CloseBySetupAddress,
    FIPS_SETTLE_FAULT_ProbingCounts,
    FIPS_SETTLE_FAULT_KeepIgnored,
    FIPS_SETTLE_FAULT_ReleasedSetupIndexed,
    FIPS_SETTLE_FAULT_CapableIgnored,
    FIPS_SETTLE_FAULT_UnheardInclusive,
    FIPS_SETTLE_FAULT_HeardFromInstall,
    FIPS_SETTLE_FAULT_HeardNeedsFrame,
    FIPS_SETTLE_FAULT_DampenerFromRefusal,
    FIPS_SETTLE_FAULT_OutboundGated,
    FIPS_SETTLE_FAULT_DampenerAtMsg2,
    FIPS_SETTLE_FAULT_ReplayUnbounded,
    FIPS_SETTLE_FAULT_ReplaySeenIgnored,
    FIPS_SETTLE_FAULT_HeardKeepsMemory,
    FIPS_SETTLE_FAULT_FrameForgetsReplays,
    FIPS_SETTLE_FAULT_SilentReadsHeard,
    FIPS_SETTLE_FAULT_StandbyRoamResets,
    FIPS_SETTLE_FAULT_MemoryNeverDropped,
    FIPS_SETTLE_FAULT_SilentEarly,
    FIPS_SETTLE_FAULT_SilentFlat,
    FIPS_SETTLE_FAULT_IntervalTen,
    FIPS_SETTLE_FAULT_LegNeverReaped,
    FIPS_SETTLE_FAULT_ResendFresh,
    FIPS_SETTLE_FAULT_AnswerFresh,
    FIPS_SETTLE_FAULT_PollAhead,
    FIPS_SETTLE_FAULT_DeadlineWraps,
    FIPS_SETTLE_FAULT_LeaseKept,
    FIPS_SETTLE_FAULT_UnsentStaysDue,
    FIPS_SETTLE_FAULT_UnsentDueAtWake,
    FIPS_SETTLE_FAULT_UnsentPastCap,
    FIPS_SETTLE_FAULT_MoveKeepsOldPair,
    FIPS_SETTLE_FAULT_MoveKeepsInboundLink,
    FIPS_SETTLE_FAULT_MoveClosesNewPair,
    FIPS_SETTLE_FAULT_MoveLeavesDialOpen,
    FIPS_SETTLE_FAULT_MoveNoReset,
    FIPS_SETTLE_FAULT_MoveLeavesUnheard,
    FIPS_SETTLE_FAULT_MoveKeepsReplayMemory,
    FIPS_SETTLE_FAULT_IgnoreOwnDial,
    FIPS_SETTLE_FAULT_CrossedRunsKeep,
    FIPS_SETTLE_FAULT_TouchKeepsHeardInstant,
    FIPS_SETTLE_FAULT_MovedLeavesEntry,
    FIPS_SETTLE_FAULT_EpochAtCutover,
    FIPS_SETTLE_FAULT_ModelFallbackInCore,
    FIPS_SETTLE_FAULT_ReplacementUngated,
    FIPS_SETTLE_FAULT_RedialInstallsAtSent,
    FIPS_SETTLE_FAULT_LegHeardIgnored,
    FIPS_SETTLE_FAULT_RedialUngated,
    FIPS_SETTLE_FAULT_RedialHeardFloored,
    FIPS_SETTLE_FAULT_RedialSlotIgnored,
    FIPS_SETTLE_FAULT_RedialLegKept,
    FIPS_SETTLE_FAULT_RedialEntryKeptAtPromote,
    FIPS_SETTLE_FAULT_RedialMemoryIgnored,
    FIPS_SETTLE_FAULT_RedialSkipsDampener,
    FIPS_SETTLE_FAULT_RedialTearsDownAtMsg1,
    FIPS_SETTLE_FAULT_RedialNeverExpires,
    FIPS_SETTLE_FAULT_AuthorizedRedialUnnoted,
    FIPS_SETTLE_FAULT_RedialMemoryLapses,
    FIPS_SETTLE_FAULT_RedialSwapHeard,
    FIPS_SETTLE_FAULT_RedialKeptAtDisconnect,
    FIPS_SETTLE_FAULT_UnsendableKeepsRedial,
    FIPS_SETTLE_FAULT_AttributeBySetupOnly,
    FIPS_SETTLE_FAULT_AttributeByCurrent,
    FIPS_SETTLE_FAULT_PathMoveIgnored,
    FIPS_SETTLE_FAULT_SwitchIgnored,
    FIPS_SETTLE_FAULT_SwitchLeavesPeer,
    FIPS_SETTLE_FAULT_HeardMovesPeer,
    FIPS_SETTLE_FAULT_FollowResets,
    FIPS_SETTLE_FAULT_SwitchWritesLive,
    FIPS_SETTLE_FAULT_ReleaseKeepsPair,
    FIPS_SETTLE_FAULT_StateIgnored,
    FIPS_SETTLE_FAULT_PunchedDropped,
    FIPS_SETTLE_FAULT_SendFailedDropsLeg,
    FIPS_SETTLE_FAULT_OrphanedIgnored,
    FIPS_SETTLE_FAULT_OrphanedNoRetry,
    FIPS_SETTLE_FAULT_RestartNoRetry,
    FIPS_SETTLE_FAULT_RedialNoRetry,
    FIPS_SETTLE_FAULT_RemovalClosesEveryPair,
    FIPS_SETTLE_FAULT_CapLifted,
    FIPS_SETTLE_FAULT_AnswerBeforeAdmit,
    FIPS_SETTLE_FAULT_CapNotCounted,
    FIPS_SETTLE_FAULT_ReachOneMore,
    FIPS_SETTLE_FAULT_FlightByHash,
    FIPS_SETTLE_FAULT_FrameOpensAny,
    FIPS_SETTLE_FAULT_NoHeartbeat,
    FIPS_SETTLE_FAULT_ReplyNewConn,
    FIPS_SETTLE_FAULT_EofDropped,
    FIPS_SETTLE_FAULT_BleSecondConn,
    FIPS_SETTLE_FAULT_SendAtDial,
    FIPS_SETTLE_FAULT_DelayUnbounded,
    FIPS_SETTLE_FAULT_ConnectRefusedSilent,
    FIPS_SETTLE_FAULT_RefusedConnKept,
    FIPS_SETTLE_FAULT_ReplacedConnKept,
    FIPS_SETTLE_FAULT_ShapeBlind,
    FIPS_SETTLE_FAULT_ShapeLoose,
    FIPS_SETTLE_FAULT_ExemptByPath,
    FIPS_SETTLE_FAULT_RightLinkOff,
    FIPS_SETTLE_FAULT_RightLinkPairOnly,
    FIPS_SETTLE_FAULT_RightLinkNoDirection,
    FIPS_SETTLE_FAULT_OneSessionNoSplit,
    FIPS_SETTLE_FAULT_OneSessionNoPending,
    FIPS_SETTLE_FAULT_OneSessionNoFresh,
    FIPS_SETTLE_FAULT_ClassifiedNoRefusal,
    FIPS_SETTLE_FAULT_ResyncKeepsAll,
    FIPS_SETTLE_FAULT_ModelDropsFallback,
    FIPS_SETTLE_FAULT_PacketStamp,
    FIPS_SETTLE_FAULT_StoredByBytes,
    FIPS_SETTLE_FAULT_StoredAnswersAny,
    FIPS_SETTLE_FAULT_YieldKept,
    FIPS_SETTLE_FAULT_RekeyOutlivesPeer,
    FIPS_SETTLE_FAULT_PreviousUnheard,
    FIPS_SETTLE_FAULT_ReachablePeeks,
    FIPS_SETTLE_FAULT_NoShrink,
    FIPS_SETTLE_FAULT_RecordedCompared,
    FIPS_SETTLE_FAULT_FrameCutoverMissed,
    FIPS_SETTLE_FAULT_CandidateHoldsNothing,
    FIPS_SETTLE_FAULT_AgedTieBreak,
    FIPS_SETTLE_FAULT_WireByNumber,
    FIPS_SETTLE_FAULT_MoveBuildsFresh,
    FIPS_SETTLE_FAULT_RenewSkipped,
    FIPS_SETTLE_FAULT_ModeIgnored,
    FIPS_SETTLE_FAULT_ReplaceDropped,
    FIPS_SETTLE_FAULT_PathsUnmirrored,
    FIPS_SETTLE_FAULT_RestartDropped,
    FIPS_SETTLE_FAULT_KeyFromView,
    FIPS_SETTLE_FAULT_KeyOmitsBundle,
    FIPS_SETTLE_FAULT_HeardFeedDropped,
    FIPS_SETTLE_FAULT_ArrivalIgnored,
    FIPS_SETTLE_FAULT_ProvenCompared,
    FIPS_SETTLE_FAULT_RedialCompared,
    FIPS_SETTLE_FAULT_RedialUnnoted,
    FIPS_SETTLE_FAULT_ReopenedCompared,
    FIPS_SETTLE_FAULT_MoveBuildsStale,
    FIPS_SETTLE_FAULT_ReplaceKeepsMachine,
    FIPS_SETTLE_FAULT_LeaseForgotten,
    FIPS_SETTLE_FAULT_CloseSkipped,
    FIPS_SETTLE_FAULT_OrphanKept,
    FIPS_SETTLE_FAULT_BackoffDoubles,
    FIPS_SETTLE_FAULT_FirstResendAtInterval,
    FIPS_SETTLE_FAULT_OriginIgnored,
    FIPS_SETTLE_FAULT_ConnectingUnlinked,
    FIPS_SETTLE_FAULT_ConnectingCloses,
    FIPS_SETTLE_FAULT_UnsendableCloses,
    FIPS_SETTLE_FAULT_TeardownIgnoresKeep,
    FIPS_SETTLE_FAULT_UnpinOwnOnly,
    FIPS_SETTLE_FAULT_TimerNever,
    FIPS_SETTLE_FAULT_COUNT
} fips_settle_fault;
#define FIPS_SETTLE_FAULT_RESENDS_MAX 64u
typedef struct fips_settle_faults {
    uint64_t bits[(FIPS_SETTLE_FAULT_COUNT+63u)/64u];
    uint8_t resends[FIPS_SETTLE_FAULT_RESENDS_MAX][FIPS_SETTLE_MESSAGE_CAPACITY+1u];
    size_t next_resend;
} fips_settle_faults;
FIPS_CORE_API const char *fips_settle_fault_name(fips_settle_fault fault);
FIPS_CORE_API bool fips_settle_fault_named(const char *name,fips_settle_fault *out);
FIPS_CORE_API bool fips_settle_fault_supported(fips_settle_fault fault);
FIPS_CORE_API const char *fips_settle_fault_boundary(fips_settle_fault fault);
FIPS_CORE_API fips_status fips_settle_fault_select(fips_settle_faults *set,fips_settle_fault fault,bool enabled);
/* Unknown/unsupported bits refuse atomically and do not change *previous. */
FIPS_CORE_API fips_status fips_settle_fault_enter(fips_settle_faults *set,fips_settle_faults **previous);
FIPS_CORE_API bool fips_settle_fault_on(fips_settle_fault fault);
FIPS_CORE_API void fips_settle_fault_resends_begin(void);
FIPS_CORE_API const uint8_t *fips_settle_fault_resend(const uint8_t *bytes,size_t length,size_t *written);
FIPS_CORE_API fips_instant fips_settle_fault_deadline(fips_instant now,fips_duration after);
#ifdef __cplusplus
}
#endif
#define SETTLE_FAULT(name) fips_settle_fault_on(FIPS_SETTLE_FAULT_##name)
#define settle_deadline fips_settle_fault_deadline
#else
#define SETTLE_FAULT(name) false
#define settle_deadline fips_deadline
#define fips_settle_fault_resends_begin() ((void)0)
#define fips_settle_fault_resend(bytes,length,written) (bytes)
#endif
#endif
