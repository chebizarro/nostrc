# Critique — Signet Passkeys FIDO2 Feasibility Plan

Reviews `docs/plans/signet-passkeys-fido-2026-07-02.md`. Scope respected: authenticator-only, feasibility-first. Verdict on the plan: sound direction, but **Phase 0 gates the wrong risk** and two hard facts in signet's store contradict the storage model. Prefer fixing those before any "go."

## 1. Top 3 under-specified seams

1. **Storage cardinality collides with the schema.** `store.c:141` declares `CREATE UNIQUE INDEX idx_secrets_pubkey ON secrets(agent_pubkey)` — **one `secrets` row per agent**. The plan (Phase 1.3/1.4) puts each passkey's private key in its own envelope-encrypted `secrets.payload` row, but a passkey vault needs N credentials per agent (≥1 per RP). An implementer stalls immediately: either relax/drop that index (touches the nsec invariant) or move the encrypted key blob into the new `passkey_credentials` table — which contradicts the plan's "private material never leaves `secrets.payload`." **The plan never says which table owns the key blob.** This, not crypto, is the first thing that breaks.
2. **Binary CTAP payloads over JSON NIP-46.** CTAP2 is canonical CBOR; `clientDataHash`, `credentialId`, `authData`, and the DER signature are raw bytes. NIP-46 transport is JSON (`nip46_server.c`), and D-Bus needs explicit `ay` byte-array signatures. "JSON payloads mirror the D-Bus API" (Phase 2.7) leaves the encoding (base64url?), size caps, and interaction with the decrypt/replay path (`nip46_server.c:392–500`) unspecified.
3. **DEK model is single/global, not per-agent.** Background says secrets are "envelope-encrypted per agent" (`store_secrets.h:22`), but `store.c` holds one process-wide `dek` derived with domain `"signet-agent-nsec-v1"` (`store.c:42`) and `signet_store_put_secret` encrypts every secret with it (`store_secrets.c`). Passkey keys inherit the nsec blast radius under an nsec-named domain. The plan should state whether passkeys get their own DEK domain and stop claiming per-agent isolation that doesn't exist today.

## 2. Contradictions / missing dependencies

- **"Per-agent" encryption** (Background/Approach) vs **single global DEK** (`store.c`). Correct the security framing.
- **One-secret-per-agent** (`store.c:141`) vs **many-passkeys-per-agent** (whole premise). Unresolved.
- **Ordering slip:** the BE/BS backup-bit and credential-ID-scheme Open Questions are tagged "resolve before Phase 2," but both drive the Phase 1 payload/schema (versioned payload 1.4, `credential_id` table 1.2). They gate Phase 1, not Phase 2.
- **The riskiest unknowns sit *behind* the gate.** Phase 0 proves crypto bytes; the storage collision (#1) and RP acceptance of a headless authenticator (§4) are only hit in Phase 1–3, *after* "go" is declared.

## 3. Over-planning (cut for a feasibility doc)

- **Phase 4 (virtual CTAP-HID)** is fully specced in 5 sub-items yet explicitly out of the feasibility gate. Collapse to one line: "deferred; not assessed."
- **Phase 3** splits an RP-side verification harness *and* external `python-fido2` interop that largely duplicate Phase 0.4. One acceptance harness suffices.
- **Phase 2 detail** (audit event wording, versioned-payload field list, config keys) is implementation, not feasibility. The audit item is well-grounded (`SIGNET_AUDIT_EVENT_*` enum exists, additive) — but that's a reason to *not* belabor it here.
- Net: a feasibility doc needs only Phase 0 + the storage-fit question. Phases 1–4 file:line detail is premature.

## 4. Questions that change order or go/no-go

- **Will a real RP accept UP=1 with UV=0 and `fmt:none` for the intended agent flows?** If not, no amount of byte-correct crypto saves it — this is the true go/no-go and is testable in Phase 0 at ~zero extra cost against a real WebAuthn server, not just a signature verifier.
- **Which agent use cases actually require WebAuthn** rather than the existing secrets vault? If the consuming RP is cooperative/controllable, a non-FIDO credential may suffice and the project is unnecessary.
- **Can `secrets` accept multiple rows per agent without regressing the nsec path?** Determines whether passkeys live in `secrets` at all (see seam #1).

## Focused verdicts requested

- **Is Phase 0 the right gate?** *Partly.* It gates the **cheapest, most-solved** risk — OpenSSL P-256 + libcbor + `python-fido2` fixtures is well-trodden. It defers the two genuinely novel risks (storage cardinality; RP acceptance of a headless authenticator). Fix: add to Phase 0 (a) a ~20-line storage-fit probe against `store.c:141`, and (b) an **end-to-end RP registration+assertion** (UP=1/UV=0/`fmt:none`), not just "verifier accepts the signature."
- **Is the headless UP/UV stance coherent?** *No — it addresses UV but not UP.* Risk #2 default-denies `userVerification:"required"`, which is right. But **every** usable passkey ceremony must set the User Presence (UP) flag; RPs reject UP=0. For a headless daemon that means UP is a **fabricated assertion in 100% of ceremonies**, not an edge case. The plan must own this explicitly ("this authenticator asserts presence it cannot witness, by design") or the premise fails. Framing UV as *the* semantic problem understates it.
- **Does signet's architecture contradict the plan?** Yes, in the store: the single-row-per-agent unique index and the global (not per-agent) DEK. The D-Bus/NIP-46 dispatch and policy-engine seams (`nip46_server.c:455/500`, `policy_engine.c:41`) are accurately described and fit fine.
