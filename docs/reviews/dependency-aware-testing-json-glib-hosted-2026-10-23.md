# JSON-GLib remedy: real hosted closure of `.29` — 2026-10-23

Scope is **nostrc-0cfsg.29 only**, not complete broad or W11 acceptance. No product,
workflow or dependency changes in this evidence batch; CI-only remedy already
landed through approved `4a55397bb` / `e17d3843` / `0e3601`. **No component bump.**

## Literal issue contract

The official bead has no separate `acceptance_criteria` field. Its description
requires a verified upstream fix/backport or compatible dependency release for
the JSON-GLib malformed-input leak, **without** suppression, leak-disable,
corpus deletion/filtering, target skipping, shortened fuzz duration or an
unauthorized dependency-source patch. Subsequent comments identify actual hosted
strict-fuzz execution as remaining proof after parent-verified official1.10.8
remedy, input before/after checks and metadata enrollment. This batch supplies
that real hosted proof. It does not invent a permanent recovered-input condition
that the issue never imposed.

The original `leak-62179efc…` input/probe/log remain unavailable/unrecoverable;
**exact-original replay NOT RUN**. The previously independently retained51
inputs are distinct, **not substitutes** for that original. Prior failed runs
39132650260 and39753293484 and their source-linked artifacts remain in the
acceptance ledger. The new success does not erase those failures.

## Actual source and jobs

[Manual strict run39922316626](https://github.com/chebizarro/nostrc/actions/runs/39922316626),
at actual `94647b282b1c26d8eed2a4fc2b7a1bf109479e65`, attempt1, terminal
**SUCCESS**. All five jobs are terminal success, not skips:

| Job | Original log evidence |
|---|---|
|select120310236399|checkout/log `git log -1 --format=%H` prints exact94647b2 at lines123–124|
|JSON120311754884|exact source215–216; official archive checksum2011; built1.10.8/project2019 and version2155; configured Clang18.1.3/Debug/LIBFUZZER2158–2173; JSON dependency1.10.8 at2260/2314; actual object build/link2370–2371; pin verify/probe/ldd2372–2409; strict command2410–2440; **5,892,314 runs/61s**, successful completion3113/3119|
|NIP49120311754975|exact source215–216; authored `-max_total_time=60`2321; **325 runs/62s**, successful completion2376/2382|
|BIP39120311756128|exact source215–216; authored `-max_total_time=60`2311; **41,446 runs/61s**, successful completion4325/4331|
|coverage120322628197|exact source214–215; separately checksum-verified1.10.8 install2278–2422; pin verify2743–2779; actual instrumented corpus replay2795–2826 and emitted coverage tables2850 onward|

The JSON command uses the seeded `build/apps/grotto/tests/fuzz/corpus/json_event`,
not an empty or renamed corpus. It runs `-max_total_time=60`, artifact output,
`-print_final_stats=1`, with `set -euo pipefail` and original tee output; actual
runtime61s is libFuzzer termination behavior, not a duration reduction. The
source-bound fuzz CMake contract applies `-fsanitize=fuzzer,address,undefined`
and frame pointers to compile/link; the actual log shows LIBFUZZER configuration,
build/link and instrumented runtime. The strict verification probe compiles with
ASan, prints six input results and maps the actual loaded library. No leak-disable
or suppression override was introduced. No separate fuzz-binary hash/verbose
compiler database artifact was uploaded; this is source-bound authored sanitizer
execution evidence, not an independent binary-fingerprint attestation.

Official archive pin is JSON-GLib**1.10.8**, SHA256
`55c5c141a564245b8f8fbe7698663c87a45a7333c2a2c56f06f811ab73b212dd`;
the exact-source setup script checks it with `sha256sum -c`. Both JSON and
coverage logs show the archive `OK`, `pkg-config`1.10.8 and the ASan probe loading
`/home/runner/work/_temp/json-glib-1.10.8/lib/libjson-glib-1.0.so.0.1000.8`.
Actual `ldd` for the fuzz executable resolves the same pinned prefix, not system
1.8. Coverage's existing `|| true` replay/report steps remain advisory; their
actual printed reports are retained, not promoted into a new blocking contract.
The three strict timed fuzz jobs are independently successful blocking jobs.

## Original bytes and integrity

[Machine evidence](dependency-aware-testing-json-glib-hosted-2026-10-23.json),
SHA256`fa8d7ec9c9971c94111bf51a7dc28553713dd98118ad662ad6c26cd22eaf4dc8`,
retains exact API/source/job/attempt identities, all original log capture hashes,
line-indexed evidence, original source blob/SHA256 bindings, archive metadata and
every member hash. Raw bytes:
`/tmp/nostrc-w11-e-fuzz-originals-20261023/` (temporary, may expire).

| Original artifact | Bytes | API-verified original ZIP SHA256 |
|---|---:|---|
|12488830715 JSON corpus|25504|`cad4dd4e4014d5d265ea530ef8c688891e5e2e60d3c90add55af1e25cd33157d`|
|12488847881 NIP49 corpus|4389|`f8f1fb5dbd15bc70d41e11af1ed7cd2cf7fc1d01f056276c94c4f91fe9c2f701`|
|12489007569 BIP39 corpus|15677|`4d6082bfd2dfb30a242e2fcf713a6f14ec1794f3659201798a90e84f15c4428c`|

ZIPs match API SHA256/size and CRC/member checks. The job-logs API provides **no
content digest**: those original logs are captured from exact job endpoints and
locally SHA256-hashed for capture integrity, not falsely described as API-checksum
attested. API status, source checkout output and actual commands/results are all
retained rather than relying on the run's green colour.

## Disposition

`.29`'s compatible-dependency strict-fuzz remediation contract is met and the bead
is closed with this evidence. Exact62179 replay remains NOT RUN, not recast as
passing. **Standalone fuzz contributes zero complete broad integrations and zero
reviewed PR comparisons**. Manual broad39922309846 still has actual selected/
policy/native failures and is nonterminal at the separate dated snapshot; A/B own
those defects. W11 remains NOT MET and W12 NOT RUN; no defaults, triggers,
protections, publication or release assets changed.
