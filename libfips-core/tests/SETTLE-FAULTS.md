# Settlement fault inventory

Pinned upstream: `64f5b4d482cfe14abad108ffc2d92128f7658311`.

147 catalog names; 92 have C mutation sites. Site presence is not
a passing mutation result: `FipsSettleFaults` must kill every enabled mutation
with an invariant assertion after each runner passes with none.

No production fault selection, environment reads or mutable global state.
Test threads explicitly borrow caller-owned scopes through a TLS pointer.
Mutant resend scratch is bounded to64 outputs per poll; never used in production.
Unsupported names refuse activation, rather than silently doing nothing.

| Fault | C site / explicit boundary | Upstream sites |
|---|---|---|
| `IndexStale` | C derives its index directly from records; no cached index exists to leave stale | `links.rs` |
| `CloseByPeerAddress` | links.c | `links.rs` |
| `CloseBySetupAddress` | links.c | `links.rs` |
| `ProbingCounts` | links.c, table.c | `links.rs` |
| `KeepIgnored` | links.c | `links.rs` |
| `ReleasedSetupIndexed` | links.c | `links.rs` |
| `CapableIgnored` | links.c | `links.rs` |
| `UnheardInclusive` | gates.c | `gates.rs` |
| `HeardFromInstall` | gates.c | `gates.rs` |
| `HeardNeedsFrame` | gates.c | `gates.rs` |
| `DampenerFromRefusal` | gates.c | `gates.rs` |
| `OutboundGated` | gates.c | `gates.rs` |
| `DampenerAtMsg2` | gates.c | `gates.rs` |
| `ReplayUnbounded` | gates.c | `gates.rs` |
| `ReplaySeenIgnored` | gates.c | `gates.rs` |
| `HeardKeepsMemory` | gates.c | `gates.rs` |
| `FrameForgetsReplays` | table.c | `table.rs` |
| `SilentReadsHeard` | table.c | `table.rs` |
| `StandbyRoamResets` | join.c | `join.rs` |
| `MemoryNeverDropped` | gates.c | `gates.rs` |
| `SilentEarly` | gates.c | `gates.rs` |
| `SilentFlat` | gates.c | `gates.rs` |
| `IntervalTen` | params.c | `params.rs` |
| `LegNeverReaped` | legs.c, table.c | `legs.rs` |
| `ResendFresh` | legs.c, table.c | `legs.rs` |
| `AnswerFresh` | legs.c, table.c | `legs.rs` |
| `PollAhead` | legs.c, table.c | `legs.rs` |
| `DeadlineWraps` | faults.c | `params.rs` |
| `LeaseKept` | legs.c, table.c | `legs.rs` |
| `UnsentStaysDue` | legs.c | `legs.rs` |
| `UnsentDueAtWake` | legs.c | `legs.rs` |
| `UnsentPastCap` | legs.c | `legs.rs` |
| `MoveKeepsOldPair` | table.c | `table.rs` |
| `MoveKeepsInboundLink` | table.c | `table.rs`, `tests/legacy/master.rs` |
| `MoveClosesNewPair` | table.c | `table.rs` |
| `MoveLeavesDialOpen` | table.c | `table.rs` |
| `MoveNoReset` | table.c | `table.rs` |
| `MoveLeavesUnheard` | table.c | `table.rs`, `tests/legacy/master.rs` |
| `MoveKeepsReplayMemory` | table.c | `table.rs` |
| `IgnoreOwnDial` | table.c | `table.rs` |
| `CrossedRunsKeep` | table.c | `table.rs` |
| `TouchKeepsHeardInstant` | table.c | `table.rs` |
| `MovedLeavesEntry` | table.c | `table.rs` |
| `EpochAtCutover` | table.c | `table.rs` |
| `ModelFallbackInCore` | table.c | `table.rs` |
| `ReplacementUngated` | table.c | `table.rs` |
| `RedialInstallsAtSent` | table.c | `table.rs` |
| `LegHeardIgnored` | table.c | `table.rs` |
| `RedialUngated` | gates.c | `gates.rs` |
| `RedialHeardFloored` | gates.c | `gates.rs` |
| `RedialSlotIgnored` | table.c | `table.rs` |
| `RedialLegKept` | table.c | `table.rs` |
| `RedialEntryKeptAtPromote` | table.c | `table.rs` |
| `RedialMemoryIgnored` | table.c | `table.rs` |
| `RedialSkipsDampener` | table.c | `table.rs` |
| `RedialTearsDownAtMsg1` | table.c | `table.rs` |
| `RedialNeverExpires` | table.c | `table.rs` |
| `AuthorizedRedialUnnoted` | table.c | `table.rs` |
| `RedialMemoryLapses` | gates.c | `gates.rs` |
| `RedialSwapHeard` | table.c | `table.rs` |
| `RedialKeptAtDisconnect` | table.c | `table.rs` |
| `UnsendableKeepsRedial` | table.c | `table.rs` |
| `AttributeBySetupOnly` | links.c | `links.rs` |
| `AttributeByCurrent` | table.c | `table.rs` |
| `PathMoveIgnored` | table.c | `table.rs` |
| `SwitchIgnored` | table.c | `table.rs` |
| `SwitchLeavesPeer` | table.c | `table.rs` |
| `HeardMovesPeer` | table.c | `table.rs` |
| `FollowResets` | table.c | `table.rs` |
| `SwitchWritesLive` | table.c | `links.rs` |
| `ReleaseKeepsPair` | table.c | `table.rs` |
| `StateIgnored` | table.c | `table.rs` |
| `PunchedDropped` | table.c | `table.rs` |
| `SendFailedDropsLeg` | table.c | `table.rs` |
| `OrphanedIgnored` | table.c | `table.rs` |
| `OrphanedNoRetry` | table.c | `table.rs` |
| `RestartNoRetry` | table.c | `table.rs` |
| `RedialNoRetry` | table.c | `table.rs` |
| `RemovalClosesEveryPair` | table.c | `table.rs` |
| `CapLifted` | table.c | `table.rs` |
| `AnswerBeforeAdmit` | table.c | `table.rs` |
| `CapNotCounted` | table.c | `table.rs` |
| `ReachOneMore` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/explore.rs` |
| `FlightByHash` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/world.rs` |
| `FrameOpensAny` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/world.rs` |
| `NoHeartbeat` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/party.rs` |
| `ReplyNewConn` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/world.rs` |
| `EofDropped` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/world.rs` |
| `BleSecondConn` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/world.rs` |
| `SendAtDial` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/world.rs` |
| `DelayUnbounded` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/world.rs` |
| `ConnectRefusedSilent` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/world.rs` |
| `RefusedConnKept` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/joint/party.rs`, `tests/legacy/maint.rs`, `tests/legacy/master.rs`, `tests/party.rs` |
| `ReplacedConnKept` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/joint/party.rs`, `tests/party.rs` |
| `ShapeBlind` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/world.rs` |
| `ShapeLoose` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/world.rs` |
| `ExemptByPath` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/oracle.rs` |
| `RightLinkOff` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/oracle.rs` |
| `RightLinkPairOnly` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/oracle.rs` |
| `RightLinkNoDirection` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/oracle.rs` |
| `OneSessionNoSplit` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/oracle.rs` |
| `OneSessionNoPending` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/oracle.rs` |
| `OneSessionNoFresh` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/oracle.rs` |
| `ClassifiedNoRefusal` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/shadow.rs` |
| `ResyncKeepsAll` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/shadow.rs` |
| `ModelDropsFallback` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/party.rs` |
| `PacketStamp` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/joint/party.rs` |
| `StoredByBytes` | table.c | `table.rs` |
| `StoredAnswersAny` | table.c | `table.rs` |
| `YieldKept` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/shadow.rs` |
| `RekeyOutlivesPeer` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/shadow.rs` |
| `PreviousUnheard` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/shadow.rs` |
| `ReachablePeeks` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/joint/party.rs` |
| `NoShrink` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/sim.rs` |
| `RecordedCompared` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/shadow.rs` |
| `FrameCutoverMissed` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/shadow.rs` |
| `CandidateHoldsNothing` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/joint/party.rs` |
| `AgedTieBreak` | alternative candidate-policy experiments; not production rules | `table.rs` |
| `WireByNumber` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/world.rs` |
| `MoveBuildsFresh` | join.c | `join.rs` |
| `RenewSkipped` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/joint/machines.rs` |
| `ModeIgnored` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/joint/machines.rs` |
| `ReplaceDropped` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/joint/route.rs` |
| `PathsUnmirrored` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/joint/route.rs` |
| `RestartDropped` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/joint/route.rs` |
| `KeyFromView` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/joint/machines.rs` |
| `KeyOmitsBundle` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/joint/machines.rs` |
| `HeardFeedDropped` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/joint/feeds.rs` |
| `ArrivalIgnored` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/joint/mod.rs` |
| `ProvenCompared` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/shadow.rs` |
| `RedialCompared` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/shadow.rs` |
| `RedialUnnoted` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/drive.rs`, `tests/joint/party.rs`, `tests/legacy/master.rs` |
| `ReopenedCompared` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/shadow.rs` |
| `MoveBuildsStale` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/joint/machines.rs` |
| `ReplaceKeepsMachine` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/joint/party.rs` |
| `LeaseForgotten` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/joint/party.rs` |
| `CloseSkipped` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/joint/party.rs` |
| `OrphanKept` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/joint/route.rs` |
| `BackoffDoubles` | params.c | `params.rs` |
| `FirstResendAtInterval` | legs.c | `legs.rs` |
| `OriginIgnored` | legs.c | `legs.rs` |
| `ConnectingUnlinked` | table.c | `table.rs` |
| `ConnectingCloses` | table.c | `table.rs` |
| `UnsendableCloses` | table.c | `table.rs` |
| `TeardownIgnoresKeep` | table.c | `table.rs` |
| `UnpinOwnOnly` | table.c | `table.rs` |
| `TimerNever` | guard of the unported world/joint/legacy/comparison infrastructure | `tests/joint/party.rs` |

The three candidate-policy alternatives are not enabled by this API.
They need the retained-session test-shell protocol, not a production config knob.
