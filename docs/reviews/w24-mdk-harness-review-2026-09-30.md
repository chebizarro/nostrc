# Review: W24 slice F, versioned MDK interop harness (nostrc-a5u5)

- Branch: `test/w24-mdk-harness-matrix`, commit `84176e0d` (on `c564ff8e` beads, on master `82a615e4`)
- Reviewer: independent peer review (AGENTS.md), 2026-09-30
- Worktree: `/tmp/rv-w24-mdk-harness` (branch `review/w24-mdk-harness`)

## Verdict: CHANGES-REQUIRED

The expected-failure semantics are sound. I verified them by running the cases, by mutation, and by fail-closed experiments. The pins match upstream exactly, the CI job is safe, the 0.8 harness is untouched, and no secret reached any artifact.

The one change I require is M1. The brief's first acceptance point is that the driver uses MDK v0.11.0 "the way White Noise does: same engine, profile and config". It does not. The driver drops marmot-app's three agent-text-stream features and component 0x8006, so its KeyPackages and leaves differ on the wire from White Noise 0.11's. The README and commit message nonetheless claim parity. The fix is about 20 lines.

Everything else is Low or Nit and can go to follow-ups.

## What I ran

| Step | Result |
| --- | --- |
| `cmake -G Ninja -DBUILD_GROUNDHOG=ON -DBUILD_TESTING=ON -DBUILD_MDK011_INTEROP=ON`, `ninja` (macOS 27, `/tmp/nostrc-macos27-env.sh`) | OK |
| `ctest -R '^groundhog-mdk011-interop' -V --output-junit` | 7/7 as designed, 18.8 s total. Both images' layers were all `CACHED`, so the existing images match the reviewed source byte-for-byte: `-image` 0.86 s, `-image-mdk09` 0.29 s. **control Passed 9.70 s.** `groundhog-invites-mdk` Skipped 1.54 s, `mdk-invites-groundhog` Skipped 1.37 s, `adopted-welcome` Skipped 2.57 s, `mdk09-probe` Skipped 2.46 s, each with an `XFAIL [unsupported]` line |
| Workflow summary script, run locally on that log and JUnit | Control: pass. The four adopted cases: `XFAIL (not green)` with class and reason |
| Same, with `DOCKER_HOST=unix:///nonexistent` | Images Failed, the five cases Not Run (fixture), **ctest rc 8**, summary FAIL / not run |
| Binary with `GH_MDK_DRIVER=/nonexistent/driver` | abort, rc 134 |
| Binary with a missing Docker image | `driver->ended` assertion, rc 134 |
| Binary with the 0.8 image passed off as 0.11 | fatal-critical, rc 133 (pin and profile check) |
| Binary with `GH_MDK_DRIVER` unset | rc 77 (Skipped, as documented; not green) |
| Mutation: `adopted-welcome` expects another refusal reason | **Failed** (abort), not Skipped |
| Mutation: `mdk-invites-groundhog` XPASS path forced | **Failed** (SIGTRAP), not Skipped |
| 0.8 `groundhog-mdk-interop` (`-DBUILD_MDK_INTEROP=ON`), checking the `mdk_driver_start_env` change | Passed, 3.13 s, all 6 subtests `ok` against `575ae29d` |
| `python3 scripts/check-unsequenced-args.py` | OK |
| `scripts/linux-gate.sh --sanitizers` | Not run: no library code changed (test, test header, CMake option, out-of-tree drivers, CI) |

## Verified good

- **Pins.**
  - All six MDK crates are pinned at `rev = 946e0547…`.
  - Every crates.io package in `driver-0.11/Cargo.lock` has the same version and checksum as in MDK v0.11.0's own `Cargo.lock`.
  - Every git source matches too: OpenMLS fork `59e7d3b2`, nostr fork `a9c7a642`, hax `8f9cb576`, rusqlite `5ae7fdf8`.
  - The driver crate is the only extra package.
  - `[patch.crates-io]` is identical to upstream's, the toolchain is 1.97.1 as upstream pins it, and the Dockerfile builds with `--locked`.
  - The probe checks out the same way against `a102b196`: lock identical, Rust 1.90.0, nostr 0.44.2.
- **The control really exercises adopted wire bytes** (from `vectors.jsonl`):
  - MDK KeyPackages are `MLSMessage(KeyPackage)` (`0001 0005`) with profile `Current` and 0x8009 in `app_components`. They carry neither an `encoding` nor a `relays` tag.
  - The Welcome rumor is kind 444, unsigned, with content `MLSMessage(Welcome)` (`0001 0003`) and tags `e` and `relays`.
  - The GroupContext carries components 0x0001/0x8001/0x8003/0x8004/0x800c, and the two members' GroupContexts are compared equal.
  - Groundhog's KeyPackage is `0001 0001` with an `encoding` tag. MDK's own `key_package_metadata` refuses it (`Serialize`, `UnknownValue(30)`).
- **Admission parity.** `admit_key_package` (`main.rs:528-601`) mirrors marmot-app's relay-fetch `key_package_from_borrowed_record` (`crates/marmot-app/src/key_package_records.rs:367-447`). The order of checks and the tag/metadata cross-checks are the same, plus an explicit `Current` check.
- **Expected-failure semantics.**
  - An XFAIL is reached only after every refusal assertion has passed, and `main()` then turns rc 0 into 77.
  - Any assertion, `g_error`, crash or stall (`spin_until` has a bounded `WAIT_SECONDS`) aborts with a non-77 code.
  - An XPASS calls `g_error`.
  - A missing Docker binary fails at configure time (`find_program … REQUIRED`). A daemon or image failure fails the fixture, which makes ctest exit non-zero.
- **Redaction.**
  - The 4 test secrets and `nsec1` appear 0 times in the 25 vector lines and 0 times in the ctest `-V` log.
  - Request lines are never logged, neither by `mdk-peer.h` nor by either driver.
  - `peer_new` returns only the pubkey.
  - The `GroupEvent` Debug strings that reach `results` hold ids, epochs and app payloads only.
- **CI.**
  - `permissions: contents: read`, and no secrets.
  - Triggers are `schedule` and `workflow_dispatch` only, with no `pull_request_target`.
  - No `actions/cache` and no Docker layer cache, so there is no cache-poisoning surface.
  - `persist-credentials: false`.
  - Timeouts: job 90 min, CTest 900 s per case and 3600 s per image.
  - The SHAs match their tags (`git ls-remote`): `actions/checkout` v4.2.2 = `11bd71901bbe…`, `actions/upload-artifact` v4.6.2 = `ea165f8d65b6…`.
- **Relays.** The new code has no non-loopback URL. The `ws://127.0.0.1:1` placeholder publish endpoint is never dialled, because `to` is empty.
- **`mdk_driver_start_env`** is the old body parameterised by the variable name, and `mdk_driver_start()` delegates to it with `GH_MDK_DRIVER`. It is source- and behaviour-compatible, and the 0.8 test passes.
- The VERSION_MANIFEST "no bump" row is present.

## Findings

### M1 (Medium): the 0.11 driver is not configured as marmot-app / White Noise 0.11; its KeyPackages and leaves differ on the wire

`tests/interop/mdk/driver-0.11/src/main.rs:429-456` (`app_feature_registry`, `app_components`); `tests/interop/mdk/README.md:114-118`.

**What differs.** marmot-app v0.11.0 does two things the driver does not:

- `crates/marmot-app/src/lib.rs:6829-6870` registers SelfRemove *and* the three agent-text-stream-QUIC roles (receive/send/fanout) as `Optional` features. Their backing capabilities are private-use extension types: `AGENT_TEXT_STREAM_QUIC_RECEIVE_CAPABILITY = Extension(0xF2D1)`, and so on.
- `lib.rs:6729-6741` lists `AGENT_TEXT_STREAM_QUIC_COMPONENT_ID` (0x8006) among the supported components.

cgka-engine's `leaf_capabilities` (`crates/cgka-engine/src/capabilities.rs:31-50`) advertises the extension of *every* registered feature, Optional or not. A White Noise 0.11 KeyPackage, and every WN leaf in a ratchet tree, therefore carries 0xF2D1–0xF2D3 in its capabilities and in the `mls_extensions` tag, and 0x8006 in its `app_components`. The driver registers SelfRemove only and drops 0x8006, as its own comments admit ("less the agent text stream"). Advertising these needs no QUIC transport.

**Why it matters.** The README ("configured as marmot-app configures it … the app's component set") and the commit message claim parity, and this harness is the release gate's acceptance evidence (nostrc-7gx7).

**Failure scenario.** nostrc-qp24.5.1 lands, and libmarmot's adopted path rejects, or tag-mismatches, unknown private-use leaf capabilities or component 0x8006. `groundhog-invites-mdk` and `adopted-welcome` then flip from XFAIL to pass against this driver. Real White Noise 0.11 KeyPackages and Welcomes still fail, and the gate reports a false green.

**Fix.**

- Register the three QUIC features exactly as marmot-app does (same table, `Optional`), and insert `AGENT_TEXT_STREAM_QUIC_COMPONENT_ID`.
- Assert in the control that the KeyPackage's `mls_extensions` include `0xf2d1..0xf2d3` and its `app_components` include `0x8006`.
- Otherwise, state the difference in the README and treat a pass as provisional.

### L1 (Low): `group_context` dumps confidential component bytes; the redaction guard knows only nsecs

`tests/interop/mdk/driver-0.11/src/main.rs:631-650` and `393-405`; `gnome/groundhog/tests/mls/test_mdk011_interop.c:313`.

**What leaks.** `group_context` hex-dumps every component from 0x8001 to 0x8010. That range includes 0x8002 `GroupBlossomImageV1`, which holds `image_key`, `image_nonce` and `image_upload_key`. MDK itself prints these as `<redacted>` in Debug (`crates/traits/src/app_components/blossom_image.rs:16-30`).

The dump goes to three places:

- the stdout answer, which the test logs (`GroupContext (carol): …`) into the CI log artifact;
- `vectors.jsonl`;
- the `create_group`/`evolve` vectors.

`Vectors::record` drops a line only if it contains a peer's secret-key hex.

**Failure scenario.** This is latent today: no command sets a group image. A later case that sets one (an `update_group_data` with an image, as White Noise does) would put the image key and the Blossom upload private key into the uploaded CI artifact and log.

The README also calls these "public wire objects", but a GroupContext is member-confidential.

**Fix.**

- Redact secret-bearing components by id: emit a length and digest for 0x8002, and for any future key-bearing ids.
- Better still, build vectors from an allowlist of fields.
- Correct the README wording.

### L2 (Low): a `-p` path that matches nothing gives a vacuous Pass

`gnome/groundhog/tests/mls/test_mdk011_interop.c:720-737`; `gnome/groundhog/CMakeLists.txt` (the `foreach` of case names).

I verified this: `-p /groundhog/mdk011-interop/contrl` exits 0 with `1..0`. The names match today.

**Failure scenario.** Someone renames a case path in C, or mistypes it in the CMake list. The must-pass `groundhog-mdk011-interop-control` then reports Passed while running nothing.

**Fix.** Either have `main()` fail (rc 1) when no case ran (a counter bumped at the top of each case), or set `PASS_REGULAR_EXPRESSION` / `FAIL_REGULAR_EXPRESSION "^1\\.\\.0"` on the CTest.

### L3 (Low): the 0.9 probe omits 0.9 marmot-app's feature registry, so its KeyPackage is internally inconsistent

`tests/interop/mdk/driver-0.9-probe/src/main.rs:170-177`.

0.9's marmot-app calls `.feature_registry(app_feature_registry())` (`crates/marmot-app/src/lib.rs:1858`, `@a102b196`), which registers SelfRemove (proposal 0x000a) and the QUIC roles. The probe does not. Its leaf therefore advertises neither, while its event tags (copied from marmot-app, correctly) claim `mls_proposals` 0x000a and `mls_extensions` 0x000a.

**Failure scenario.** This is masked today because MDK 0.11 checks the proof version first. If MDK or libmarmot ever checks tags or capabilities before the proof, the probe's XFAIL turns into a tag-mismatch error (the `mdk09-probe` assertion on "unsupported proof version 1" fails). The probe would then be testing a KeyPackage no real 0.9 client makes.

**Fix.** Copy 0.9's `app_feature_registry()` into the probe.

### L4 (Low): the CI summary labels a crashed-after-XFAIL case as XFAIL

`.github/workflows/marmot-mdk011-interop.yml:85`.

`if num in xfail` is checked before the JUnit `failure` element.

**Failure scenario.** A case prints `XFAIL [...]` and then aborts in `world_down`. The JUnit status is `fail`, but the summary row reads "XFAIL (not green)". The job is still red because the ctest step fails, so this is diagnostic only.

**Fix.** Check `failure` / `status == "fail"` first.

### Nits

- **N1.** `hello` advertises a `messages` command (`main.rs:760`) that `handle()` does not implement; it answers `unsupported: unknown command`. Drop it from the list.
- **N2.** Dead code:
  - `main.rs:1306-1310` is a no-op `match` on `msg.envelope` whose result is discarded.
  - `sha256_hex` is `#[allow(dead_code)]`, and it is the only reason for the `sha2` dependency.
- **N3.** Both Dockerfiles pin `rust:*-slim-bookworm` and `debian:bookworm-slim` by tag, not digest; the 0.8 image does the same. With a read-only token and no secrets, the worst case is a poisoned result. Consider `@sha256:` pins.
- **N4.** The `fetch_key_package` tie-break differs from marmot-app:
  - The driver's `max_by_key((created_at, id))` picks the *larger* id when `created_at` values are equal.
  - marmot-app's `relay_event_id_cmp` (`key_package_records.rs:487`) prefers the smaller one.
  - This rarely matters, but it is an oracle difference.
- **N5.** ctest's own footer reports the four XFAILs under "The following tests passed" and "100% tests passed". This is a ctest quirk with `SKIP_RETURN_CODE`; the "did not run … (Skipped)" list is right. Locally that headline reads as green, so say so in the README. Likewise, the CI job's conclusion is green whenever the control passes; "not green" lives only in the step summary. That is acceptable by design, but worth one sentence.
- **N6.** `vectors.jsonl` is opened in append mode and never truncated, so local runs accumulate lines from earlier runs, with no run id. Truncate it in the image fixture, or stamp a run id.
- **N7.** The `-v <build>/mdk011-interop-artifacts:/artifacts` path is unquoted inside `GH_MDK_DRIVER`, so a build path with spaces breaks `g_shell_parse_argv`.

## Follow-ups to file

M1, plus L1–L4 as separate beads if not fixed in this branch. N1, N2 and N6 can ride along with the M1 fix.
