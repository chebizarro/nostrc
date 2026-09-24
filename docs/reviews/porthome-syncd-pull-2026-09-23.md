# Portable-home syncd pull path — v1 punts

**Bead:** `nostrc-p6qp` (Portable-home Phase 3 — Item 2). Parent epic
`nostrc-h10m` — E-portable-home. Design: `docs/designs/home-from-relay.md`
§6.3 (pull + merge policy) and §6.4 (interlocks).
**Author:** Claude Opus 4.7 (Agent SDK), 2026-09-23.

The I2 landing carries the three-way reconciler, the pull glue that
turns a live kind-30078 EVENT into a reconcile pass, the
`.conflict-<device>-<ISO8601>` naming, the `.conflict-deleted` marker,
the desktop notification (via `libnotify` when linked or `notify-send`
as a shell fallback), and the `NOSTR_HOME_STATE=partial` interlock file
that trips the push path (I1 §6.4) until the base is trustworthy.

Everything below is a KNOWN-BAD behaviour in v1, explicitly per
design §6.3. Each entry lists a concrete reproducer sketch so a Phase
5 owner can pick it up.

## 1. Same-file concurrent edits → `.conflict` file, no content merge

**What we do:** last-writer-wins by `mtime` (remote-wins on tie), the
loser is preserved under `<name>.conflict-<device>-<ISO8601>`, a
notification lists conflict paths. Nothing is silently discarded.

**What we do NOT do:** three-way content merge (e.g. `diff3` on text
files, hunk-level merge). If Alice and Bob both edit `notes.md`
between two generations of the pointer, the reconciler will preserve
both files but the user must resolve the merge by hand.

**Reproducer:**
1. Machines A and B start at the same snapshot with
   `notes.md = "hello\n"`.
2. A edits `notes.md` to `"hello\nfrom-A\n"` and pushes gen=N+1.
3. B (offline for the push, then online again) edits `notes.md` to
   `"hello\nfrom-B\n"`.
4. B pulls A's gen=N+1 EVENT. Reconciler classifies as `changed-both`.
5. Expected v1 outcome: `notes.md` on B holds A's version;
   `notes.md.conflict-B-<stamp>` holds B's version.

## 2. Directory rename vs edit inside the directory → copy + conflict

**What we do:** each entry is reconciled independently by
plaintext-rel path. If A renames `docs/` → `notes/` (introducing a new
directory + a modified file inside it) and B edits `docs/x.md` at the
same time, the reconciler will apply A's changes as if `notes/`
introduces new content and `docs/x.md` was locally modified; B's edit
of `docs/x.md` will queue as a local push while A's `notes/` files
materialise. The end state has duplicated content in both paths.

**Reproducer:**
1. Both machines start at snapshot with `docs/x.md = "old"`.
2. A: `git mv docs notes; edit notes/x.md; push gen=N+1`.
3. B (offline): edit `docs/x.md` locally.
4. B pulls A's gen=N+1. Result on B: both `docs/x.md` and `notes/x.md`
   exist; B's `docs/x.md` gets queued for push and will eventually
   land under the old plaintext path back on A.

The reconciler carries no rename detection in v1. Phase 5 will add
identity tracking via a stable per-entry ID (bead
`nostrc-porthome-rename`, not yet filed).

## 3. Non-atomic multi-file application state → mitigated only by ignore list

**What we do:** the reconciler applies remote changes one entry at a
time through openat + rename atomic swaps. A single file transitions
atomically, but a *set* of files does not — if the remote pointer
carries a mid-write browser profile, some files may be applied and
others still pending during the pass.

**What we do NOT do:** provide multi-file transactions.

**Reproducer:**
1. A pushes a mid-write Firefox profile (unlikely in practice — the
   default ignore set includes `~/.mozilla/firefox/*/lock` and
   `*.tmp`).
2. B pulls. B's Firefox sees inconsistent state.

Mitigation is the ignore list plus daemon quiesce on session close
(the debounce forces a flush before the process exits). Phase 5 will
add per-tree write-barrier awareness.

## 4. Clock skew makes `mtime` lie → generation for order, mtime for tie-break

**What we do:** the **generation** counter (bumped monotonically on
every accepted local push, and set to `max(local, remote)` on every
accepted pull) is the sole ordering signal for snapshot progression.
`mtime` is used ONLY inside a `changed-both` conflict for LWW
tie-breaking (with remote-wins on `remote_mtime >= local_mtime` — a
deliberate bias in favour of the network source when clocks agree).

**What we do NOT do:** correct for skewed clocks. If machine A's
system clock is 24 h in the future, its `mtime`s will always beat
machine B's on ties.

**Reproducer:**
1. Machine A: `sudo date -s "+24 hours"; edit foo.txt to A-version;
   push`.
2. Machine B (correct clock): edit `foo.txt` at the same wall time to
   B-version.
3. Both pointers have close generations. Whichever pointer arrives
   later at the peer wins the generation race, but if generations
   were equal (e.g. they crossed in flight) A's future-dated mtime
   forces A to win the LWW tie.

Mitigation is generation ordering — generation always advances by ≥1
per accepted push, so mtimes only matter when generations tie, which
is rare in practice. If the fleet standardises on NTP this vanishes.

## 5. Remote-only files whose plaintext name is unknown → dropped in v1

**Not in the design as a punt, but real:** the manifest carries
encrypted-name entries (design §2.3). Machine B, seeing an entry in
the remote manifest whose `path_enc` matches no local plaintext path,
has no way to recover the plaintext (name encryption is HMAC — one-way).

**What we do:** dirs and symlinks with unmatched `path_enc` are
materialised under the opaque encrypted name (matches how
`nostr-home-fetch` writes into the staging tree). Regular files with
unmatched `path_enc` are SKIPPED in v1 — they'll surface again once
the same plaintext path is created locally and hits the reconciler
in a subsequent pull.

**Reproducer:**
1. Two machines share a home. A has never pulled B's `secrets.env`.
2. B pushes gen=N+1 including `secrets.env` (path_enc = HMAC form).
3. A pulls B's gen=N+1. A's reconciler cannot map `path_enc` to
   plaintext, so `secrets.env` is not materialised.
4. A creates `secrets.env` locally under the same name (or fetches
   it via another channel), pushes gen=N+2. Subsequent pulls carry
   the same `path_enc`, but now A can bidirectionally map it.

Phase 5 will introduce a plaintext-name-in-manifest option gated on
homeless-relay privacy (the trade-off is that a hostile relay learns
filenames — currently prohibited by design §2.3).

## Test coverage summary

`gnome/nostr-homed/tests/unit/test_syncd_reconcile.c` exercises:
- remote-only change → applied; snapshot advanced; no notification
- local-only change → I1 batcher receives the change (queued for push)
- changed-both file → LWW-by-mtime; loser preserved; notification fires
- remote-delete of local-modified → local preserved + `.conflict-deleted`
- unknown base → additive rescan + partial-state marker + push interlock

`gnome/nostr-homed/tests/integration/test_syncd_pull_e2e.c` exercises:
- pull ctx accepts a first EVENT with new generation
- pull ctx no-ops on same/earlier generation
- pull ctx applies successive higher generations
- d-tag mismatch is filtered

Both tests are labelled `nostr-homed;porthome;syncd;portable` and
gated by `NOSTR_HOMED_ENABLE_SYNCD_EXPERIMENTAL`. Live long-lived REQ
lifecycle (EOSE/CLOSED/AUTH + backoff reconnect) is exercised only in
the operator's acceptance test against `wss://relay.sharegap.net`;
`nostr-simple-pool` handles reconnection internally, so no polling
call is emitted from I2 code (verified by reading
`nh_syncd_pull.c` — the callback path is pure decode+reconcile).
