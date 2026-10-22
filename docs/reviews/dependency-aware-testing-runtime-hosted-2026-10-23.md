# Finite MDK / provisioned signer / hosted runtime follow-through

Base source: **`0ce430299060a09e4a2db250be1a2cac4df2c212`** (master merge PR18).
Tracker ownership: original MDK `.8`; new finite runtime child
`nostrc-0cfsg.35`, created before fixture code; root `nostrc-0cfsg` retains W11/W12.
`.5` remains **closed**, with measurement updates recorded as comments only.
The finite TCP/native-x86 child `.35` is **closed** at local commit
`dad32d7976e8921093f61099acfb41bacd727b02`; parent owns integration/landing.

Evidence root: `/tmp/nostrc-runtime-hosted-20261023/`. These are source-bound
runtime diagnostics, not selector/profile admission or complete-gate receipts.
**No bump:** only internal test fixtures and evidence documentation change;
production APIs, binaries, defaults, inventory, adapters and workflows are unchanged.

## Actual original MDK08/09 and cache-backed original-source MDK011

Starting from a clean base checkout, the unchanged helper ran:

```sh
XPC_SERVICE_NAME=0 PYTHONDONTWRITEBYTECODE=1 python3 scripts/mdk-gate.py images \
  --source-root "$PWD" --evidence-dir /tmp/nostrc-runtime-hosted-20261023/mdk08-09 \
  --version 08 --version 09 --probe
```

**PASS, exit0, 499.629s**, started `2026-10-22T21:27:13Z`. Helper SHA256
`72ff2a7b6b168c81e6956b74ff2f31f115659315881cf7ceac98efcfc013e3aa`.
Receipt namespace: `mdk08-09/f761df94decd4187b356be45be6e7feb/receipt.json`.
Both original Dockerfile/context snapshots built using a unique explicit
Buildx docker-container builder, retained IID/metadata/inspection and original
private JSON-lines peers, with **network disabled for the peer smokes**:

| Version | Context digest | Built image ID | Peer disposition |
|---|---|---|---|
|08|`b25ae8920c0772aab84bb65ae36106870c4824cdb1c9f80aaa2d1a8e161501a6`|`sha256:40d027e1a52adfc9e036fcaee864c57dd49d28858d30f06a8954971935e35ee5`|hello exit0; recorded CID removed|
|09|`b0044aab92adedb3ac51c13f59e7daecca222eb7b4d3828b6f5fa6d03be11da8`|`sha256:4a11ce179505b9d12445140b43be84238723ffaf6ece0aa008f75ffa5b98962d`|hello exit0; recorded CID removed|

`status=docker_lifecycle_succeeded`, `cleanup_complete=true`, empty cleanup
errors, **`complete=false`**. The exact owned builder registration/state volume
and two immutable peer CIDs were removed; outputs were drained. Immutable images
remain artifacts. No global prune/default builder mutation, prepared011 alias,
MDK interoperability matrix or native arbitrary-descendant custody is claimed.

### MDK011: diagnosed cache/transport distinction, original-source build PASS

The old `0c79eff77` cancellation remains under
`/tmp/nostrc-original-mdk-20261023/exact-0c79eff77/`:1002.755s,exit130,
crate downloads through506.5s, no terminal Cargo error. Its final EOF is induced
by owned-builder shutdown, not proof of upstream rejection. It was not retried
unchanged and is not rewritten as success.

Current bounded diagnosis actually executed:

- `mdk011-fetch-default/`: exact Cargo manifest/lock/toolchain inputs and pinned
  Rust1.97.1 base, source-fetch-only recipe, `timeout 240s cargo fetch --locked
  --verbose`: **PASS**, fetch step190.6s, overall378.722s. Hence no current Cargo
  fetch impossibility is asserted.
- `mdk011-original-repaired/`: external Git comparison exposed actual pinned
  `git fetch`/`index-pack` work (Hax157s,rusqlite235s) and was cancelled904.305s.
  It is retained, not a faster-transport fix or an upstream rejection.
- `mdk011-final/`: restored measured-working default Cargo transport and separate
  fetch/network-disabled frozen build stages. The current cold builder instead
  stalled downloading the pinned Rust base blob
  `sha256:d753b6d908e5f10a3ea2b1a981014435f2ec287c48a3c27fb2e196730783ad6f`
  (250.66MB; only115.34MB at678.8s);902.372s cancellation retained. This is the
  observed external-transfer bottleneck, distinct from dependency fetch. Unique
  correctly removed builders do not share their downloaded base/source caches.

The supported fix is **explicit source-only cache transfer**, implemented by
`scripts/mdk011-cache-build.py`, not a prepared/built peer alias. It accepts only
the exact reviewed fetch-recipe hash
`a7ad9a5bcb317963487a6cdb186a0ac26ade79b18b91477bd8f11d4db3d45afb`
and reviewed original build-prefix hash, current helper bytes, matching Cargo
pins, real Docker daemon/executable and immutable image labels/context. It exports
an explicitly owned **stopped, network-disabled CID** from the fresh successful
fetch image. The filesystem must contain the exact source pins and **no
`/src/target` or installed MDK011 peer**. The export and image configuration are
hashed/retained; the CID is removed and actual absence checked.

A derived original011 context carries that hashed Rust/toolchain/apt/Cargo-source
cache as data, restores the recorded image environment, resets only the driver
workspace, and copies the **unchanged original Cargo files and driver source**.
All fetch/build/test RUNs use `--network=none`; Cargo uses `--frozen`. Original
release builds and all five original tests execute afresh. The final Debian base,
installed binary path, ENTRYPOINT and original peer hello remain unchanged.
No dependency source, feature registry, package lock, toolchain pin, test or
approval policy is patched. This is a disclosed cache-backed original-source
build, **not** a claim that the unchanged cold Dockerfile finished.

The exact successful source-fetch recipe is retained as
`tests/runtime/mdk011-fetch.Dockerfile` (hash above; inherited header comments
are historical, its actual ENTRYPOINT is `/bin/true`). For a future fresh cache,
create a private temporary root with `tests/interop/mdk/driver-0.11/`, copy only
the current three Cargo pin files and this recipe as `Dockerfile`, then run
`mdk-gate.py images --source-root "$temporary_root" --evidence-dir "$fetch_evidence"
--version 011` **without `--probe`**. This produces a bounded source-fetch image,
not a runnable driver. Keep that context, image and successful receipt; the cache
builder below consumes them. This setup is reproducible from committed inputs;
it does not require reconstructing an untracked recipe. The already successful
current fetch was reused, not cold-repeated after diagnosing the transfer gap.

Actual final command:

```sh
XPC_SERVICE_NAME=0 PYTHONDONTWRITEBYTECODE=1 python3 scripts/mdk011-cache-build.py \
  --source-root "$PWD" \
  --fetch-receipt /tmp/nostrc-runtime-hosted-20261023/mdk011-fetch-default/055a7379cae34cca9462256b5c8e56d8/receipt.json \
  --evidence-dir /tmp/nostrc-runtime-hosted-20261023/mdk011-source-cache-final
```

**PASS exit0**, input hashes unchanged, export CID removal verified. New owned
builder run `7c843f85311044c9863f0a5da45b187b`, derived context digest
`635e882e0af60962e039ab0adb12f9d37f866e42a9a65536ed2523dc9f196b18`,
cache archive SHA256
`effaa1904799cc2ba3d04839e1d21e5d6bde4cb840a66a3934801c26320aa546`.
Final image
`sha256:3edd31ecf60c98b5788fa8bf2bef9ecb76a7be15e8b0895f6aaa26737d986705`.
**Five original Cargo tests PASS,zero ignored/filtered; private hello PASS**,
MDK revision946e0547485c9a2c393c2048ec3a968fd50fb441. Actual recorded peer
CID `c4bd8fec86325b4bd0eb43a1a875996aec031ece9a9865121f726249a8c4192e`
removed; builder registration/state volume removed; outputs drained;
`cleanup_complete=true`,empty errors,**`complete=false`**. The final runtime is
newly compiled, not the fetch image's `/bin/true`, an old peer smoke, or prepared011.

`tests/runtime/test_mdk011_cache_contract.py` additionally passes **5/5 real CLI
negatives,zero skips**: incomplete producer, stale helper, changed pins, unknown
producer recipe, and an actual immutable image with wrong run label. All refuse
before creating export/build resources. The first cache-transfer attempt's
case-sensitive Docker absence-diagnostic failure is retained; the next attempt
checks the actual lower-case diagnostic and preserves raw inspection status.
An intermediate positive source-cache build also remains with its own identities.
The helper's separate **11 real Docker cleanup +18 gate +4 detached** regression
cases pass,zero skips. No default builder/global prune/native arbitrary-tree or
selector/profile/whole-MDK-interoperability acceptance follows.

## Real provisioned TCP signer, without an approval bypass

New files:

- `tests/runtime/nip5f_approver.c`: test-only executable approval agent.
- `tests/runtime/test_provisioned_nip5f.py`: explicit-consent Linux-only runner.

The runner requires an actual source-bound root build with
`ENABLE_TCP_IPC=ON`, `NIP55L_SECRET_BACKEND=libsecret`, and explicit test-build
`NIP55L_TEST_TRUST_ENV=ON`. It compiles the approver against GIO, compiles the
existing Grotto schema, and refuses missing consent/build contracts/tools.
It never configures or modifies a developer keychain.

Each scenario owns new0700 HOME/XDG/runtime/config/data/cache directories, a
supervised private D-Bus, and the existing Groundhog throwaway Secret Service
fixture. The **real Grotto daemon**, not a fake signer, generates its0600 random
runtime token and binds only127.0.0.1. The approver stores a public test-vector
identity through **real `StoreKey` → libsecret persistence**, then explicitly
selects it through `Control1.SetDefaultIdentityForLegacy`. No environment-key
lane, mutation override, preseeded allow grant or headless auto-approval is used.

The test-build trust setting names **only the actual compiled approver**; normal
caller executable/inode attestation still runs. `org.nostr.Grotto` ownership
prevents ambient UI activation. A bus roundtrip fences signal subscription before
launching the unmodified `test_nip5f_tcp` executable. On `ApprovalRequested`, the
agent fetches authoritative `GetApprovalInfo` and refuses anything except one
`PENDING`, `NIP5F`, `get_public_key`, **unidentified, unattested,
nonrememberable** request for the exact stored identity. Only then does an
explicit allow/deny invoke `ApproveRequest`, always `remember=false`. Hold mode
never approves. This is executable fixture consent, not an operator-policy or
production-profile change.

Actual Linux/arm64 execution uses the local `nostrc-linux-ci:arm64` environment,
read-only source/root filesystem, private writable evidence and **`--network none`**.
It is not native-x86 or hosted proof. `linux-arm64/fixture-post-review/summary.json`
records executable/cache/fixture/schema hashes, unchanged before/after inputs,
**6 tests / 0 failures / 0 errors / 0 skips**:

1. Original TCP client, real token/handshake/public key, explicit allow: exit0.
2. Explicit denial: original client nonzero, not77.
3. Wrong runtime token: original client exit1 plus daemon auth-rejection evidence;
   a healthy approver's fenced request/decision/pending counters are all zero.
4. Untrusted `gdbus` management caller: real `PermissionDenied`.
5. Cancel an actually pending client without approval, finalize its private
   fixture, then approve a distinct live sibling: sibling client exit0; tokens,
   buses and endpoints remain isolated.
6. Kill the actual owned approver: cleanup **refuses** with the retained unexpected
   exit. This intentionally negative child receipt is not hidden by suite success.

All owned subprocesses are reaped and output drained; private key/token/service
namespace is deleted. Required resources must be alive until cleanup explicitly
terminates them, and graceful daemon shutdown is required. Denial requires exact
client exit1, not a signal/crash. A public test-only Snapshot method fences daemon
pending state and already-delivered signal callbacks before reading counters. Receipts retain
`complete=false`, `complete_gate_eligible=false` and no arbitrary-tree claim.
Missing explicit consent and macOS use both refuse before creating evidence or
resources. Earlier failed harness attempts remain: wrong target name, missing
explicit trust cache option, unknown UID in container passwd, and missing schema
setup. None is retried without its specific setup correction or relabelled green.

### Parent integration requirements (not edited here)

The runner is a dedicated, directly executed runtime suite, deliberately **not**
a concurrent shared-engine/inventory/profile edit. Parent integration must enroll
both new files plus the existing Secret Service fixture/schema and real tool
inputs, and supply the TCP-enabled libsecret test build, actual executable
fingerprints, private bus/service and explicit consent. `nip5f_tcp` capability
must not be inferred from this local raw receipt or from ordinary tool/loopback
setup; no current profile is advertised as capable here. Native-x86 execution of
this **new TCP fixture** has not run; the separate NIP55L cell below has.

## Actual hosted native Linux x86 NIP55L cell

[Run39851489265](https://github.com/chebizarro/nostrc/actions/runs/39851489265),
attempt1, [job120078575663](https://github.com/chebizarro/nostrc/actions/runs/39851489265/job/120078575663),
`ubuntu-24.04 / native`, completed **success**. The actual checkout log and job
API both bind source **`0ce430299060a09e4a2db250be1a2cac4df2c212`**.
GitHub-hosted Ubuntu24.04.5 image20261012.334.1, native GCC/link paths
`x86_64-linux-gnu`, amd64 package and runner labels corroborate actual x86,
not Rosetta or an ARM profile alias.

Artifact **12459041813**, `grotto-ubuntu-24.04-native`, was downloaded through the
artifact API. `build/signer-ctest.xml` records **56 tests /0 failures /0 skips**,
including:

- `nip55l_dbus_contract`: **PASS16.3378s**.
- `nip55l_fake_secret_service_contract`: **PASS0.730736s**.

Raw job API/log, archive and extracted CTest XML/LastTest logs are retained under
the evidence root. Thus the exact two former caller-identity runtime cells are
**PASS at this hosted source**. The historical Rosetta failures remain failures;
no shim is authorized and caller policy is unchanged. This one successful
legacy native job is **not** a successful dependency root, all-profile execution
or W11 broad integration, nor hosted proof of the newly added TCP fixture.

## Source-linked W11 ledger snapshots

The complete earlier API capture window **2026-10-22T21:31:07.535453Z–21:33:25.691930Z**.
Attempt-specific jobs and artifact metadata reviewed for all48 dependency roots:
**45 failure,1 cancelled,2 queued**, no successful terminal dependency gate.
The two new roots are
[PR18 run39851463581](https://github.com/chebizarro/nostrc/actions/runs/39851463581)
at`88d36526af2c913d9d7a93dce6078d7ce5259418` and
[master run39851489448](https://github.com/chebizarro/nostrc/actions/runs/39851489448)
at`0ce430299060a09e4a2db250be1a2cac4df2c212`; both were queued/incomplete
with partial artifacts, no terminal gate/broad-accounting job at this snapshot.
Histories for direct broad/worker/release-validation workflows contain zero roots
(reusable nested jobs are not independent runs). First API capture failed on a
retained HTTP502; the second complete capture is the dated snapshot above.

**0/20 qualifying reviewed distinct PR comparisons,0/2 successful complete broad
integrations; W11 NOT MET / W12 NOT RUN.** No20-PR samples are manufactured,
no partial/nested/retry/local result earns credit, failed/cancelled history stays,
and current queued workloads are not duplicated just to create more attempts.
A later bounded refresh retains the **2026-10-22T22:15:24.573824Z** workflow
listing and fresh attempt-specific jobs/artifact metadata for both nonterminal
roots: **48 roots:45 failure,1 cancelled,1 queued,1 in_progress**. The master root
has58 jobs,PR18 has59; neither has a terminal gate/broad-accounting job. All46
completed historical heads/attempts/conclusions match the complete earlier
capture; their job/artifact bodies are **not claimed re-fetched**. The attempted
full refresh's two TLS handshake timeouts are retained, not hidden.
`hosted-current-bounded-summary.json` binds this precise mixed-age scope and raw
JSON hashes. Counters remain **0/20 and0/2**, W11 NOT MET/W12 NOT RUN.

No protection/schedule/default/publisher cutover occurs before W11 and explicit
operator approval. Sibling `.11`/`.29` remain unresolved; the reported provider
security-filter block is not a diagnosed technical sanitizer/fuzz cause and was
not retried or absorbed. Native-custody scope is unchanged.

## Gates and remaining engine enrollment

One bounded review (`C13B6C`) covered the shown TCP fixture and Dockerfile, finding
and repairing healthy-approver fencing, exact denial status and physical network
isolation. The first failed context delivery is retained. The subsequently added
source-cache bridge was **not independently Oracle-reviewed**; its actual positive
build and five CLI refusal cases are separate measured evidence, not a review
claim. Safe native full default build75.155s and all four static checks passed;
private bus/Secret Service cleanup and read-only keychain guards passed. Final
focused checks and source hashes are recorded in the handoff manifest. No broad
root CTest or selector execution is claimed.

Parent alone owns engine/inventory enrollment. In addition to the TCP inputs
above, enroll `scripts/mdk011-cache-build.py`, its negative test and the original
011 Dockerfile/Cargo/toolchain/driver bytes. A cache invocation needs the actual
successful guarded fetch receipt, its retained exact producer context and immutable
source-only image on the same fingerprinted Docker daemon, the fixed recipe and
build-prefix hashes, Docker/Buildx/BuildKit tools and explicit owned builder/CID
lifecycle. Treat exported cache bytes/configuration and derived Dockerfile as
bound inputs, never as an original cold-context or prepared-profile alias. The
current route does not provide general cache-recipe authority, native arbitrary
tree custody, selector capabilities, complete/release eligibility or cross-arch
proof. Both local MDK and new TCP execution here are Linux/arm64; only the named
hosted NIP55L cases establish native x86. Legacy native arbitrary-tree refusal
remains unchanged. `.8` retains broader integration/sanitizer obligations; this
continuation supplies the finite original-source08/09/011 cells, not whole-bead
or root acceptance. No component version bump.
