# Investigation: Critique of "A Scion-Like Source-Control Architecture Built on Nostr and Blossom" vs libhanami

## Summary
The document is a strong, spec-accurate proposal for a new native Nostr/Blossom SCM with a Git projection; its critical flaw is overclaiming causal event chains as compare-and-swap branch state, plus unstandardized-kid dependency and two blind spots (agent signing authority, multi-author 30618 projection). libhanami implements only the Git-bridge layer of it — none of the native core — and has four bridge-correctness bugs the document's own guidance predicts. Full critique and per-layer matrix in Investigator Findings.

## Symptoms
- Task: conceptual critique of the design document (extracted from a ChatGPT research export at
  `/Users/bizarro/Downloads/A Scion-Like Source-Control Architecture Built on Nostr and Blossom.txt`;
  extracted text staged at `/var/folders/mt/rcd9cp6j61sbdjzt0r576yv40000gn/T/opencode/scion_doc.md`)
  and a comparison against the actual `libhanami` implementation in this repo (`libhanami/`).

## Background / Prior Research
(Phase 1.5 explore agents populate this.)

### External fact verification (Scion / NIP-34 / Blossom / GRASP)
- Scion blog claims (changesets=WAL entries vs first parent, content-addressed immutable objects, derived indexes, API-first, optional/virtualized sparse working copies, Spanner CAS refs): CORRECT.
- Scale figures ("approached 100 GB", "about 25M checkouts/day"): PARTIALLY CORRECT — blog says "almost 100 GB", "on the order of 25 million", and excludes dev/agent/research checkouts.
- NIP-34 kinds and tag semantics (30617/30618/1617/1618/1619/1621/1630-1633/10317; refs values are Git commit IDs; clone tags are clone URLs; maintainers + r/euc): CORRECT per current NIP-34 draft.
- Blossom BUD-01/02/04/11, kind 10063 (BUD-03) server list: CORRECT.
- BUD-10: PARTIALLY CORRECT — the URI form is `blossom:<sha256>.<ext>?xs=...&as=...`, not `blossom://` as the document repeatedly writes.
- GRASP-01: PARTIALLY CORRECT — exists, Git Smart HTTP, pushes must match latest 30618 and respect recursive maintainer set; "authorized pushes from 30618" is loose shorthand (maintainers govern authorization; 30618 constrains refs/tips).

### libhanami git archaeology
- Introduced 2026-04-20 (e82510d2e) as a C/libgit2 bridge between Blossom object storage and Nostr coordination: OID↔Blossom-hash index, BUD-02 auth, Blossom HTTP client, `docs/libhanami-design.md`, ~50 tests. Design doc describes a Nostr-coordinated, Blossom-backed **Git/libgit2** system.
- Milestones: 5048764c4 (Blossom ODB backend + NIP-34 30617/30618/patches/statuses), bb6f8a23e (Nostr publish/filter/query), 17209dbf9 (NIP-34 RefDB backend — refs as kind 30618 events), 17ae41b0f (blossom:// transport), 94fff7aac (config), e67371382 (GRASP URL/push/fetch), feec7e483 (clone discovery, object walking/upload), ed446dae4 (server-capability probing + batch BUD-02 upload; beads nostrc-xeby, nostrc-ypn2), 9d2493899 + 045b94d43 (blossom shim + porthome pusher/fetcher, Content-Type), 6e18db88f (probe-driven shim selection; beads prli, si30, 4f9v).
- **No commits, branches, or beads mention Scion, changesets, causal state-transition events, derived indexes, or a native (non-Git) Nostr VCS.** libhanami implements the document's *Git-compatibility projection* layer only.
- No libhanami entry in VERSION_MANIFEST.md; no libhanami/README.

## Investigator Findings

### Scope, evidence, and verdict

- **Inspected checkout:** `/Users/bizarro/Documents/Projects/nostrc`, HEAD `0d4dd1fafa512379f36917d2d25e41b9d1c2485f`, 2026-10-13. This is a source-grounded design investigation, not a live interoperability, performance, or production-readiness certification. Existing staged `.beads/issues.jsonl` changes, the dirty `third_party/nostrdb` submodule, and the pre-existing untracked report were present before this investigation.
- **Document examined:** the supplied extracted `scion_doc.md`, including “Proposed native data model,” “Consistency, security, and concurrency,” and “Implementation roadmap and developer experience.” Section headings below refer to that document, not to libhanami's design document. External links were checked against primary specifications on 2026-10-13.
- **Bottom line:** the document is a promising proposal for a **new source-control system with a Git bridge**, not a description of libhanami, nor a small extension of its current object model. libhanami deliberately makes libgit2 the semantics engine and NIP-34 the ref layer (`docs/libhanami-design.md:9–29`). Its implementation offers useful bridge primitives but also has material integration gaps. “Git-compatibility projection layer” describes its architectural role; it must not imply that a complete, verified projection pipeline already exists.
- **Version assessment:** documentation-only investigation; no shipped artifact, public API, wire format, or component version changes. **No bump.** No implementation changes or full Nostr/backlog audit were requested or performed.

### A. Conceptual critique

#### A1. Strengths worth preserving

1. **Separate data, assertions, authorization, and availability.** “Separate the four identities,” “Nostr changeset assertion,” and “Malicious storage or index workers” correctly distinguish content hashes from signed provenance and branch authority. Publishing immutable candidates without moving `main` is a useful concurrency boundary. The separation is consistent with Scion's independent changeset creation and ref update, but does not inherit Scion's coordination guarantees automatically. [Scion primary description](https://www.anthropic.com/engineering/redesigning-source-control).
2. **Put bulk data off relay and make sparse reads hash-directed.** “Changeset descriptor,” “Tree manifests,” and “Clone and manifest reconstruction” give a concrete integrity chain from accepted root to selected file bytes. Ordered parents, raw-byte path encoding, explicit modes, and canonical array ordering address real cross-language interoperability hazards. A result root also gives a strong validation target for applying a delta.
3. **Do not overload Git fields.** “Protocol mapping” and “GRASP as a compatibility facade” correctly preserve `30618` Git commit IDs, PR `c` tips, and usable Git clone URLs rather than slipping native manifest hashes into them. `1617` format-patch and `1618` PR semantics are not native changeset semantics. These warnings are substantively right. [NIP-34](https://github.com/nostr-protocol/nips/blob/master/34.md).
4. **The proposal acknowledges important limits.** “Competing acceptance policies” favors a sequencer for canonical branches; “Verification and trust” explicitly permits trusting an indexer; “Availability and retention” distinguishes integrity from durability; “Private repositories” does not promise confidentiality from plaintext Blossom. These are qualifications to preserve, not omissions to attribute falsely to the author.

#### A2. High: a causal event chain is not compare-and-swap

**Document:** “Add an immutable state-transition log,” “Competing acceptance policies,” “Relay disagreement and replay.”

The claim that naming the old state makes movement “equivalent to a signed compare-and-swap” is too strong. Counterexample: start at accepted state A, sequence 7. Two authorized writers independently sign B = `(prev=A, seq=8, tip=b)` and C = `(prev=A, seq=8, tip=c)`. Both satisfy the stated signature, predecessor, and sequence checks. Relays can accept both; clients seeing different partitions can each accept one. Linking to A makes the conflict detectable, not mutually exclusive. In particular, rejecting a “transition already consumed by canonical successor” makes arrival order decisive unless canonical successor selection is separately defined.

The recommended single sequencer can supply the missing serialization **only with durable atomic allocation of the next predecessor/sequence**, restart recovery, and an explicit equivocation policy. A signing key alone is not a sequencer lock: two processes with the same key can fork. Threshold approval needs quorum-intersection and honest-signer/non-double-signing assumptions, rounds/locking, and a finality rule; arbitrary M-of-N approvals can certify both branches. A deterministic tie-break can give eventual convergence after common knowledge, but not linearizable CAS and immediate finality under partition. The CRDT alternative explicitly changes the single-head contract rather than implementing CAS.

**Required specification:** say whether resolution returns tentative, finalized, conflicted, or unavailable state; define equivocation/fork recovery, force updates, rollback, branch deletion/recreation, and sequencer/key rotation. Bind authorization policy and its version to historical state, rather than applying today's maintainer list retroactively. The sample transition also omits a distinct expected-old-changeset field despite the prose listing one; either derive it unambiguously from authenticated `prev` or encode it. These are prerequisites to a writable canonical branch, not merely P8 hardening. Scion's Spanner-row CAS is an actual arbitration operation; a relay carrying competing signed claims is not the same mechanism. [Scion ref coordination](https://www.anthropic.com/engineering/redesigning-source-control).

#### A3. High: immutable event bytes do not guarantee retained or discoverable history

**Document:** “Nostr changeset assertion,” “Availability and retention,” “Relay disagreement and replay.”

Regular events avoid addressable-event replacement, but their expected storage convention is not a permanent archive guarantee. NIP-01 describes kind/storage conventions, not an append-only replicated log service. [NIP-01](https://github.com/nostr-protocol/nips/blob/master/01.md). NIP-09 permits same-author deletion requests and recommends relays stop serving matching events; it cannot guarantee universal erasure either. [NIP-09](https://github.com/nostr-protocol/nips/blob/master/09.md).

Two Blossom copies of blobs/trees/descriptors do not preserve the **signed acceptance chain and historical policy** needed to prove which root was accepted. Archive exact signed transition/approval/policy events too; define trusted genesis/checkpoints, event-by-ID recovery, retention obligations, and disaster recovery. A newer signed checkpoint can accelerate bootstrap, but changes the historical-verification trust boundary unless accompanied by the requisite chain/proof. Hashes cannot reveal a withheld successor or prove that a relay has shown the latest state. NIP-77 reconciles sets peers possess; it cannot restore data absent everywhere or manufacture finality.

“Unknown previous state after timeout/reconciliation” should mean **unresolved/quarantined**, not permanently invalid. A timeout is not cryptographic evidence that a valid predecessor never existed. Likewise, two successful HEAD checks are point-in-time availability observations, not durability contracts. Count complete reachable object closure, independent failure domains, paid retention, repair, and subsequent storage loss. BUD-01's advisory `Sunset` mechanism supports the document's warning, but is not a guarantee of retention until that date. [BUD-01](https://github.com/hzrd149/blossom/blob/master/buds/01.md).

#### A4. Medium: descriptor size is mostly handled; relay and verification budgets remain unspecified

**Document:** “Changeset descriptor,” “Nostr changeset assertion,” “Tree manifests,” “Changeset publication and integration.”

Do **not** critique this draft as placing every changed path in a Nostr event: it puts the descriptor on Blossom and publishes a small pointer. A million-path descriptor therefore is not directly a million-path relay event. Remaining relay limits include parent-event hints on many-parent changesets, approval fan-out, and full-ref `30618` projections. Bound them and probe message, content, and tag budgets; NIP-11 exposes those limits, but its example numbers are not universal relay defaults. [NIP-11 limitations](https://github.com/nostr-protocol/nips/blob/master/11.md#server-limitations).

Offloading bytes does not bound work. Specify maximum descriptor/manifest size, parent count, recursion depth, decoded path length, total fetch bytes, and verification CPU. Bound missing-parent fetches and untrusted proposal indexing. Use pagination/sharding with hash-stable schemas rather than reading unbounded JSON arrays. Canonical JSON alone does not define valid source-control input: reject duplicate decoded names/paths, NUL and traversal components, inconsistent modes/types/sizes, duplicate-key ambiguity, invalid numeric ranges, and unsafe materialization through symlinks. Define gitlinks/submodules, symlink payloads, rename/copy interpretation, and non-first-parent merge semantics.

#### A5. High: a source-pinned index is attributable, not proven correct or automatically reproducible

**Document:** “Indexes should be disposable,” “Blame,” “Verification and trust,” “Architecture components.”

The four-step verification policy correctly admits a trust choice; the stronger surrounding rhetoric about adversarial services not being authoritative needs qualification. An indexer can hash and sign a completely wrong merge-base or omit a path-history entry while naming the correct source tip. A content hash proves the cached answer's bytes, not its derivation. Two agreeing workers do not prove independence or correctness, and a Merkle inclusion proof alone cannot establish completeness of a query result.

Pin semantic algorithm/version and options, not only serialization schema: first-parent versus all-parent history, rename/copy detection, diff/blame algorithm, line splitting, merge provenance, and deterministic tie handling can change valid answers. Separate semantic reproducibility from byte-identical index encoding. The example blame map keyed by `file_blob` needs source tip/path/parent-policy context: identical bytes may have different provenance. Require locally verifiable ancestry/results for authority-sensitive operations, a clearly stated trusted-indexer mode, or specified proofs. Correct fallbacks need all required source/policy history retained and an explicit cost budget; “recompute locally” is not cheap for a thin client.

#### A6. Medium/high: native-to-Git compatibility is asymmetric and cannot be only a read projection

**Document:** “Deterministic Git materialization,” “Packfile import and export,” “GRASP as a compatibility facade,” “Migration of an existing Git repository.”

Preserving imported raw commit/tree/tag representations is the right policy. The native descriptor example, however, supplies only a pubkey/timestamp/message, not all the author/committer bytes, timezones, headers, signatures, modes, and special entry semantics later required for deterministic Git materialization. Specify the missing metadata and mapping rules in the format, or explicitly restrict round-trip guarantees. Raw Git path order also needs translation into Git's tree comparator, not blind reuse of native raw-name sorting.

Conventional clients can read a materialized facade, but **writes** need a defined ingress path: Git push/PR → validated native import → policy/acceptance → updated projection. Standard Git clients do not themselves publish native transitions or Nostr `30618` authorization. Without a bridge service or Nostr-aware helper, “ordinary Git compatibility” is read compatibility plus a restricted contribution route, not symmetric read/write equivalence. Define behavior for force-push, tags, ref deletion, unmappable metadata, partial imports, and EUC ancestry. Prevent a later Git or Nostr announcement from silently replacing native accepted state.

Publication spans Blossom, native state, NIP-34, and one or more GRASP services without a shared transaction. Distinguish native acceptance from projection readiness; record resumable, idempotent stages, relay acceptance/rejection, and partial replica outcomes. A projection failure must not become an ambiguous successful native integration. GRASP-01 constrains pushes to its latest state and recursive maintainers; its purgatory model supports state-before-data, as the document proposes. [GRASP-01](https://ngit.dev/protocol/grasp/specification/01).

#### A7. Medium: the proposal adds eager snapshot work while borrowing Scion's cheap-write argument

**Document:** “Changeset descriptor,” “Tree manifests,” “Changeset publication and integration,” “Prioritized roadmap.”

A required result root is useful, but producing canonical directory manifests on every changeset is not a pure append-only WAL write. Changing one leaf rewrites each ancestor manifest; with flat directory arrays, a huge directory requires serializing all its entries even for a one-file edit. Thus cost includes directory fan-out, not just changed bytes. “Sharding later” leaves an agent-scale hot path unresolved. Benchmark this against existing Git trees and partial clone before asserting a scalability win.

Uncompressed exact-source identity is simple but does not remove transfer/storage costs; no pack/delta transfer can be a regression for many tiny files or highly similar large versions. Per-object HEAD followed by PUT adds requests, signing, and latency. Define batch/pipeline/concurrency limits and a stable encoding boundary; if a storage shim or compression changes uploaded bytes, logical source hash and stored Blossom hash are no longer identical. Shared caches, eviction, regional failure isolation, and bounded upstream concurrency deserve explicit design. Scion's measured gains rely on a service/cache topology and workload, not just replacing commit objects with descriptors. [Scion cache architecture and scope](https://www.anthropic.com/engineering/redesigning-source-control).

#### A8. Medium/high: privacy is acknowledged, but deferral is a product constraint

**Document:** “Private repositories,” “Storage discovery,” “Nostr changeset assertion,” “Prioritized roadmap.”

The public-first caveat is honest. It still excludes workloads needing private source or path-scoped access; it is not equivalent to Scion's enterprise IAM model. Public `a`/`p`/`e` tags, author hints, commit times, graph topology, and index segments disclose collaboration metadata even if later file bodies are encrypted. Unsalted public content hashes expose equality and permit known-content/existence tests. Encrypting blobs alone does not hide paths/manifests, and deleting one relay event cannot retract mirrored bytes. Define key distribution/rotation, forward access changes, encrypted index/cache behavior, and accidental-secret recovery before claiming private-repository support. HTTP access controls and end-to-end encryption are distinct choices; encryption also changes deduplication and source-versus-storage identity.

#### A9. High for interoperability: “new NIP needed” is a central dependency, not a final polish step

**Document:** “Protocol mapping,” “Competing acceptance policies,” “Prioritized roadmap,” “Architectural conclusion.”

The draft correctly labels `K_CHANGESET`, `K_STATE_TRANSITION`, index announcements, and `native` tags as proposals. Consequently existing Nostr/Blossom/GRASP support does not establish interoperable native SCM support. P0 must cover not just JCS hashes but repository/policy binding, genesis, state acceptance/finality, schema negotiation, unsupported-version refusal, bounded retrieval, and projection ownership. P9 can pursue public standardization after experiments; independently implemented clients still need a frozen experimental contract and shared test vectors **before** P2/P4 interoperability claims. Assign event kinds deliberately rather than treating an illustrative range as allocation authority.

The proposed first sparse reconstruction experiment validates the immutable reader only. Add a partition/equivocation experiment and a cold-client import/round-trip experiment before concluding that branch state and Git interoperability can simply be layered on without revisiting semantics.

#### A10. Correction to prior research: BUD-10 URI syntax is right in the supplied document

**Document:** “Executive summary,” “Protocol mapping,” “Repository snapshot manifest,” “Clone and manifest reconstruction.”

The extracted source consistently uses `blossom:` for BUD-10, including `.json`/`.bin` examples and `xs`/`as` hints. A literal search finds **no `blossom://` occurrence** in that source. Therefore the earlier Background bullet alleging repeated `blossom://` usage is not supported by this version; it may describe another version, but must not be reported as a defect here. BUD-10's actual form is `blossom:<hash>.<ext>?...`. [BUD-10](https://github.com/hzrd149/blossom/blob/master/buds/10.md).

The real distinction is with **libhanami's separate repository transport**, which parses `blossom://<host>/<owner>/<repo>` and constructs an HTTPS endpoint (`libhanami/src/hanami-transport.c:40–110`; `docs/libhanami-design.md:150–162`). That is not a BUD-10 blob URI or a BUD-10 resolver. Do not conflate or silently rename the two protocols.

### B. Architectural layers versus actual libhanami

**Classification rule:** “implemented” means a concrete local code path exists, not that deployment conformance has been demonstrated. “Partial” identifies a real subset or an incomplete integrated path. “Absent” concerns the document's native feature, not a missing responsibility of this intentionally Git-oriented library. All paths below are relative to this checkout.

| Document layer / section | Classification | Actual behavior and evidence |
|---|---|---|
| **Blob storage and identity** — “Separate the four identities,” “Changeset descriptor” | **Implemented Git-object storage; partial match to proposed native layer** | ODB write uploads libgit2's raw object **content**, hashes that buffer with SHA-256, and records Git OID, type, and size (`libhanami/src/hanami-odb-backend.c:120–154`; `libhanami/src/hanami-index.c:337–359`). Read requires an OID-map entry before GET and optionally verifies the Blossom hash (`hanami-odb-backend.c:45–92`). It accepts Git trees/commits/tags as object payloads too, not just file blobs; these are not native JSON manifests. The index names the four normal types (`hanami-index.c:61–70`). Design explicitly keeps Git OID distinct from SHA-256 of raw content (`docs/libhanami-design.md:57–75`). |
| **Changeset / event model** — “Changeset descriptor,” “Nostr changeset assertion” | **Absent native model; implemented Git/NIP-34 event helpers** | State publication creates `nip34_create_repo_state`, signs, and publishes it (`libhanami/src/hanami-nostr.c:267–293`); repository publication uses `nip34_create_repo_announcement` (`hanami-nostr.c:237–264`). No native descriptor parser, JCS changeset hash, or `K_CHANGESET` creation path exists in the inspected `libhanami/src` surface. Git remains the semantics engine (`docs/libhanami-design.md:18–29, 92–104`). |
| **Branch state / concurrency** — “Add an immutable state-transition log” | **Partial Git ref layer; absent native causal log or distributed CAS** | RefDB loads one fetched `30618`, clears its cache, and imports refs/HEAD (`libhanami/src/hanami-refdb-backend.c:168–210`). Writes compare `old`/`old_target` only against an existing local cached ref when `!force`, then mutate cache and publish a complete state (`hanami-refdb-backend.c:476–520`). This is a local optimistic check, not remote arbitration. The design's “latest”/subscription claim (`docs/libhanami-design.md:131–146`) exceeds the actual first-result query, discussed below. |
| **Recursive tree manifests / sparse reader** — “Tree manifests,” “Clone and manifest reconstruction” | **Absent native trees; partial lazy Git-object retrieval** | Git tree bytes can travel through the generic ODB, but no recursive SHA-256 JCS directory format is implemented. Fetch negotiation/download-pack are no-op/lazy-ODB paths (`libhanami/src/hanami-transport.c:306–356`). `hanami_clone` creates a real local Git repository and calls `git_checkout_tree`, not a path-directed native sparse reader (`libhanami/src/hanami.c:334–386`). Design instead calls for local Git reconstruction (`docs/libhanami-design.md:359–370`). |
| **Derived indexes** — “Indexes should be disposable,” “Commit graph,” “Path history,” “Blame” | **Absent proposed history/query indexes; implemented OID lookup index only** | `oid_map` stores `(git_oid, blossom_hash, obj_type, obj_size, timestamp)` with a reverse lookup index (`libhanami/src/hanami-index.c:35–43`). SQLite persistence exists (`hanami-index.c:87–150`), but no segmented ancestry/path/blame snapshots or source-pinned query service. Design §4.4 also means OID translation, not derived source-control computation (`docs/libhanami-design.md:164–183`). |
| **NIP-34 / Git projection** — “Protocol mapping,” “Deterministic Git materialization” | **Implemented 30617/30618 primitives; partial interoperable bridge; absent native materializer** | RefDB publishes ordinary Git ref targets and HEAD (`libhanami/src/hanami-refdb-backend.c:217–291`); announcement/state wrappers use NIP-34 (`hanami-nostr.c:237–293`). High-level Blossom push enumerates Git ODB objects and publishes Git refs (`libhanami/src/hanami.c:479–566`). There is no native→Git conversion engine because its inputs are already Git objects. Clone endpoint interpretation conflicts with the document's correct `clone` warning (`hanami.c:309–320`). Design's event table is a usage plan, not evidence all collaboration workflows are implemented (`docs/libhanami-design.md:92–104`). |
| **GRASP** — “Blossom mirror plus GRASP push,” “GRASP as a compatibility facade” | **Partial client helpers, not a GRASP server or native projection pipeline** | `hanami_push_to_grasp` uses `git_remote_push` and subsequently publishes state; failures publishing state can still return success (`libhanami/src/hanami-grasp.c:575–686`). Fetch uses standard libgit2 Smart HTTP and explicitly leaves Nostr ref verification TODO (`hanami-grasp.c:693–730`). No GRASP helper calls occur in `hanami-transport.c`, `hanami-odb-backend.c`, or `hanami.c`. GRASP is not a native acceptance authority here. |
| **Transport and storage discovery** — “Storage discovery,” “Clone and manifest reconstruction” | **Implemented custom Git transport registration; partial transfer path; absent BUD-10 resolution** | Parser expects repository-oriented `blossom://`, not hash-oriented `blossom:` (`libhanami/src/hanami-transport.c:40–110`); registration wires libgit2 callbacks (`hanami-transport.c:520–578`). Download delegates to lazy ODB reads; push ignores `git_push *` and advertises current refs (`hanami-transport.c:306–463`). No native URI `xs`/`as`→10063 resolver is supplied by this parser. This matches the custom transport intent, not the new data model (`docs/libhanami-design.md:148–162`). |
| **Clients / API-first SDK and CLI** — “C / libgit2 stack,” “Agent UX,” “CLI UX” | **Implemented C/libgit2 library entry points; absent native agent API/CLI and browser engine** | `hanami_repo_open` creates a bare Git repository and attaches ODB/RefDB; `hanami_clone` creates/checks out a local Git repository (`libhanami/src/hanami.c:121–248, 251–386`). These are not `createChange`, native `integrate`, history/blame RPCs, or the proposed `nscm` CLI. The design names application integration above libgit2, not an independent SCM engine (`docs/libhanami-design.md:31–52, 232–291`). No inference about a separately maintained BlossomFS/browser project is made from this C checkout. |

**Absence checks:** searched all 12 files under `libhanami/src` for native changeset/state/index/JCS terminology and inspected the full component file inventory, the actual ODB/index/ref/transport paths, and design §§1–4 and §9. The positive architectural evidence above is stronger than keyword absence alone. Findings do not claim that native features are absent from unrelated projects or that arbitrary Git tree objects are unsupported.

### C. Concrete implementation findings that change the comparison

#### C1. Raw-content hashing contradicts the proposed wrapped-object mapping

**Document:** “Deterministic Git materialization.”

The proposal computes Blossom SHA-256 over `type + space + size + NUL + payload`; libhanami computes it over **payload alone**. This is a deliberate difference, not two descriptions of the same map. The ODB and high-level uploader both use the raw buffer (`libhanami/src/hanami-odb-backend.c:120–145`; `libhanami/src/hanami.c:411–442`), matching `docs/libhanami-design.md:57–75`. Header-inclusive Git SHA-1/SHA-256 helpers exist separately (`libhanami/src/hanami-index.c:361–424`) but are not the Blossom upload hash.

Consequences: file-blob hashes match exact source bytes; identical payload bytes of different Git types share a Blossom hash, with type supplied by the map. A SHA-256 Git OID does **not** automatically equal this stored Blossom hash, because Git includes its header. Adopting the document's alternative requires a versioned dual-format/migration strategy, not merely renaming the existing hash field. Neither format is intrinsically invalid; the incompatible byte domains must be explicit. The presence of SHA-256 Git hash helpers also does not establish end-to-end SHA-256-repository support: the high-level push and transport serialize some ref targets into 41-byte buffers (`libhanami/src/hanami.c:536–539`; `libhanami/src/hanami-transport.c:428–430`).

#### C2. The OID index is currently required discovery metadata, not a disposable history accelerator

**Document:** “Indexes should be disposable,” “Migration of an existing Git repository.”

A Git commit/tree contains Git OIDs, not the SHA-256 addresses of the raw payloads stored by this bridge. You cannot derive a payload's Blossom SHA-256 from its SHA-1 OID alone. ODB read returns `GIT_ENOTFOUND` before any GET if the local map lacks the requested OID (`libhanami/src/hanami-odb-backend.c:50–65`). Yet `attach_blossom_backends` creates a fresh `:memory:` index (`libhanami/src/hanami.c:135–140`), and the high-level uploader also uses and then closes a temporary map (`hanami.c:470–500`). Design §3.1 says map synchronization as a Blossom blob is optional, but the inspected clone path does not bootstrap that map (`docs/libhanami-design.md:72–75`; `hanami.c:334–386`).

Thus source uploads plus `30618` tips alone do not demonstrate a cold second-client clone. Existing local objects may make an operation succeed without testing remote bootstrap. The map is rebuildable if all original typed Git objects are otherwise available; it is **not** presently disposable from the perspective of a fresh hash-only reader. Native SHA-256 manifests avoid that particular traversal dependency, but imported Git objects still need a recoverable mapping or a complete alternate retrieval route.

#### C3. Local ref checks and first-relay reads do not establish even a global latest-state fold

**Document:** “The hard problem is branch state, not blob storage,” “Relay disagreement and replay.”

`hanami_nostr_build_state_filter` scopes kind/author/`d` and requests one result (`libhanami/src/hanami-nostr.c:315–330`). `query_single_event` visits relays in configured order, receives one event from the first successful source, then unsubscribes (`hanami-nostr.c:356–425`). It does not collect candidates and compare `created_at`/event IDs across relays. Its “most recent” and “EOSE or an event” comments should not be mistaken for an implemented cross-relay fold or an explicit EOSE-channel state machine.

RefDB refresh replaces its cached state from that result; network errors preserve existing cache (`libhanami/src/hanami-refdb-backend.c:168–210`). `old` comparisons are against that cache only (`hanami-refdb-backend.c:489–520`). The document correctly identifies the limitations of timestamp-LWW, but current libhanami is weaker than a convergent multi-relay LWW resolver. A native causal chain is absent, and adding one still needs A2's acceptance protocol.

#### C4. Success does not currently mean relay acceptance or globally replicated ref movement

**Document:** “Changeset publication and integration,” “Blossom mirror plus GRASP push.”

The Nostr publisher calls `nostr_relay_publish` on connected relays, ignores its return value, and reports success if any such call was made; this layer does not wait for `OK` or interpret `OK false` (`libhanami/src/hanami-nostr.c:212–235`). RefDB changes local cache first and returns zero even after publication failure, explicitly treating relay replication as best-effort (`libhanami/src/hanami-refdb-backend.c:288–307, 508–520`). This is incompatible with treating successful ref write as durable native branch acceptance. Underlying relay machinery may process frames; this API does not expose their acceptance outcome as its success condition.

For an implementation of the proposed state protocol, define durable signed-input recording, publication acceptance/quorum policy, idempotent retry, permanent rejection, AUTH/CLOSED handling, and pending/unresolved outcomes. Do not infer transaction success from a websocket enqueue, timeout, or local cache mutation. This is a source-confirmed protocol boundary gap, not a live failure reproduced in this investigation.

#### C5. GRASP ordering directly contradicts the document, and the document is right for current GRASP-01

**Document:** “Blossom mirror plus GRASP push.”

The document explicitly publishes authorization state **before** Smart HTTP push. libhanami's helper explicitly performs “Push ... BEFORE state publish,” invokes `git_remote_push`, and publishes `30618` only afterward (`libhanami/src/hanami-grasp.c:609–684`). Nostr context/publication failure after data transfer logs a warning and returns `HANAMI_OK` (`hanami-grasp.c:649–656, 679–683`). Current GRASP-01 requires incoming pushes to match its latest repository-state announcement and describes purgatory for state awaiting Git data. Therefore a new-tip push can be refused before this helper reaches its announcement step. [GRASP-01 core requirements](https://ngit.dev/protocol/grasp/specification/01).

This is a genuine reconciliation item, **not** a reason to rewrite the proposal to match the code. GRASP fetch also leaves signed-state consistency verification TODO (`libhanami/src/hanami-grasp.c:709–718`). No local source evidence establishes end-to-end conforming GRASP publication, native materialization, or multi-GRASP replication.

#### C6. Clone discovery is an actual example of the NIP-34 misuse the document warns against

**Document:** “Protocol mapping,” “Blossom mirror plus GRASP push.”

`hanami_clone` selects the first HTTPS `30617.clone` URL as a Blossom API endpoint (`libhanami/src/hanami.c:309–320`); the older design workflow explicitly calls clone URLs “Blossom endpoints” (`docs/libhanami-design.md:366–370`). A normal `https://host/<npub>/<repo>.git` clone URL is a Git remote, not necessarily a server exposing `/upload` and `/<sha256>`. The document correctly keeps real Git URLs in `clone` and proposes separate native metadata. Reuse that distinction, but specify how an existing libhanami deployment discovers Blossom endpoints/maps without breaking older announcements. Do not call a proposed `native` tag an already-supported feature. [NIP-34 repository announcements](https://github.com/nostr-protocol/nips/blob/master/34.md).

#### C7. Transport registration and comments do not prove object transfer integration

**Document:** “C / libgit2 stack,” “Packfile import and export.”

The custom transport's lazy fetch path does no pack transfer (`libhanami/src/hanami-transport.c:306–356`). Its push callback discards the `git_push *` argument, collects repository refs, and publishes state; the assertion that libgit2 push automatically invokes the remote ODB's write for every object is a **comment**, not an upload loop in that callback (`hanami-transport.c:363–463`). By contrast, the separate `hanami_push_to_blossom` really does enumerate ODB objects (`libhanami/src/hanami.c:479–500`). It enumerates the whole ODB, not only a proven reachable closure or a native changeset delta.

No calls connect GRASP helpers to these transport/ODB/high-level functions in the inspected three source files. These are separate reusable paths, not one proven native→Blossom→Git→GRASP pipeline. Test cold-client object discovery, refspec behavior, signed authorization, and data completeness together before representing this layer as complete.

### D. What libhanami experience validates, and what it warns against

1. **The C adapter boundary is practical.** Concrete ODB/RefDB vtables and repository attachment validate the proposal's use of libgit2 at a Git boundary (`libhanami/src/hanami-odb-backend.c:251–263`; `libhanami/src/hanami.c:154–213`). They do not validate a non-Git changeset engine. The document's unnamed “eventual library” C sketch should explicitly recognize the existing libhanami bridge and describe new native layers separately rather than suggest all plumbing starts from zero.
2. **Server variance is an existing engineering concern, not a theoretical edge case.** The client probes `server`-tag tolerance, multi-`x` authorization viability, and strict hash binding (`libhanami/src/hanami-blossom-client.c:1208–1268`). It separately probes random-body acceptance and PNG-wrapped acceptance, with comments recording media-sniffer and decoder differences (`hanami-blossom-client.c:1271–1354`). Shim selection uses per-server empirical capability data and explicit overrides (`libhanami/src/hanami-blossom-shim.c:339–425`). This supports a concrete requirement for server profiles, typed rejection/unknown states, re-probing, content-type policy, and compatible-provider selection. These source-recorded operational lessons were not re-tested against live servers here.
3. **Do not oversell the shim as transparent native storage.** Probe auth hashes the shim-inclusive bytes (`libhanami/src/hanami-blossom-client.c:1306–1330`). BUD-02 hashes exact uploaded bytes, so a shim changes the stored address; it cannot preserve `SHA256(source bytes) == Blossom address` without a second mapping/encoding layer. [BUD-02 exact-byte rule](https://github.com/hzrd149/blossom/blob/master/buds/02.md). There are no shim-selection calls in the inspected ODB/transport/high-level bridge files, so the existence of shim helpers does not prove that Git pushes automatically adapt. Prefer compatible raw-data providers or explicitly version encoded storage; do not silently mutate the native identity contract.
4. **“Batch” needs precise meaning.** The probe tests one auth event covering multiple hashes with separate PUTs (`libhanami/src/hanami-blossom-client.c:1228–1251`). This is not proof of a standardized bulk-object request or of the high-level push using that path: its callback uploads individually (`libhanami/src/hanami.c:401–444`). The proposal underweights per-object RTT/signing costs and deployed auth differences. Current BUD-11 defines optional server scoping and mandatory hash scoping for uploads; support should be tested rather than assumed from a kind number alone. [BUD-11](https://github.com/hzrd149/blossom/blob/master/buds/11.md).
5. **Integrity verification needs a precise trust boundary.** ODB can verify the returned payload's SHA-256, enabled by default in high-level attachment, but the map supplies object type/address and the read path does not itself recompute the requested Git OID (`libhanami/src/hanami-odb-backend.c:50–92`; `libhanami/src/hanami.c:154–160`). If the map is later downloaded from an untrusted indexer, authenticating just the fetched payload against that same map is insufficient. Define independent OID/type verification or an authenticated mapping commitment. This finding concerns this backend's checks, not a claim that no upper libgit2 layer ever performs additional checks.
6. **Existing policy intentionally differs from the new proposal.** libhanami chooses Git-native semantics, raw payload hashes, local OID lookup, local optimistic refs, and best-effort relay replication; the proposal chooses native JCS identities, native accepted-state authority, rebuildable semantic indexes, and deterministic Git projection. These can coexist through a bridge, but adopting the proposal is a new protocol/engine and a migration, not a straightforward backend enhancement. Conversely, fixing GRASP order and clone discovery is bridge correctness work, not dependent on inventing that native engine.

### E. Prioritized conclusions and acceptance evidence

- **P0 — Correct the claims before implementation:** replace “causal link equals CAS” with an explicit acceptance/finality contract; keep the supplied document's already-correct BUD-10 syntax and correct the contrary Background assertion; distinguish the two Blossom URI schemes, the two hash domains, and native architecture from bridge implementation.
- **P0 — Resolve the source-of-truth contract:** specify historical authorization, durable sequencer behavior/equivocation, checkpoint/recovery rules, and what clients return when predecessors, policies, or storage are missing. Model-check two-writer, restart, partition, delayed-predecessor, equivocation, and key-rotation cases before canonical writes.
- **P1 — Prove the existing bridge independently:** cold second-client clone with no local ODB/map; all Git object types and special tree entries; GRASP state-before-data with observed rejection/purgatory/acceptance; publication failure without false success; explicit Git-versus-Blossom endpoint discovery. A warm local test or URL parser test does not establish those properties.
- **P1 — Freeze the experimental immutable schema:** cross-language golden vectors, raw path/mode semantics, bounded parsers, native-versus-storage encoding policy, imported Git OID/signature preservation, and a defined mapping/bootstrap format. Benchmark eager root construction and small-object transfer before claiming Scion-like efficiency.
- **P2 — Add derived computation only with a trust contract:** source/policy pinning, semantic versions/options, reproducibility vectors, completeness assumptions, and verified or explicitly trusted authority-sensitive results. Keep that index distinct from the bridge's required OID locator map.
- **P2 — Treat privacy, durable storage, and provider compatibility as explicit supported profiles:** do not infer them from signing, hashes, successful uploads, or replaceable hosting. Obtain operational evidence separately from source tests.

**Review method:** three completed narrow read-only probes covered ODB/index, transport/GRASP, and RefDB/Nostr; one failed startup was retried. Load-bearing claims were checked against source. One independent peer review found no concrete corrections. Jev relevance/claim triage used 15 requests, 63,677 input tokens, approximately $0.0027; high-presence results guided targeted reads, and an unclear transport claim was resolved by directly reading the callback. This triage was advisory, not evidence in place of source.

**Verification boundary:** source inspection and current primary-spec comparison only. No code was changed; builds, CTest, live relay/Blossom/GRASP tests, and throughput benchmarks were not run. The findings intentionally preserve uncertainty about runtime behavior outside the inspected call paths. Proposed follow-up implementation work is not claimed complete or automatically authorized by this report.

### F. Peer review addendum (independent critique-of-critique)

The critique's high-severity arguments (A2, A3, A5, A6) hold up. Two genuinely missing document gaps were confirmed by direct search of the source document; two other candidate gaps turned out to be already addressed by it:

1. **CONFIRMED GAP — CI/agent signing authority is undefined.** The document's only related text is "key compromise is serious" in the sequencer trade-off table. It never specifies how short-lived automation contributes: scoped grants, expiration/revocation, delegation, or whether agents sign proposals while only the sequencer key signs acceptance. Sharing the canonical signing key across CI processes recreates the A2 equivocation problem. NIP-05 identification grants no write authority.
2. **CONFIRMED GAP — multi-author 30618 projection publishing is unaddressed.** NIP-01 addressable-event replacement is scoped by `(kind, pubkey, d)`: multiple maintainers of one repository publish into **separate** replaceable-state slots, not one shared coordinate. The document treats `30618` as "the" projection of accepted state without defining a single projection publisher, a designated state signer, or a multi-author resolution rule. (This also independently strengthens finding C3 about libhanami's owner-only state filters.)
3. Already addressed by the document (do not report as gaps): spam/permissionless admission economics ("Spam and permissionless contributions" section: relays/Blossom may impose payment, quotas, WoT; policy determines canonicality) and clock-skew/LWW concerns (explicit causal-links + sequence numbers design; `created_at` demoted to feeds). Residual minor point: the `30618` **Git projection itself** still inherits NIP-01 `created_at`/event-ID LWW across maintainers, so projection staleness must be tolerated or a single publisher defined (see item 2).

### Summary
See final synthesis below.

## Investigation Log

- Phase 1: extracted the 64KB document from a ChatGPT JSON export (message contents were stubs; real text in `widget_state.report_message.content.parts`); identified libhanami scope (~6.8k LOC C); created this report.
- Phase 1.5: explore agents verified Scion/NIP-34/Blossom/GRASP claims and ran libhanami git archaeology (Background above). One web-explore error (BUD-10 `blossom://` allegation) was disproven by direct grep and corrected in A10.
- Phase 2: context_builder seeded selection (all libhanami sources/headers/tests, docs/libhanami-design.md, this report).
- Phase 3: pair investigator appended sections A–E. Orchestrator spot-checks confirmed the four load-bearing source claims: `hanami-grasp.c:609` push-before-state-publish, `hanami-odb-backend.c` raw-payload (header-less) SHA-256 upload hashing, `hanami.c:309–320` clone-URL-as-Blossom-endpoint, `hanami-nostr.c` publish return values ignored / best-effort success.
- Phase 4: oracle peer review validated A2/A3/A5/A6 and raised 4 candidate gaps; direct grep of the source document confirmed 2 and refuted 2 (recorded as section F).

## Re-verification 2026-10-28 (HEAD e2025a154a9e8a998f9a10b746b696a0fe3d5f4a)

The repo changed substantially since 2026-10-13: `libblossom` and `libporthome` were extracted as separate libraries (5dee18cd3, 14ecb3136, b474a527a), BUD-02 auth moved to `nips/bud02` (767f37c72), relay plumbing now runs over the libnostr pool (c9f553785), vault objects and private ref state are sealed (5d0316f01), and the refdb backend gained loopback-relay tests (af0d48f13). Path notes: Blossom client/shim/capability code cited below as `libhanami/src/hanami-blossom-*.c` now lives in `libblossom/src/` (`client.c`, `shim.c`, `capability.c`); `hanami-index.c` is now `hanami-index-sqlite.c`; the ODB backend now calls `blossom_client_address`/`blossom_client_put` instead of a local hash+upload pair.

### Status of the four bridge-correctness beads

| Bead | Issue | Status |
|---|---|---|
| nostrc-fsuei | GRASP push-before-state ordering | **Still present** — `hanami-grasp.c:612` comment "moved BEFORE state publish" unchanged; push still precedes the 30618 publish, and publish failure after successful push still returns HANAMI_OK. |
| nostrc-g2g37 | clone URL reused as Blossom endpoint | **Still present** — `hanami.c:341–349` still takes the first https:// `30617.clone` entry as the Blossom API endpoint. |
| nostrc-3rxpv | Publication success without relay acceptance | **Core fixed** — `hanami_nostr_publish_event` (`hanami-nostr.c:219–258`) now uses `nostr_relay_publish_and_wait`, discriminates auth-required/rejected/timeout, and returns HANAMI_OK only when at least one relay returned OK; loopback protocol tests added (c9f553785). Residuals: the GRASP helper still swallows post-push publish failures (owned by nostrc-fsuei), and refdb still treats relay publish as best-effort — but that is now an *explicit documented policy* ("cache is source of truth", `hanami-refdb-backend.c:328–336`), not a silent bug. |
| nostrc-v2zyh | Cold-clone OID-map bootstrap | **Still present** — `hanami.c:131` and `hanami.c:509` still open `:memory:` indexes; ODB read still returns GIT_ENOTFOUND without a map entry (`hanami-odb-backend.c:66, 73, 131`); no bootstrap/sync path exists. **Worsened** by vault sealing (5d0316f01): vaulted objects are encrypted before upload (`blossom_odb_write`, `hanami-odb-backend.c:153–170`), so the stored Blossom hash is over sealed bytes — the OID map is strictly non-reconstructible client-side for private repos, making a published bootstrap artifact mandatory rather than optional. |

### Effect on the C1/D3 hash-domain findings

The vault work makes finding C1 more pronounced: libhanami now has **three** byte domains — raw Git payloads (public mode), sealed/encrypted payloads (vault mode), and the document's proposed wrapped-object `type + size + payload` hash. The "shim changes the stored address" warning (D3) now generalizes to "any encoding layer changes the stored address"; the vault is a deliberate instance of that, with `hanami-sealed.c` handling private ref state as sealed snapshots (`hanami-refdb-backend.c:307`). The document's BUD-10 identity chain applies cleanly to public mode only; a native architecture with private repos needs the encoding-vs-identity split the vault work has now forced in practice.

## Conceptual Verdict

The document is a well-researched, protocol-literate proposal whose strongest instincts — separating content hashes from signed assertions from branch authority, keeping bulk data off relays, refusing to overload NIP-34's Git-typed fields, and treating Git as a projection rather than the native model — are sound and correctly grounded in the specs it cites. Its factual base is accurate (verified against the Scion post, NIP-34 draft, BUD-01/02/04/10/11, GRASP-01).

Its central weaknesses, by severity:
1. **Branch-state concurrency is under-specified** (A2). A causally chained signed event log detects conflicts but does not implement compare-and-swap; "equivalent to a signed compare-and-swap" overclaims. Sequencer/threshold options are trade-off tables, not protocols (equivocation, recovery, rotation, finality rule).
2. **Nostr is not an archive** (A3). Signed acceptance chains, historical policy, and checkpoint/recovery rules are required; two Blossom blob copies do not preserve them.
3. **"New NIP needed" is the critical path, not the last step** (A9). The entire native layer (changeset kind, state-transition kind, index announcements, `native` tags) is unstandardized.
4. Derived indexes are attributable, not provably correct (A5); Git interop is read-heavy/asymmetric for writes (A6); eager manifest rewriting borrows Scion's cheap-write rhetoric without its service topology (A7); privacy is a product constraint, not a caveat (A8).
5. Two blind spots (F): CI/agent signing authority, and multi-author `30618` projection publishing.

## libhanami Divergence Verdict

libhanami is **not** an implementation of this architecture and does not claim to be: it is a Git/libgit2 bridge with Blossom as object store and NIP-34 as ref/announcement layer. Of the document's nine architectural layers, libhanami implements blob storage (Git-object flavor), NIP-34 30617/30618 primitives, a custom `blossom://` Git transport, GRASP client helpers, and C library entry points; it lacks the entire native core (changeset descriptors, causal state log, recursive SHA-256 manifests, derived indexes, native materializer). An architectural difference, not a deficiency of libhanami's narrower mission.

The comparison surfaced four bridge-correctness issues in libhanami that the document's guidance predicts or directly contradicts: push-before-state ordering in the GRASP helper (`hanami-grasp.c:609`, vs GRASP-01's state-before-data requirement), NIP-34 `clone` URLs reused as Blossom API endpoints (`hanami.c:309–320`), best-effort publication reported as success (`hanami-nostr.c:212–235`, `hanami-refdb-backend.c:288–307`), and the non-disposable OID map blocking cold clones (`hanami.c:135–140`). Raw-payload hashing also differs from the document's wrapped-object hash domain (C1) — deliberate but migration-relevant.

## Recommendations
1. Before any native implementation: rewrite the branch-state section as an explicit acceptance/finality protocol (equivocation, fork recovery, sequencer restart/rotation, tentative/finalized/conflicted/unavailable outcomes) instead of a CAS equivalence claim.
2. Specify the two blind spots: scoped, revocable agent signing authority; and a single `30618` projection publisher (or multi-author resolution rule) per repository.
3. Freeze the experimental native schema (JCS, golden cross-language vectors, bounded parsers, import/export mapping) before interoperability claims; treat the extension NIP as the project's critical path.
4. For libhanami, treat as P1 bridge-correctness work independent of the native proposal: GRASP state-before-data ordering, Git-clone vs Blossom-API endpoint discovery separation, publication success semantics, and cold-clone OID-map bootstrap.
5. Keep the document's discipline of not overloading NIP-34 Git fields; apply the same discipline to libhanami's `clone` tag handling.

## Preventive Measures
- Future design docs: distinguish "inferable from protocol properties" from "specified"; the CAS overclaim arose from that blur.
- Record hash-domain decisions (raw payload vs wrapped object) as a versioned contract, not implicit convention.
- When proposing new NIP kinds, make a frozen spec + test vectors a first-milestone deliverable, not the last.
- libhanami: add a cold-client (empty ODB + empty index) integration test and a GRASP rejection/purgatory test to prevent regression of findings C2/C5.
