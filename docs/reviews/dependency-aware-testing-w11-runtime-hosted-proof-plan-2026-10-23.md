# W11 runtime/AppArmor/MDK: smallest hosted proof and expected physical records

## Context

For B to execute once the exact tip is integrated and enrolled. Source: code tip `a1c110de0` (frozen handoff
`11e2f0044` plus the teardown follow-up). Nothing here has run yet. Every attempt (failed, cancelled or
refused) is retained by run ID and attempt. No rerun-to-green, no promotion of partial results, no
`complete`/acceptance inference. Local verification on exact `11e2f0044` (macOS, no Docker): 103 non-Docker cases,
96 pass, 7 explicit skips (6 opt-in real Docker, 1 Linux-only `/proc`), plus the follow-up harness 12/12.
**No Linux namespace proof is possible on this host**: it is macOS, and its only Linux kernel (the Docker VM) is
unresponsive.

## Findings this plan must settle

1. Hosted AppArmor provisioning really adds, uses and exactly removes one owned profile (`c872f8da2`, `a1c110de0`).
2. The pidns custody helper runs on x86 hosted runners inside that profile, and the signer fixture passes natively
   with real caller attestation (`9c7a82c5f`, `877bc14ba`).
3. Supplementary groups (Docker socket group) survive the user namespace, and a Docker CLI descendant reaches the
   daemon without any chmod.
4. MDK selected lifecycle (after B wiring) produces, uses and cleans exactly what the selected tests require.

## Commands

```sh
LANDED_REF=<branch containing the exact integrated tip>
gh workflow run dependency-tests.yml --ref "$LANDED_REF" -f mode=broad
gh run list --workflow dependency-tests.yml --event workflow_dispatch --branch "$LANDED_REF" --limit 1 \
  --json databaseId,headSha,status,createdAt          # require headSha == integrated tip
gh run view "$RUN_ID" --log > "run-$RUN_ID-attempt-$ATTEMPT.log"
gh run download "$RUN_ID" --dir "run-$RUN_ID-attempt-$ATTEMPT-artifacts"
```

Diagnostic step for finding 3 (add once to a Linux pidns worker, AFTER "Provision per-job namespace-custody
AppArmor profile" and BEFORE teardown; its result is diagnostic, never acceptance):

```sh
set -euo pipefail
setup="$RUNNER_TEMP/dependency/namespace-apparmor/setup.json"
name="$(python3 -I -c 'import json, sys; print(json.load(open(sys.argv[1]))["name"])' "$setup")"
out="$RUNNER_TEMP/dependency/namespace-groups"; mkdir -p "$out"
id > "$out/outer-id.txt"
stat -c '%u %g %a %i %n' /var/run/docker.sock > "$out/socket-before.txt"
python3 -I - "$name" "$out" <<'PY'
import json, os, subprocess, sys, time
from pathlib import Path
name, out = sys.argv[1], Path(sys.argv[2])
marker = 'nostrc-groups-' + os.urandom(16).hex()
ready = out / 'inner-ready'
target = ('import pathlib, subprocess, time; '
          'r = subprocess.run(["docker", "version", "--format", "{{.Server.Version}}"], capture_output=True, text=True); '
          f'pathlib.Path({str(ready)!r}).write_text(json.dumps([r.returncode, r.stdout, r.stderr])); time.sleep(30)  # {marker}')
target = 'import json; ' + target
with (out / 'helper.log').open('wb') as log:
    helper = subprocess.Popen([sys.executable, 'scripts/adapter_namespace_custody.py', '--apparmor-profile', name,
                               sys.executable, '-c', target], stdout=log, stderr=subprocess.STDOUT)
    try:
        deadline = time.monotonic() + 120
        while not ready.exists():
            if helper.poll() is not None or time.monotonic() > deadline:
                raise SystemExit('namespace target never reported (helper exit %s)' % helper.poll())
            time.sleep(0.05)
        pids = []
        for pid in filter(str.isdigit, os.listdir('/proc')):
            try:
                argv = Path('/proc', pid, 'cmdline').read_bytes().split(b'\0')
            except OSError:
                continue
            if len(argv) >= 3 and argv[1] == b'-c' and marker.encode() in argv[2]:
                pids.append(pid)
        status = Path('/proc', pids[0], 'status').read_text().splitlines() if len(pids) == 1 else []
        groups = sorted(int(g) for g in next(r for r in status if r.startswith('Groups:')).split()[1:]) if status else None
        code, stdout, stderr = json.loads(ready.read_text())
        result = dict(target_pids=pids, outer_view_groups=groups, caller_groups=sorted(set(os.getgroups())),
                      label=Path('/proc', pids[0], 'attr/current').read_text().strip('\0\n ') if pids else None,
                      docker_exit=code, server_version=stdout.strip(), docker_stderr=stderr[-2000:])
        (out / 'result.json').write_text(json.dumps(result, indent=2, sort_keys=True))
        ok = (len(pids) == 1 and groups == result['caller_groups'] and code == 0
              and result['label'] == name + ' (unconfined)')
    finally:
        helper.terminate(); helper.wait(timeout=60)
sys.exit(0 if ok else 1)
PY
stat -c '%u %g %a %i %n' /var/run/docker.sock > "$out/socket-after.txt"
cmp "$out/socket-before.txt" "$out/socket-after.txt"
```

## Expected physical records (all from the retained artifacts of the same attempt)

| Job | File | Must show |
|---|---|---|
| `policy_regressions / regressions`, `broad / static / regressions` | `control/apparmor/setup.json` | `kind=namespace_apparmor_setup_v1`, `state=loaded`, `name` matches `nostrc-ns-[0-9a-f]{32}`, `admission.RUNNER_ENVIRONMENT=github-hosted` |
| same | `control/apparmor/profiles-added.txt` / `profiles-removed.txt` | exactly `<name> (unconfined)` / empty |
| same | `apparmor-settings-before.txt` vs `-after.txt` | byte-equal (`restrict_unprivileged_userns=1` still) |
| same | `probe-unconfined.jsonl` | negative control retained: `after-unshare` label `unprivileged_userns (enforce)`, `open-failed` errno 13 |
| same | `probe-profile.jsonl` | `profile-entered` label `<name> (unconfined)`, then three `write-ok` |
| same | `control/container-before.json` | `AppArmorProfile=<name>`, `SecurityOpt` contains `apparmor=<name>` (no `systempaths` entry; empty Masked/ReadonlyPaths) |
| same | `evidence/logs/mapping-probe.log` | both `before-unshare` and `after-unshare` apparmor `<name> (unconfined)` |
| same | `evidence/commands.tsv`, `regression.json` | all 14 commands 0; kernel matrix >=79 tests, 0 skips, incl. `test_supplementary_groups_survive_user_namespace_for_socket_access`, `test_named_apparmor_profile_is_entered_or_refused_before_target` |
| same | `control/apparmor/teardown.json`, `profiles-after.txt` | `status=removed`; `profiles-after.txt` byte-equal `profiles-before.txt`; no `profile-users.txt` |
| `policy / execute` | `dependency-build/namespace-policy/receipt.json` | `status=passed`, `contract.apparmor.name=<setup name>`, `workload_apparmor` both stages `<name> (unconfined)`, `cleanup_complete=true`, `complete=false` |
| `core / execute` (core.linux, pidns) | `evidence/*/0/lane-*/lane.log` | `namespace custody apparmor: <name> (unconfined)` |
| same | `.../signer/signer-runtime.json` | `status=passed`, `contract.instrumentation=none`, `instrumented_executables` x3 `asan=false`, `approval_snapshot=(uint32 1, uint32 1, uint32 0)` |
| same | `.../signer/worker/receipt.json` | `caller_attestation` bus/daemon/approver `same_inode=true`; daemon and approver `exit_code=0`; `sanitizer_reports=[]` |
| same | `dependency/namespace-groups/result.json` (diagnostic step) | `docker_exit=0`, server version present, `outer_view_groups == caller_groups` (includes the Docker socket group), label `<name> (unconfined)`; `socket-before.txt == socket-after.txt` |
| `core_asan`, `grotto_asan` (after B authors signer profiles) | `signer-runtime.json` | `instrumentation=asan_ubsan`, x3 `asan=true`, `contract.sanitizer_environment` equals authored, no `ERROR: LeakSanitizer` |
| `mdk08`, `mdk011` (after B wiring) | `.../mdk-runtime.json` | `status=passed`, `production_complete=true`, `required` matches selected tests, `docker_cleanup_complete=true`, `docker_access.socket_identity == docker_access_after.socket_identity`, `broker_processes=external_daemon_not_owned`, `complete=false` |
| same | adapter `receipt.json` lane `mdk_recovery` | every entry `closed_in_namespace=true`; no `mdk-recover-*.log` |
| every Linux worker | `dependency/namespace-apparmor/teardown.json` | `removed`, or `not_provisioned` for non-namespace lanes; `incomplete` fails the job and is retained |

## Recommendations

Any deviation is a named, retained failure: refused admission (exit 2), non-empty `profiles-removed.txt`,
`incomplete` teardown, non-equal groups, attestation mismatch, or a missing record. None of these is relaxed or
re-run until green. A passing set proves only these runtime seams. W11 counts, the normal contract and
completeness stay with B's validator.
