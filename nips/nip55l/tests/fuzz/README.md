nip55l fuzz harnesses (nostrc-tf3b)
====================================

Structured libFuzzer-compatible harnesses that drive the real input parsers
in `nostr_nip55l_*`. Every harness has the same shape:

- Under Clang with `-fsanitize=fuzzer` (`ENABLE_FUZZING_RUNTIME=ON`),
  `LLVMFuzzerTestOneInput` is called by libFuzzer.
- Under GCC or Clang without the fuzzer runtime, the same object is linked
  with `tests/fuzz_driver_main.c` and replays a corpus directory
  deterministically. That is the shape `ctest` invokes.

Build
-----

    cmake -S . -B build -DENABLE_NIP55L=ON \
                        -DENABLE_FUZZING=ON \
                        -DENABLE_FUZZING_RUNTIME=ON \
                        -DCMAKE_C_COMPILER=clang
    cmake --build build --target fuzz_nip55l_relays \
                                 fuzz_nip55l_sign_event_json \
                                 fuzz_nip55l_nip44_b64 \
                                 fuzz_nip55l_decrypt_zap_event

Corpus replay under ctest (deterministic, no fuzzer runtime needed):

    cmake -S . -B build-nofz -DENABLE_NIP55L=ON \
                             -DENABLE_FUZZING=ON \
                             -DENABLE_FUZZING_RUNTIME=OFF
    cmake --build build-nofz --target fuzz_nip55l_relays_replay \
                                      fuzz_nip55l_sign_event_json_replay \
                                      fuzz_nip55l_nip44_b64_replay \
                                      fuzz_nip55l_decrypt_zap_event_replay
    ctest -R '^nip55l_fuzz_' --output-on-failure

Targets
-------

- `fuzz_nip55l_relays` — `nostr_nip55l_relays_normalize_json`.
  A JSON array of relay URL strings, strict scheme/host normalisation,
  duplicate removal, size caps. The input is the whole document.

- `fuzz_nip55l_sign_event_json` — `nostr_nip55l_sign_event_json`.
  The first byte picks the identity selector (a static hex key, a static
  nsec1..., an empty selector plus `NOSTR_SIGNER_SECKEY_HEX`); the rest is
  the event JSON. Never touches libsecret or the Keychain.

- `fuzz_nip55l_nip44_b64` — `nostr_nip55l_nip44_encrypt_b64` and
  `nostr_nip55l_nip44_decrypt_b64`. The first byte picks encrypt vs
  decrypt; the rest is either raw plaintext (encrypted round-trip) or a
  candidate NIP-44 v2 ciphertext string (parser exercised).

- `fuzz_nip55l_decrypt_zap_event` — `nostr_nip55l_decrypt_zap_event`.
  Whole input is a zap event JSON; the harness supplies a static test key.
