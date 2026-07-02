# Signet Passkeys — Phase 0 Feasibility Spike

Executes Phase 0 of [`docs/plans/signet-passkeys-fido-2026-07-02.md`](../../../docs/plans/signet-passkeys-fido-2026-07-02.md):
the go/no-go gate for signet acting as a software FIDO2/WebAuthn authenticator.

**Verdict: GO.** All checks pass, including independent verification by a
separate WebAuthn implementation (python-fido2).

## What it proves

| Plan item | Proven by | Result |
|-----------|-----------|--------|
| 2. ES256 / P-256 backend | `test_fido_crypto.c` | keygen, export/import PKCS#8, sign, verify, tamper-reject |
| 3. COSE_Key + authData + `none` attestation | `test_fido_cbor.c` | byte-correct encoding; self-hosted RP register→assert verifies |
| 4. Storage-fit + portability | `test_passkey_store.c` | N passkeys per agent; PSK-wrap export A→B; unwrap+sign+verify; wrong-PSK rejected |
| 5. External RP acceptance | `emit_artifacts.c` + `verify_external.py` | **python-fido2 independently CBOR-decodes the attestation object and verifies the ES256 assertion** |
| 1. Build gate | `meson.options` / `meson.build` / `CMakeLists.txt` | `signet_enable_passkeys` (Meson) / `SIGNET_ENABLE_PASSKEYS` (CMake), off by default |

## Run

```bash
# C tests (OpenSSL + libsodium + sqlite3 only — no full signet build needed)
bash build_and_run.sh

# Independent WebAuthn cross-check (auto-provisions a venv with python-fido2)
bash run_external_check.sh
```

## Notes / scope

- These are **standalone spike modules**, deliberately built without the full
  signet daemon (which needs the in-tree libnostr/nip46 libraries). The reusable
  modules live at their planned paths: `signet/include/signet/fido_crypto.h`,
  `signet/src/fido_crypto_openssl.c`, `signet/include/signet/fido_cbor.h`,
  `signet/src/fido_cbor.c`.
- The CBOR encoder here is a **minimal deterministic spike encoder**. The
  production build replaces it with libcbor (Phase 0 item 1 / Phase 1).
- The crypto is key-**handle** based (`signet_fido_key`) so a future
  hardware/enclave backend implements the same interface with a non-exportable
  key — the "software now, pluggable hardware later" seam.
- AAGUID used in the spike is a fixed placeholder; a stable signet AAGUID is
  frozen during Phase 2 per the plan.
