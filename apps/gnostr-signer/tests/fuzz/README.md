# Fuzz Testing for gnostr-signer

This directory contains fuzz testing targets for security-critical components
of gnostr-signer. Fuzzing helps find crashes, memory corruption bugs, and
edge cases that unit tests might miss.

**Issue:** nostrc-p7f6

## Fuzz Targets

| Target | Description | Critical Functions |
|--------|-------------|-------------------|
| `fuzz-nip49` | NIP-49 encrypted key backup | `nostr_nip49_encrypt`, `nostr_nip49_decrypt`, `nostr_nip49_payload_deserialize` |
| `fuzz-bip39` | BIP-39 mnemonic parsing | `nostr_bip39_validate`, `nostr_bip39_seed` |
| `fuzz-json-event` | Nostr event JSON parsing | JSON parsing used in bunker_service.c |

## Building Fuzz Targets

### Prerequisites

- **Clang** compiler (required for libFuzzer)
- **CMake** 3.30+
- **libFuzzer** (bundled with Clang)

### Build Commands

```bash
# Configure with fuzz targets enabled
cmake -S . -B build-fuzz -G Ninja \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_BUILD_TYPE=Debug \
    -DBUILD_FUZZ_TARGETS=ON \
    -DFUZZ_ENGINE=LIBFUZZER \
    -DENABLE_NIP19=ON \
    -DENABLE_NIP49=ON

# Build all fuzz targets
cmake --build build-fuzz --target fuzz-targets

# Or build individual targets
cmake --build build-fuzz --target fuzz-nip49
cmake --build build-fuzz --target fuzz-bip39
cmake --build build-fuzz --target fuzz-json-event
```

## Running Fuzz Tests

### Basic Usage

```bash
# Run NIP-49 fuzzer with corpus
./build-fuzz/apps/gnostr-signer/tests/fuzz/fuzz-nip49 \
    build-fuzz/apps/gnostr-signer/tests/fuzz/corpus/nip49

# Run for 60 seconds
./build-fuzz/apps/gnostr-signer/tests/fuzz/fuzz-nip49 \
    build-fuzz/apps/gnostr-signer/tests/fuzz/corpus/nip49 \
    -max_total_time=60

# Run with multiple workers
./build-fuzz/apps/gnostr-signer/tests/fuzz/fuzz-nip49 \
    build-fuzz/apps/gnostr-signer/tests/fuzz/corpus/nip49 \
    -jobs=4 -workers=4
```

### Using the Run Script

After building, a convenience script is generated:

```bash
# Run all fuzz targets for 60 seconds each
FUZZ_TIMEOUT=60 ./build-fuzz/apps/gnostr-signer/tests/fuzz/run-fuzz.sh

# Run with multiple jobs
FUZZ_TIMEOUT=120 FUZZ_JOBS=4 ./build-fuzz/apps/gnostr-signer/tests/fuzz/run-fuzz.sh
```

### Common libFuzzer Options

| Option | Description |
|--------|-------------|
| `-max_total_time=N` | Run for N seconds total |
| `-max_len=N` | Maximum input size in bytes |
| `-jobs=N` | Number of parallel fuzzing jobs |
| `-workers=N` | Number of parallel workers |
| `-artifact_prefix=PATH/` | Directory for crash/timeout artifacts |
| `-print_final_stats=1` | Print statistics at the end |
| `-help=1` | Show all options |

## Corpus Management

Each fuzz target has a corpus directory containing seed inputs:

```
corpus/
├── nip49/           # NIP-49 encryption test cases
│   ├── valid_ncryptsec_1.txt
│   ├── valid_decrypt_mode.bin
│   └── ...
├── bip39/           # BIP-39 mnemonic test cases
│   ├── valid_12_words.txt
│   ├── valid_24_words.txt
│   └── ...
└── json_event/      # Nostr event JSON test cases
    ├── valid_event_1.json
    ├── empty_content.json
    └── ...
```

### Adding Corpus Seeds

To improve coverage, add more seed inputs:

1. Create valid inputs that exercise different code paths
2. Include edge cases (empty strings, max-length inputs, etc.)
3. Include inputs that previously caused issues

```bash
# Add a new seed
echo '{"kind":1,"content":"test"}' > corpus/json_event/new_seed.json

# Minimize the corpus (remove redundant seeds)
./fuzz-json-event -merge=1 corpus/json_event corpus/json_event
```

## AFL++ Support

For AFL++ fuzzing (alternative to libFuzzer):

```bash
# Configure for AFL++
cmake -S . -B build-afl -G Ninja \
    -DBUILD_FUZZ_TARGETS=ON \
    -DFUZZ_ENGINE=AFL \
    -DAFL_CC=/path/to/afl-clang-fast

# Build
cmake --build build-afl --target fuzz-targets

# Run with AFL++
afl-fuzz -i corpus/nip49 -o findings -- ./build-afl/.../fuzz-nip49
```

## CI Integration

Fuzzing runs automatically:

- **On PRs:** When security-critical files are modified (60s per target)
- **Weekly:** Sunday at 3:00 UTC (5 minutes per target)
- **Manual:** Via GitHub Actions workflow dispatch

See `.github/workflows/gnostr-signer-fuzz.yml` for details.

## Interpreting Results

### Crash Files

When a crash is found, libFuzzer saves the input as `crash-<hash>`:

```bash
# Reproduce a crash
./fuzz-nip49 crash-abc123def456

# Get more details with ASAN
ASAN_OPTIONS=symbolize=1 ./fuzz-nip49 crash-abc123def456
```

### Coverage Analysis

To see which code is covered by the corpus:

```bash
# Build with coverage
cmake -S . -B build-cov -DCMAKE_C_FLAGS="-fprofile-instr-generate -fcoverage-mapping" ...

# Run corpus
LLVM_PROFILE_FILE=fuzz.profraw ./build-cov/.../fuzz-nip49 corpus/nip49 -runs=0

# Generate report
llvm-profdata merge -sparse fuzz.profraw -o fuzz.profdata
llvm-cov report ./build-cov/.../fuzz-nip49 -instr-profile=fuzz.profdata
```

## Security Issues Found

Document any issues found by fuzzing here:

| Date | Target | Issue | Status | Fix |
|------|--------|-------|--------|-----|
| - | - | No issues found yet | - | - |

## Best Practices

1. **Run regularly:** Schedule fuzzing in CI for continuous testing
2. **Save the corpus:** Commit interesting inputs that increase coverage
3. **Minimize crashes:** Before reporting, reduce crash input to minimal size
4. **Fix promptly:** Security issues found by fuzzing should be fixed quickly
5. **Add regression tests:** Convert crash inputs into unit tests after fixing

## Resources

- [libFuzzer Documentation](https://llvm.org/docs/LibFuzzer.html)
- [AFL++ Documentation](https://aflplus.plus/)
- [Google OSS-Fuzz](https://google.github.io/oss-fuzz/)
- [NIP-49 Specification](https://github.com/nostr-protocol/nips/blob/master/49.md)
- [BIP-39 Specification](https://github.com/bitcoin/bips/blob/master/bip-0039.mediawiki)
