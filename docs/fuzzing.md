# Fuzzing Guide

This repository includes lightweight fuzz harnesses for JSON → Event/Envelope parsing, with optional libFuzzer integration and sanitizer builds.

## Build configurations

Recommended Debug build with sanitizers:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DENABLE_FUZZING=ON \
  -DENABLE_FUZZING_RUNTIME=ON \
  -DGO_ENABLE_ASAN=ON \
  -DGO_ENABLE_UBSAN=ON
cmake --build build -j
```

- ENABLE_FUZZING=ON enables building fuzz targets.
- ENABLE_FUZZING_RUNTIME=ON links libFuzzer (`-fsanitize=fuzzer`). Requires Clang with fuzzer runtime.
- If `ENABLE_FUZZING_RUNTIME=OFF`, drivers are built instead and use `fuzz_driver_main.c`.

## Targets

With libFuzzer runtime (recommended):
- `build/tests/fuzz_event_deserialize`
- `build/tests/fuzz_envelope_deserialize`

Without libFuzzer runtime (drivers):
- `build/tests/fuzz_event_driver`
- `build/tests/fuzz_envelope_driver`

## Seeds and invocation

Seed corpora are under `tests/fuzz_seeds/`.

- Events corpus: `tests/fuzz_seeds/events/`
- Filters corpus: `tests/fuzz_seeds/filters/`

Run examples (libFuzzer):

```bash
# Event deserialization (infinite fuzz)
./build/tests/fuzz_event_deserialize tests/fuzz_seeds/events -runs=0

# Envelope (REQ/CLOSED/EVENT/OK/EOSE/etc.) deserialization
./build/tests/fuzz_envelope_deserialize tests/fuzz_seeds/filters -runs=0
```

Run examples (drivers):

```bash
# Process all files in corpus once
./build/tests/fuzz_event_driver tests/fuzz_seeds/events
./build/tests/fuzz_envelope_driver tests/fuzz_seeds/filters
```

## Environment overrides for limits

Security limits are runtime-configurable via environment variables (fallback to sensible defaults). Useful for tightening or widening bounds while fuzzing:

```bash
# JSON/event/envelope max size in bytes (default 262144)
export NOSTR_MAX_EVENT_SIZE_BYTES=131072

# WebSocket ingress caps (used if exercising connection codepaths)
export NOSTR_MAX_FRAME_LEN_BYTES=1048576
export NOSTR_MAX_FRAMES_PER_SEC=50
export NOSTR_MAX_BYTES_PER_SEC=1048576

# Invalid signature ban policy
export NOSTR_INVALID_SIG_WINDOW_SECONDS=60
export NOSTR_INVALID_SIG_THRESHOLD=20
export NOSTR_INVALID_SIG_BAN_SECONDS=300

# Structural limits
export NOSTR_MAX_TAGS_PER_EVENT=100
export NOSTR_MAX_TAG_DEPTH=4
export NOSTR_MAX_IDS_PER_FILTER=500
export NOSTR_MAX_FILTERS_PER_REQ=20
```

These map to the following getters in code:
- `nostr_limit_max_event_size()`
- `nostr_limit_max_frame_len()`, `nostr_limit_max_frames_per_sec()`, `nostr_limit_max_bytes_per_sec()`
- `nostr_limit_invalidsig_window_seconds()`, `nostr_limit_invalidsig_threshold()`, `nostr_limit_invalidsig_ban_seconds()`
- `nostr_limit_max_tags_per_event()`, `nostr_limit_max_tag_depth()`
- `nostr_limit_max_ids_per_filter()`, `nostr_limit_max_filters_per_req()`

## Tips

- Prefer running with ASan/UBSan to expose memory and UB issues quickly.
- Keep `-runs=0` for continuous fuzzing; use `-max_total_time=<secs>` for bounded sessions.
- Add new seeds to `tests/fuzz_seeds/` to quickly expand coverage.
