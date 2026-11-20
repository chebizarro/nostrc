# fipsd parser fuzzers

`FIPSD_ENABLE_FUZZERS=ON` (defaults to `FIPS_ENABLE_FUZZERS`) builds
`fipsd_fuzz_control` and `fipsd_fuzz_config` with clang/libFuzzer. Use Debug,
`GO_WARNINGS_AS_ERRORS=ON`, `GO_ENABLE_ASAN=ON`, `GO_ENABLE_UBSAN=ON`; never TSan.
The implementation in `fipsd_lib` is coverage-instrumented too. Coverage runtime
link options propagate to ordinary consumers without adding a fuzzer main.

- `control`: production bounded JSON parse/dispatch with no live provider (no
  mutations), then encode/decode equality. Seeds include the actual fipsctl path
  request golden, a query, duplicate keys, deep and incomplete JSON.
- `config`: production in-memory YAML loader, schema normalization, validation,
  diagnostics and destruction. The filesystem loader feeds this same parser.
  Seeds are copied from `tests/config_corpus` and packaging/native-api fixtures.
  It does not start a daemon, read configuration files or create identities.

Run each binary with a **writable copy** of `corpus/<target>` and
`-max_total_time=30 -timeout=5 -artifact_prefix=<scratch>/` under ASan/UBSan.
The four build/run lists in FIPS CI include both targets. Parser rejection is
normal, never suppressed sanitizer failure. No network, sleeps or test-only
parser implementation. The request bound is 4096 bytes / 64 containers; YAML
bounds are 8 MiB input, 64 levels, 100000 expanded nodes and 8 MiB expanded
scalar/key bytes (including terminators), plus at most 4096 diagnostics.
