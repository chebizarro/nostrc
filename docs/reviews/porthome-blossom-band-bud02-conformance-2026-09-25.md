# blossom.band BUD-02 conformance note — `x`-tag binding not enforced when `server` tag omitted

Bead: `nostrc-5uij` (P4). Documentation-only. Evidence base: `docs/reviews/porthome-blossom-batch-auth-2026-09-24.md` §2, §2.1, §7 (rounds 1 + 3). Not blocking porthome; not affecting the D8 verdict.

## 1. Observation

Two independent, easily reproducible behaviors observed on `https://blossom.band` on 2026-09-24, ephemeral test pubkey `403c237d81b0cebc0aa850b4420062896c374ee83115c8e3574de25fe4089ae6` (yo44 round 3):

### 1.1 `server` tag rejected in every URL form

`yo44` round 2 exercised the four `server`-tag URL variants against `blossom.band`. In every case where the auth event carried a `server` tag, the response was **401 Unauthorized** with body `"Blossom Authorization event server tags do not include this server"` (yo44 §7 round 2):

```
variant "trailing slash"   server_tag="https://blossom.band/"   → 401 148 ms
variant "no server tag"    server_tag=None                       → 415 1 737 ms  (auth PASS, content policy)
variant "host only"        server_tag="blossom.band"             → 415 372 ms    (auth PASS, content policy)
variant "exact URL"        server_tag="https://blossom.band"     → 401 145 ms
```

That is — blossom.band accepts the auth event only when the `server` tag is completely absent, OR present as the bare host `"blossom.band"`. The three URL-shaped variants (exact URL, trailing-slash, no scheme) all fail. This is spec-conformant: BUD-02 makes the `server` tag optional and does not mandate URL-normalization, so a strict server-tag matcher is within spec. (yo44 §2.1 already characterizes this as "spec-conformant but portable-hostile"; the recommendation to omit the `server` tag by default lives in yo44 §5. Not the substance of this note.)

### 1.2 `x`-tag binding not enforced when `server` tag omitted — **the substance of this note**

With the `server` tag omitted (i.e. after clearing the round-1 hurdle), yo44 round 3 mismatched the auth event's `x` tag against the actual PUT body's sha256:

- auth event carries `x = H1` only, one tag
- PUT body has sha256 = `H2`, `H2 ≠ H1`

Expected per BUD-02: **401** with body identifying the `x`-tag mismatch — analogous to `blossom.primal.net`'s `401 "invalid x tag"` under the identical scenario (yo44 §7 round 3, primal block, mismatch entries).

Actual on blossom.band (yo44 §7 round 3, band block, mismatch entries):

```
=== https://blossom.band  (server_tag omitted) ===
  # C. mismatch (auth x=[H1], PUT blob H2)
  PUT /upload  415  323 ms  body="File type not allowed, unsupported. …"   ← auth PASS on mismatched blob (permissive)
  PUT /upload  415  329 ms  body=<same as above>
  SUMMARY: … C=[415,415]  batch auth ACCEPTED, mismatch PERMISSIVE
```

The 415 body is the content-policy reject (`"File type not allowed…"`). A 415 arriving in this scenario means the server ran the `x`-tag binding check, did **not** find a mismatch worth surfacing, moved on to content policy, and rejected on the random-noise body. Compare `blossom.primal.net` in the same code slot (yo44 §7 round 3, primal block):

```
=== https://blossom.primal.net ===
  # C. mismatch
  PUT /upload  401  564 ms  body="invalid x tag"
  PUT /upload  401  553 ms  body="invalid x tag"
  SUMMARY: … C=[401,401]  batch auth ACCEPTED, mismatch STRICT
```

Same probe code, identical mismatch shape, primal.net returns `401 "invalid x tag"`, band returns `415 <content-policy>`. That's the conformance gap.

Note: t44z's body-size probe (2026-09-25) does not exercise mismatches, only correct-x-tag uploads at increasing sizes, so t44z data is silent on this specific behavior — the evidence is entirely yo44's.

## 2. Why this is a mild BUD-02 conformance gap

BUD-02 (`hzrd149/blossom/buds/02.md`) specifies that the `x` tag list on a kind-24242 authorization event names the blobs the event authorizes, and that the server MUST validate `sha256(request-body) ∈ event.x_tags` before performing the requested operation. Concretely, the spec's normative sequence is:

1. Parse the `Authorization: Nostr <b64>` header, decode the event.
2. Verify the event signature, kind = 24242, `t` tag, `expiration` freshness, and (if present) that any `server` tag matches this server.
3. Compute `sha256(request-body)` and verify it appears in the event's `x` tags.
4. **Then** apply any additional policy checks (content-type, quota, rate limit, …).

Steps 1-3 are auth; step 4 is policy. Auth failures are 401. Policy failures are 4xx of the appropriate flavor (413/415/etc.).

Step 3 gates step 4. If step 3 fails, the correct response is 401 (with a body identifying the mismatch); the server should *not* reach step 4, because the caller has not yet demonstrated authorization over the body being POSTed. `blossom.primal.net` implements this correctly (returns `401 "invalid x tag"`). `blossom.band` reaches step 4 despite step-3 mismatch: the 415 tells us step 4 ran and rejected on content, which is only reachable in the correct impl if step 3 passed.

The gap is *mild* because in practice it does not enable any attack: BUD-02 does not require the server to bind the auth to the specific body it applies to, only to accept an event that *claims* to authorize *some* body with that hash. An adversary who intercepts and swaps the body would produce a different body-hash which, if `x` isn't enforced, is nevertheless "authorized" — but the server stores blobs by *content*-hash under BUD-01 anyway, so the swapped body ends up at a different address than the original event claimed. No integrity is lost; the attacker just uploaded their own blob under their signature. The one measurable consequence is diagnostic: a well-meaning client can no longer *distinguish* "my auth was rejected" from "my content was rejected" on this server. That distinction is what makes it useful, but not urgent, to file.

## 3. Impact on porthome

**Correctness: none.** Porthome's `x` tags are always computed from the exact bytes it is about to PUT (see the DESIGN §? "convergent encryption" section — the caller computes `sha256(ciphertext_chunk)` and uses it as both the `x` tag and the Blossom blob address). Mismatch never happens in production. The `x`-tag binding on blossom.band could be enforced or not without any observable effect on porthome's Phase-2 provisioner.

**Diagnostic clarity: mild.** The capability-probe follow-up (`nostrc-ypn2`) uses an intentional `x`-tag mismatch to characterize `strict_x_binding` per server (yo44 §7's methodology, translated into the probe wrapper). On blossom.band the probe gets 415 whether the server *actually* enforces the binding or not, because the content-policy filter fires first for the random-noise probe blob. That is: the probe cannot cleanly distinguish "band enforces `x` binding but the content-filter fires first" from "band does not enforce `x` binding". Both surface as `HANAMI_CAP_UNKNOWN` (or a "permissive" mislabel) on this specific server.

For porthome this doesn't matter — production `x` tags are always correct — but the probe writer should record it so the mislabel is understood the day someone reads the capability cache and wonders why band is inconclusive.

## 4. Recommendation

**A. Do not amend `libhanami/src/hanami-server-capability.c`.** The file was inspected on 2026-09-25 at HEAD (`ed446dae`). It contains only the `hanami_server_capabilities_init` and `hanami_capability_state_str` helpers and does not itself interpret 415 responses — the 415 interpretation logic will live in whatever capability-probe caller `nostrc-ypn2` writes. There is no line in this file that would benefit from the doc-comment envisioned in 5uij's spec, and inserting one in an unrelated helper would just add reader noise. The zero-code-change outcome is preferred per t44z/5uij spec ("if touching that file feels risky … SKIP the comment addition and just record the recommendation in the review").

**B. Add the comment where 415 is actually interpreted — but do it inside `nostrc-ypn2`.** When the capability-probe caller is written under `nostrc-ypn2` (per-server capability probe at connect time), the `HANAMI_CAP_UNKNOWN` branch on 415-for-mismatch should carry a comment referencing this document:

```c
/* blossom.band's content-policy filter fires before its x-tag check under BUD-02,
 * so an intentional x-tag mismatch probe returns 415 (content-policy) rather than
 * 401 (auth). This means we cannot cleanly infer strict_x_binding for that server
 * — we get 415 whether it enforces the binding or not. That's fine for porthome
 * production correctness because our x-tags are always correct (convergent-hash
 * of the exact bytes we're about to PUT), so mismatch never happens in practice.
 * See docs/reviews/porthome-blossom-band-bud02-conformance-2026-09-25.md §3.
 */
```

This is a note to leave with the `nostrc-ypn2` implementer, not a pre-emptive edit into today's code.

**C. File upstream — mode: internal-only for now.** Per 5uij spec, we have the choice of pasting this note into a blossom.band contact channel (their nostr contact or GitHub) or marking it as internal-only. **Recommended: internal-only for now, because**:
1. Porthome is not yet in a public-facing state and we have not yet had a real-world encounter that this bug hurt.
2. The gap is diagnostic-only and requires no fix on the client side.
3. blossom.band operators run a busy nostr.build backend and any bug we file that doesn't come with a concrete attack scenario or a customer-facing impact statement will (correctly) sit at the bottom of their queue.
4. When we do want to escalate — likely once porthome is in a public beta and a real user hits an ambiguous 415 — we already have the transcripts, the pubkey, and the timestamps captured in yo44's report. Filing later is cheap; filing now would be noise.

If a future maintainer disagrees and does want to file upstream, the contact channel appears to be `https://github.com/nostr-band/blossom.band` (or the nostr.build operators via their nostr `nprofile1…` — check https://nostr.build for the current pubkey). Include:

- Test pubkey: `403c237d81b0cebc0aa850b4420062896c374ee83115c8e3574de25fe4089ae6`
- Timestamps: 2026-09-24 (yo44 rounds 2 + 3, UTC)
- Reproduction: PUT `/upload` with a valid kind-24242 auth event containing `x = H1` and *no* `server` tag, request body with `sha256 = H2 ≠ H1`. Expected 401 `"invalid x tag"`; observed 415 content-policy.
- Reference to BUD-02 §? "Blob upload authorization" (`hzrd149/blossom/buds/02.md`).

## 5. Closeout

- **`nostrc-5uij`**: recommendation recorded — internal-only for now, escalate on customer-facing impact. No hanami code change.
- **`nostrc-ypn2`**: this document is the reference for the 415-interpretation comment when that bead's implementer writes the capability probe.
- No new beads spawned by this note.

---

Report ends.
