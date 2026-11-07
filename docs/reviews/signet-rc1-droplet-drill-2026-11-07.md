# Signet rc.1 disposable Droplet drill (W4) — 2026-11-07

**Beads:** `nostrc-mk7v1.5` (W4), epic `nostrc-mk7v1`, enrollment `nostrc-1cx`; new local test failure `nostrc-onloi`.

**Branch:** `signet/rc1-w4-droplet-drills` (local only; no push or merge).

**Candidate:** `7f2aeaecde6dc271d5a8d2754a708c95177652ba`. The separate task-owned source snapshot is detached at this exact commit; evidence-only commits are not image source.

**Authority:** [plan §10 and §12.4](../plans/signet-production-deployment-2026-11-06.md), [runbook](../../signet/deploy/README.md).

## Boundary and current verdict

**Authentication resolved; awaiting owner decision on Spaces permissions before provisioning.** No live Droplet drill has run. Local runtime build passed; local test-stage build failed. Neither is hosted/native/release acceptance.

The owner authorized environment-variable authentication and explicitly withdrew the named-context requirement. Account access works. The earlier auth failure receipt is retained below as history, not the current blocker.

The intended exercise uses throwaway master/bunker/provisioner/backup/client keys. It is a procedure proof, **not the production key ceremony**. No drill signing or SSH keys have been generated.

### Owner decision: Spaces permission mismatch

The runbook requires a single-bucket PutObject-only, no-delete key on the host. Installed doctl 2.3.0 requires `--grants` for `spaces keys create`. The [current API](https://docs.digitalocean.com/reference/api/reference/spaces-keys/) offers `read`, `readwrite`, and `fullaccess`, not a PutObject-only grant. [Provider access documentation](https://docs.digitalocean.com/products/spaces/how-to/manage-access/) states that scoped write includes delete and full access permits bucket creation/configuration. It also states that limited-access keys and bucket policies cannot be combined for the same bucket.

Requested approval: a temporary full-access workstation-only key for bucket creation/cleanup plus a bucket-scoped read/write/delete host key, both destroyed at cleanup. This is an explicit **disposable-only exception**, not proof of production PutObject-only isolation. No key or bucket has been created while awaiting that decision. No broader credentials were silently installed.

## Executed evidence

### Authentication and preparation

Initial command at `2026-11-07T05:59:43Z`:

```sh
doctl auth init --context signet-drill --access-token "$(cat ~/.config/doctl/token)"
```

Exited 1: `Unable to read DigitalOcean access token: unknown terminal`. The token file existed with mode 0600. Local `auth list` showed only the existing `default` context before and after; nothing was deleted or changed.

After owner-authorized retry, every doctl shell used:

```sh
set +x
export DIGITALOCEAN_ACCESS_TOKEN="$(tr -d '[:space:]' < ~/.config/doctl/token)"
doctl account get --format Status,DropletLimit --no-header
```

Output: `active    25`, exit 0. No token value was printed or committed. No named context was created.

`bd onboard`, `bd prime`, and claim ran; hooks were already installed. `bd dolt pull` failed with `Error 1105: no remote`; no invented synchronization route or unchanged retry. The root AGENTS, plan sections, deployment runbook, listed scripts/units/cloud-init renderer, relevant ops implementation, reference client, and production-roundtrip wrapper were read. RepoPrompt excluded this worktree, so shell access was used without rebinding other roots.

A private temporary workspace `/tmp/signet-drill-20261107.p5S0Ke` contains a venv (PyYAML/jsonschema/boto3), source snapshot, build metadata, and logs. It contains no generated drill credentials. Local artifacts are retained for continuation; cloud resources are not accruing cost.

### Exact candidate runtime image — local build PASS

In the detached candidate snapshot:

```sh
signet/deploy/build-image.sh OFF --load -t signet-drill:candidate \
  --metadata-file "$D/build-candidate-metadata.json" --progress plain \
  > "$D/build-candidate.log" 2>&1
docker image inspect signet-drill:candidate --format '{{json .RepoDigests}} {{.Id}}'
```

Build exit 0. Docker Desktop server 29.8.2; script selects linux/amd64. Actual output:

```text
["signet-drill@sha256:98b163ac2650f1834f53245bb5827b4aa2dbe9e5b195d3d27e47334a7ebb0ad3"] sha256:98b163ac2650f1834f53245bb5827b4aa2dbe9e5b195d3d27e47334a7ebb0ad3
```

This is a local digest, not a published GHCR image or host-preflight proof. No manifest has been bound to a Droplet.

### Candidate test stage — local FAIL, preserved

```sh
signet/deploy/build-image.sh OFF --target test --load -t signet-drill:test \
  --progress plain > "$D/build-tests.log" 2>&1
```

Exit 1; no retry-to-green. Results:

| Route | Actual local result |
|---|---|
| `test_connection_tls_no_resumption` | PASS, 0.51 s |
| SQLite Meson suite | 38 pass, 1 fail, 0 skips; `mgmt_ingress_wire` fails in 12.61 s |
| SQLCipher Meson suite | 38 pass, 1 fail, 0 skips; `mgmt_ingress_wire` fails in 13.66 s |
| SQLCipher `backup_restore` | PASS, 4.31 s; not an off-host backup/restore proof |

SQLCipher failure excerpt:

```text
File "/src/signet/tests/test_mgmt_ingress_wire.py", line 73, in run
  assert result.returncode == 0, (result.returncode, state, result.stdout + result.stderr)
AssertionError: (1, {'requests': 5, 'sid': '3', 'old_sid': '2', 'live': 4,
'connections': 2, 'late_sent': False}, ...)
FAIL ../tests/test_mgmt_replay.c:538:
  w.eoses == expected && g_atomic_int_get(&w.accepted) == expected
```

Filed `nostrc-onloi`; no daemon fix made. Root cause is not established, and a local reconnect test failure is not by itself a deployed-daemon finding. Failed-stage container logs remain in `$D/build-tests.log`; no passing test image is claimed.

## Per-drill status and blocker disposition

All live host drills remain **not executed**, pending the permission decision. This table neither clears other workers' evidence nor reclassifies their existing results.

| # / drill | Disposition in this run |
|---|---|
| §10.B / 10: firewall-first provisioning; absent/wrong volume UUID | Not executed; no Droplet or volume. |
| §10.C / 13: throwaway ceremony, modes, systemd, process hardening, secret-free environment | Not performed; no drill keys. Production ceremony is outside this proof. |
| Blocker 1: health/ready/metrics, loopback listener and off-host denial | No host evidence. |
| §10.D / 7: reference-client pair/sign/deny/restart/revoke, writer-acquire | Not executed. |
| `homed_broker_login_nip46_live` | Not executed: no deployed signer or generated live config; workstation runtime capability not yet assessed. Not claimed unsupported. |
| Container restart, Droplet reboot, 270 s quarantine | Not executed. |
| §10.E / 14: online backup/upload and manifest update | Local SQLCipher backup test passed; live service/upload not executed. |
| §10.F / 8, 9: in-image encrypted restart and fresh-host restore | Unproven; no deployed store, audit/agent verification, or client reconnect. Independent custody review not reassessed. |
| §10.G: upgrade, image rollback, confirmed state rollback | Not executed. |
| 11: ≥15-minute idle CPU observation | Unobserved; no busy-loop finding inferred. |
| §10.H: missing keys/config, wrong key, AUTH rejection if encountered, <270 s refusal | Not executed. |

## Cloud inventory, resource accounting, and cost

No resource-create command has been issued. At `2026-11-07T06:06:42Z`, authenticated commands returned:

```text
doctl compute droplet list --tag-name signet-drill --output json
[]
FIREWALLS []
VOLUMES []
SNAPSHOTS []
SSH_KEYS []
SPACES_KEYS []
TAGS []
```

The latter rows came from `doctl compute firewall list`, `volume list`, `snapshot list`, `ssh-key list`, `doctl spaces keys list`, and `doctl compute tag list`, each with `--output json`. Local Python filters selected tag `signet-drill` or names starting `signet-drill`; SSH/Spaces keys were selected by name, and the tag list by exact name. Spaces output allowed only name/access-key-ID/grants, never secrets.

These are **empty inventory receipts, not deletion receipts**. No Droplets, volumes, snapshots, firewalls, uploaded/local SSH keys, Spaces keys/buckets/objects, or tags were created in this run. Bucket inventory was not queried because no S3 credential has been created; account-wide absence of earlier buckets is not claimed. No named context exists to remove. Existing resources and the owner's token file are preserved.

Provider size query reports `s-1vcpu-2gb`, nyc3 available, `$0.01786/hour` (`$12/month`). Incremental cloud cost so far: **0 hours × $0.01786 = $0.00**, plus zero created volume/bucket usage. No cloud billing reconciliation performed. Local build compute is not included in this estimate.

## Changes, limits, and next step

- Tooling fixes: **none**. No daemon, script, unit, runbook, or version-source edits.
- Report updated in place; no push or merge. Bead carries progress and evidence pointers.
- Version: **no bump**, documentation-only; Signet stays declared 0.8.0.
- No Oracle tooling-fix review: no tooling fix exists to review.
- Two narrow Jev queries selected prior-review formatting and deployment image/preflight/backup implementation ranges; partial implementation excerpts were widened to inspect called checks. No Jev judgment is product verification.
- Await explicit approval or rejection of the disposable Spaces permission exception. After approval, resume the original drills, record each created resource ID immediately, destroy task-owned resources, and replace these pending dispositions with actual evidence.
