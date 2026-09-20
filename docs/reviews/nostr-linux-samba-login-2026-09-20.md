# Bounded Critique: Nostr Linux + Samba Login Shipping Plan

**Date:** 2026-09-20 · **Reviewer:** bounded fidelity/consistency review · **Status:** critique only — no plan edits, no implementation, no tracker or git mutations

## 1. Scope and method

**Documents compared**

| Role | Path |
|---|---|
| Plan under review | `docs/plans/nostr-linux-samba-login-2026-09-19.md` (1,655 lines) |
| Preservation baseline | `prompt-exports/oracle-plan-2026-09-19-234200-linux-samba-plan-1fc-bd84.md`, **Generated Plan section only** (lines 224–1,845) |

**What this review covers:** implementation-bearing content dropped or weakened relative to the baseline; internal contradictions; incorrect references; under-specified lifecycle/security seams; unowned dependencies; material decisions stated without a decision record. Both documents were read in full.

**Spot-checks performed** (named seams only, read-only, at working tree state): `gnome/nostr-homed/src/pam/pam_nostr.c`, `src/common/nostr_cache.c`, `src/ctl/nostr-homectl.c`, `src/nss/nss_nostr.c`, `meson.build`, `CMakeLists.txt`, `nostr-homed.pc.in`, `tests/integration/*.sh`, `tests/integration/mock_signer.c`, `libnostr/src/event.c`, `nips/nip46/src/core/nip46_session.c`, `nips/nip46/include/nostr/nip46/nip46_client.h`, `VERSION_MANIFEST.md`, and `git log` for `ecc5948f`. No build, install, or deployment was attempted. Unrelated dirty work untouched.

**Accepted premise:** the baseline's "no passphrases through PAM" requirement was an invented constraint the user never asked for. The plan's §3.4 reversal — normal hidden GDM PAM conversation, private keys stay in the provider, no custom greeter patch — is a **correct and well-propagated** deviation, not a fidelity loss. Findings below are scoped accordingly. The standalone SMB credential bridge is likewise evaluated only as an explicitly *proposed* mechanism, consistent with how both documents label it.

**Evidence-map spot-check result:** the ten `file:line` seams in §1's evidence table resolve correctly (±2 lines). `pam_nip46_challenge` at `pam_nostr.c:58`, `nh_cache_upsert_user` at `nostr_cache.c:118–131` with `ON CONFLICT(uid) DO UPDATE SET npub=…`, `nostr_event_deserialize_signed`/`compute_id`/`validate` at `event.c:489/580/669`, `nostr_nip46_client_connect` at `nip46_session.c:563`, the `caller frees` ownership comment at `nip46_client.h:13–18`, the 128-char `FAKE_SIG` at `mock_signer.c:52–59`, the real-signer→mock fallback at `run_e2e_real_signer_test.sh:39–52`, `nostr-homed.pc.in:8–9` declaring `0.1.0` and linking `-lnostr_homed_common`, and `VERSION_MANIFEST.md` omitting homed. Commit `ecc5948f` exists. The citation layer is sound.

---

## 2. Findings

Ranked by severity. Each is actionable without rewriting the plan.

### F1 — The "curated record" the plan cites no longer exists, and was destroyed without a copy (High)

The baseline task mandated preserving `docs/plans/nostr-linux-samba-login-2026-09-19.md` intact; the export's file manifest records it at **1,092 tokens**. That path now holds the 1,655-line shipping plan (~34k tokens). The file is **untracked** (`git log` on it returns nothing; `git status` shows `??`), so no prior version is recoverable from git, and no copy exists elsewhere under `prompt-exports/` or `docs/`.

The plan still refers to that vanished document as a distinct source:

- §2.7: *"The curated record's Samba conclusions remain applicable:"* — a dangling self-reference; the plan now cites itself as external evidence.
- §1 *Audit boundary* and §1 *Beads reconciliation* present tracker/worktree observations that originated in the destroyed record, now indistinguishable from newly verified facts.

**Action:** decide explicitly whether the curated record is (a) superseded and folded in — then delete the §2.7 "curated record" phrasing and re-attribute those four Samba conclusions as this plan's own findings; or (b) still a separate artifact — then restore it to a new path and re-point the reference. Either way, commit the plan so the next revision is diffable.

### F2 — A live use-after-free was demoted from a prescribed fix to an unowned "separately track" (High)

Baseline §4.1, `nostr-homectl.c`: *"Remove WarmCache account/group provisioning; check failures in remaining affected paths; **move relay cleanup after final use**. Keep roaming CLI semantics separate."*

Plan §4.1 replaces this with: *"…separately track its relay lifetime and false-success defects."*

The defect is real and confirmed in source. `nh_warm_cache()` frees the owned relay array at `src/ctl/nostr-homectl.c:309`:

```c
if (relays_owned){ for (size_t i=0;i<relays_n;i++) free((void*)relays[i]); free((void*)relays); }
```

and then dereferences the same freed pointers at line 314:

```c
char *se_j = NULL; if (nh_fetch_latest_secrets_json(relays, relays_n, author_hex, ns, &se_j) != 0) break;
```

The plan correctly refuses to bundle roaming repair into login delivery — but "separately track" is not satisfied anywhere in the document: §6's execution index has no roaming row, no phase owns it, and §1's beads guidance only derives beads *from the execution index*. The net effect of the rewrite is that a memory-safety bug the baseline scheduled for repair is now scheduled nowhere.

**Action:** add one sentence to §4.1 or §6 naming the follow-up as a required standalone tracker item with the exact file/line, so it survives the plan's own bead-derivation rule. Keeping it out of the login critical path is the right call; dropping it from the document's output set is not.

### F3 — No operation→endpoint authorization matrix; `SubmitUnlock` on `user.sock` is required by §3.4 but unauthorized by §3.3 (High)

§3.3 defines two endpoints (`auth.sock` root-only `0600`; `user.sock` world-connectable `0666` for "Authenticated local users requesting only their own SMB authorization") and a flat operations table. Caller-binding step 4 constrains only *"For PAM requests, require UID 0 and an allowed PAM service."*

§3.4's final bullet then states the post-login SMB client *"sends the unlock input on that same connection"* — i.e. `SubmitUnlock` must be reachable on `user.sock`. Nothing in §3.3 authorizes that, and nothing forbids an unprivileged caller from attempting `BeginLogin`, `OpenLocalSession`, or the administrative operations on the world-connectable socket beyond the general sentence *"Unprivileged clients receive only their own account operations."*

This is the single most security-load-bearing table in the design and it is the one left implicit. The `ui.sock` removal (correct) also removed the only place where per-endpoint operation scoping was previously spelled out.

**Action:** add a three-column table — operation × endpoint × required peer credential — to §3.3, and state the default-deny rule for unlisted pairs. This is a contract freeze item for Phase 0, not an implementation detail.

### F4 — Priority scheme contradicts itself: §1 calls the complete product P1, §6 labels seven items P0 (Medium-High)

§1: *"Prioritize proof verification and collision-safe identity ownership as P0 blockers, with the complete product as a P1 delivery program."*

§6 execution index: pre-login IPC, proof+vault, **external provider**, identity authority, local homes+PAM, **GDM acceptance**, and the release gate are all labelled **P0**. Only the two Samba rows are P1.

Since the user requires both providers and both Samba pathways, "P0 blocker" and "P1 delivery program" are being used for two incompatible purposes — once as *severity of the existing defect*, once as *scheduling priority of the deliverable*. A reader deriving beads from §6 (as §1 instructs) will file seven P0s against a plan whose summary says the product is P1.

**Action:** pick one axis. Suggested: keep §6's labels as the scheduling truth and reword §1 to *"the cache-only authentication and UID-overwrite defects are P0 remediations; the full product ships as the P0/P1 phase program in §6."*

### F5 — Hard gates 5 and 6 are defined but never scheduled before the work they gate (Medium-High)

§3.1 defines six hard gates, each stopping its deliverable on failure. Gates 1–4 map cleanly onto Phases 1–3. Gates 5 (*Samba administration: prove the chosen Samba command/binding behavior and stable SID handling*) and 6 (*Domain route: prove actual PAM dispatch and credential-cache behavior*) map onto nothing.

Phase 0's freeze list and exit evidence do not mention Samba or winbind. Phase 7 lists *"Samba administration feasibility gate"* as a **dependency**, and §6's index lists *"independent server-authority spike"* as a dependency for the standalone SMB row — but neither the gate nor the spike is a row or phase anywhere. §3.13 is emphatic that *"The gate must freeze one inspection mechanism and its exact version contract before implementation proceeds"* — with no phase in which that can happen.

This matters concretely: the `pdbedit`-vs-Samba-Python-passdb-binding choice, and whether stable SID access exists on the pinned package, determine whether `smb_account.c` is a thin wrapper or a fixture-tested output parser. Discovering that inside Phase 7 is exactly the failure §6's critical-path note warns against (*"Do not wait until the desktop is finished to discover incompatible external-signer or Samba assumptions"*).

**Action:** add a Phase 0b (or two rows to §6) that owns gates 5 and 6 and the server-authority spike, with the dependency edges Phase 7/8 already claim.

### F6 — Test-modification list misattributes the false-pass defect; the actual exit-0 skips go unfixed (Medium)

§4.6 (inherited verbatim from the baseline) says:

- `run_mock_signer_test.sh`: *"true skip reporting."*
- `run_systemd_opensession_test.sh`: *"explicitly experimental roaming test, not login acceptance."*

Source says the opposite. `run_mock_signer_test.sh` **already reports skips correctly** — `exit 77` at both the opt-in guard and the missing-binary path. The script that reports missing prerequisites as **success** is `run_systemd_opensession_test.sh`, at three sites:

```
:9-10   systemctl not available; skipping  → exit 0
:55-56  sqlite3 CLI not found; skipping    → exit 0
:61-62  /dev/fuse not present; skipping    → exit 0
```

So §2.7's accurate observation (*"sometimes report missing prerequisites as success"*) is never converted into a fix instruction for the file that actually does it, while a fix is prescribed for a file that doesn't need it.

Related and correct: §4.6's *"Both build systems: register exit 77 as skip where applicable"* is warranted — `gnome/nostr-homed/CMakeLists.txt` calls `add_test()` nine times and never sets `SKIP_RETURN_CODE`, so today every `exit 77` is recorded as a CTest **failure**.

**Action:** swap the two instructions — `run_systemd_opensession_test.sh` gets "replace `exit 0` prerequisite skips with `exit 77`, and label as experimental roaming"; `run_mock_signer_test.sh` needs only the label/skip-registration change.

### F7 — "Expected user pubkey, **distinct** from transport pubkey" over-constrains NIP-46 and may reject the sole named candidate signer (Medium)

§3.9's persistence list (carried unchanged from the baseline) requires *"Expected user pubkey, distinct from transport pubkey"* alongside *"Remote signer transport pubkey."* NIP-46 permits the remote-signer pubkey and the user pubkey to be **the same key**, and that is the common configuration for phone-resident signers — including Amber, which §3.1 now names as the **primary** compatibility candidate.

Read as a hard enrollment invariant, this rejects a conformant signer at pairing time. The intended invariant is almost certainly *"do not assume they are identical — always resolve the user key via `get_public_key_rpc()` and compare against the administrator-selected key"*, which is exactly what pairing steps 3–4 already do. Phase 3's test *"Different signer transport and user pubkeys"* is likewise a *handle-when-different* case, not a requirement that they differ.

**Action:** reword to "the user pubkey is established only by `get_public_key_rpc()` and MUST NOT be assumed equal to, or different from, the remote-signer pubkey." One sentence; prevents a gate failure caused by the plan's own wording.

### F8 — External-signer matrix now has one named candidate and one unnamed fallback (Medium)

Baseline: *"nsec.app bunker flow; Amber release exposing a compatible NIP-46 bunker flow."*
Plan: *"Amber NIP-46 as primary compatibility candidate; independently operated bunker as secondary. Historical nsec.app flow is not a hosted-service availability promise."*

Demoting nsec.app is justified — a hosted web signer is a poor pre-login dependency and its availability is not the project's to promise. But "independently operated bunker" is a *deployment shape*, not a named product, and the plan's own rules turn that into a program risk:

- §3.1: *"At least one named external signer must pass before releasing the required external-signer feature."*
- Phase 6 measurable exit: *"At least 20 observed approvals using **each claimed real external client**."*
- The user requires external-signer login; §3.1 Hard-gate failure *"stops the affected deliverable."*

The result is a required user-facing capability resting on a single named third-party Android app, with no second named candidate to fall back to.

**Action:** either name the secondary bunker implementation concretely in the matrix, or add an explicit decision note in §3.1 accepting single-candidate risk and stating the contingency if Amber's gate fails (e.g. ship the project's own bunker as the reference signer — noting §4.3 currently declines to fill `nip46_bunker_g.c`).

### F9 — Phase 1's passphrase-canary gate is unpassable as written, given §3.4's own concession (Medium)

§3.4 correctly concedes: *"Do not promise memory locking for copies inside GDM itself; document that boundary."*

Phase 1 then asserts as an exit test: *"Passphrase canary appears only in the expected hidden conversation and bounded broker input, **is wiped after use**, and never appears in logs, arguments, environment, PAM_AUTHTOK or persistent files."*

With a normal GDM conversation the canary transits gnome-shell's JS heap and the `gdm-session-worker` address space, neither of which the project can wipe or is entitled to assert about. As written the gate fails for the exact reason §3.4 already accepted and documented, which will either block Phase 1 or, worse, get quietly reinterpreted at test-writing time.

**Action:** scope the assertion to the boundaries the project controls — PAM module memory after every return path, broker and worker memory after wipe, journal/syslog, `argv`/`environ`, core dumps, and on-disk state — and state the greeter's own unwipeable heap as an explicitly excluded, documented residual. This is a wording fix to Phase 1, not a weakening of the requirement.

### F10 — Provider selection over `PAM_PROMPT_ECHO_ON` is a UX/behaviour contract with no grammar (Medium-Low)

§3.4: *"Offer enrolled provider selection with a bounded text prompt (`PAM_PROMPT_ECHO_ON`) only when needed; never silently switch on failure."* §3.7: *"If both are present, the user chooses explicitly."*

Undefined: the accepted token set, the prompt string, whether a default exists when both providers are enrolled, what happens on unparseable input (re-prompt? how many times? does it consume the `PAM_MAXTRIES` budget mapped in §3.12?), and how the 180-second transaction budget is charged while the user reads a visible text field at the greeter. Hard gate 1 requires proving *"explicit provider choice"* against this unspecified behaviour.

**Action:** fix the token grammar and retry policy in Phase 0's PAM-conversation contract freeze — one short block in §3.4 is sufficient.

### F11 — `WAITING_UI` is a stale state name after the §3.4 reversal (Low)

§3.6's state machine still reads `WAITING_UI / PREPARING_PROVIDER`, a name inherited from the deleted attachment-UI design. Under the current design the broker waits on a PAM-relayed conversation, not on a UI client. Cosmetic, but the state machine is a Phase 0 freeze artifact and will be transcribed literally into `auth_transaction.c`.

**Action:** rename to `WAITING_INPUT` (or `WAITING_CONVERSATION`).

### F12 — §2.7 attributes silent PAM omission to CMake only (Low)

§2.7: *"CMake silently omits PAM when unavailable; Meson requires FUSE."* Both halves are true, but Meson also degrades silently: `meson.build:63-70` declares `dependency('libpam', required: false)` and emits `message('libpam not found; skipping pam_nostr module')`. Since §4.5 requires *"Linux-PAM is required; configuration fails if absent"* in **both** build systems, the current phrasing understates the Meson change.

**Action:** note that both build systems currently skip PAM silently.

---

## 3. Changes from the baseline that are correct — preserve them

Recorded so a later revision does not "restore fidelity" by reverting improvements.

| Change | Verdict |
|---|---|
| §3.4 rewritten to standard hidden GDM PAM conversation; `ui.sock`, `AttachUi`, attachment capabilities, and `packaging/gdm/greeter-integration.patch` all removed | **Correct.** The invented "no passphrases through PAM" requirement is gone, and the removal was propagated cleanly through §3.2 (`nostr-auth-ui` → post-login SMB helper), §3.3 (endpoints, operations, receipts), §3.15 (package 4), §4.2 (`packaging/gdm/README.md`), §5.4, Phase 0 freeze list, and Phase 1 tests. Only F3, F9 and F11 remain as residue. |
| Authority path `/var/lib/nostr-auth/authority.db` → `/var/lib/nostr-auth/private/authority.db`, parent `0755` + private subdir `0700` | **Correct — fixes a baseline contradiction.** The baseline put the authority directory at `0700` while also requiring the NSS projection directory at `0755`; as written, unprivileged NSS could not traverse to `nss.db`. The plan resolves it. |
| §2.5 framing: "shipping blockers" → "integration hazards… race/exploit outcomes require the named reproductions before assigning a specific existing-library bug" | **Correct.** Sharper epistemics; the thirteen-item hazard list and all sixteen §3.9 hardening requirements survive verbatim, and §6 still schedules them as P0. |
| New §1 (goal, release decision, audit boundary, evidence map with `file:line`), beads reconciliation, §6 execution index with owners/sizes/dependencies, detailed phase acceptance | **Net addition.** All ten evidence-map citations spot-checked and sound. |
| Homed1 XML and `EVENT_SCHEMA.md` rows narrowed to documentation-only, with wire-contract repair deferred to a versioned roaming change | **Correct scope discipline** — and, unlike F2, the deferral is self-consistent because no defect fix is implied. |
| §4.5 *"Explicitly unchanged … unless a reviewed decision supersedes it"* | **Correct.** Adds the revision escape hatch the baseline's flat "Explicitly unchanged" lacked. |

---

## 4. Recommended dispositions

| # | Finding | Severity | Smallest sufficient fix |
|---|---|---|---|
| F1 | Curated record destroyed; §2.7 self-reference | High | Decide superseded-vs-separate; re-attribute or restore; commit the plan |
| F2 | WarmCache use-after-free unowned | High | Name it as a required standalone tracker item with `nostr-homectl.c:309/314` |
| F3 | No operation×endpoint ACL matrix | High | Add the table + default-deny to §3.3; mark Phase 0 freeze |
| F4 | P0/P1 self-contradiction | Med-High | Reword §1 to defer to §6's labels |
| F5 | Gates 5–6 and server-authority spike unscheduled | Med-High | Add a pre-implementation Samba/domain gate phase |
| F6 | Skip-reporting instructions swapped | Medium | Swap the two §4.6 entries |
| F7 | "distinct from transport pubkey" over-constrains NIP-46 | Medium | Reword to "established only by `get_public_key_rpc()`" |
| F8 | One named external signer, unnamed fallback | Medium | Name the secondary, or record accepted single-candidate risk + contingency |
| F9 | Phase 1 canary gate unpassable | Medium | Scope the assertion; document the greeter-heap residual |
| F10 | Provider-selection prompt has no grammar | Med-Low | Fix token set and retry policy in Phase 0 |
| F11 | `WAITING_UI` stale | Low | Rename |
| F12 | Meson also skips PAM silently | Low | Correct the sentence |

**Overall:** the plan is a faithful and in several places improved rendering of the baseline. The §3.4 reversal was the right call and was propagated with care. The substantive risks are not in the architecture but at its edges: one document destroyed without a copy (F1), one confirmed memory-safety defect that fell out of every work list during a scope-narrowing rewrite (F2), and one security table left implicit after the design that carried it was deleted (F3). F5 is the highest-leverage scheduling fix — both Samba deliverables are user-required and currently depend on gates no phase performs.

*No files outside `docs/reviews/` were created or modified. No beads, git, or hook operations were performed. Unrelated dirty work preserved.*
