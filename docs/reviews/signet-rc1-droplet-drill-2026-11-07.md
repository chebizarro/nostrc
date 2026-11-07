# Signet rc.1 disposable Droplet drill (W4) — 2026-11-07

**Beads:** `nostrc-mk7v1.5` (W4), epic `nostrc-mk7v1`; enrollment dependency `nostrc-1cx`.

**Branch:** `signet/rc1-w4-droplet-drills` (local only; no push or merge).

**Candidate:** `7f2aeaecde6dc271d5a8d2754a708c95177652ba` (`signet-v0.8.0-rc.1` candidate; no tag published by this run).

**Authority:** [deployment plan §10 and §12.4](../plans/signet-production-deployment-2026-11-06.md).

## Boundary and verdict

**BLOCKED BEFORE PROVISIONING. No live deployment proof.** The required DigitalOcean authentication command exited 1. The owner's explicit instruction was to stop if authentication failed, so no retry, alternative authentication route, cloud mutation, or live drill followed. The error does not establish whether the token is invalid, empty, or rejected by the API; the cause remains undiagnosed.

The intended exercise uses disposable infrastructure in nyc3 and fresh throwaway signing, database, backup, and client keys. It is a procedure proof, **not the production key ceremony**. No drill keys were generated and no ceremony occurred.

This report records only this resumed run. The supplied recovered transcript was empty. The bead's pre-existing notes identified pending live drills but listed no created resources. This run cannot attest to earlier sessions or the account's existing cloud inventory.

## Executed evidence

All commands ran in the assigned worktree. Times below are UTC.

### Local preparation

```text
$ git status --short
[no output]
$ git branch --show-current
signet/rc1-w4-droplet-drills
$ git rev-parse HEAD
7f2aeaecde6dc271d5a8d2754a708c95177652ba
$ scripts/install-hooks.sh
Pre-push static checks already installed in /Users/bizarro/Documents/Projects/nostrc/.beads/hooks/pre-push
$ bd update nostrc-mk7v1.5 --claim
✓ Updated issue: nostrc-mk7v1.5
$ bd dolt pull
Pulling from Dolt remote...
Error: fetch from origin/main: Error 1105: no remote
```

`bd onboard` and `bd prime` ran. The claim succeeded; configured tracker synchronization did not. No remote was invented and the failed sync was not retried. The root AGENTS instructions and plan §10/§12.4 were read. Detailed runbook, tooling, and integration-client review was not completed before the authentication stop; no tooling validation is claimed.

### Credential-file metadata and authentication

The file's existence and permissions were checked without printing its contents:

```python
from pathlib import Path
p = Path.home() / '.config/doctl/token'
print('TOKEN_FILE_EXISTS', p.is_file())
if p.is_file():
    print('TOKEN_FILE_MODE', oct(p.stat().st_mode & 0o777))
```

```text
TOKEN_FILE_EXISTS True
TOKEN_FILE_MODE 0o600
```

Exact authentication command, with shell tracing disabled:

```sh
set +x
umask 077
date -u "+%Y-%m-%dT%H:%M:%SZ"
doctl auth init --context signet-drill --access-token "$(cat ~/.config/doctl/token)"
```

```text
2026-11-07T05:59:43Z
Error: Unable to read DigitalOcean access token: unknown terminal
→ run doctl auth init --help
```

Exit status: **1**. No credential contents were printed or committed. The requested command supplied the credential to doctl via its access-token argument; this report contains only the command template, not the expanded argument.

```text
$ doctl version
doctl version 2.3.0-release
API endpoint: https://api.digitalocean.com/
```

### Post-failure local context check

```text
$ date -u '+%Y-%m-%dT%H:%M:%SZ'
2026-11-07T06:00:13Z
$ doctl --context signet-drill auth list
default
```

The same context listing before authentication showed only `default`. No `signet-drill` context exists to remove; the existing default context was not altered or deleted.

## Per-drill execution status

| Requested drill | Result / missing evidence |
|---|---|
| §10.A/B bundle, firewall first, volume, cloud-init Droplet, absent/wrong UUID refusal | Not run; no cloud resources created, no mount-identity proof. |
| Candidate linux/amd64 image, real digest, manifest/preflight check | Not built or transferred; no image digest evidence. |
| §10.C throwaway key ceremony, modes, systemd start, health/ready/metrics, loopback listeners, off-host denial, process hardening, secret-free env | Not run; no host or drill keys. |
| §10.D public-relay reference client enrollment/sign/deny/reconnect/revoke and writer-acquire | Not run; no deployed signer. |
| `homed_broker_login_nip46_live` | Not run: no deployed signer or generated login configuration. Workstation build/runtime capability was not assessed before the mandatory auth stop; not claimed unsupported. |
| Container restart and Droplet reboot; pairing survives; 270 s quarantine | Not run. |
| §10.E online backup service, Spaces upload, manifest update | Not run; no Spaces key, bucket, backup, or online consistency observation. |
| §10.F fresh-host restore, audit/agent verification, existing/revoked clients | Not run. |
| §10.G upgrade, image rollback, confirmed state rollback | Not run. |
| ≥15-minute idle CPU observation | Not run; no CPU measurement or daemon busy-loop finding. |
| §10.H missing master/config, wrong DB key, relay AUTH rejection if encountered, restart <270 s | Not run. No relay connection was attempted. |

## §12.4 blocker disposition

These are W4 evidence dispositions, not a reassessment of other workers' code or test results. No release blocker is cleared by this run.

| # | Blocker | W4 disposition |
|---|---|---|
| 7 | Launch client enrollment/reconnect | **Unproven:** reference client and homed live test not executed. |
| 8 | Deployed-image encrypted custody/restart/restore | **Unproven:** no image or deployed store exercised. Independent custody review not reassessed. |
| 9 | Trustworthy recovery | **Unproven:** no fresh-host restore. |
| 10 | Missing/wrong volume fail-closed | **Unproven:** mount-identity checks not executed. |
| 11 | Idle core saturation | **Unobserved:** no idle CPU sample; no daemon defect inferred. |
| 13 | Key ceremony | **Not performed:** neither throwaway procedure proof nor production ceremony. |
| 14 | Online backup consistency | **Live leg unproven:** backup/upload/restore not executed; prior concurrent-writer test results not reassessed. |

Blocker 1's additional host/off-host listener evidence is also absent. Restart quarantine evidence for blocker 15 was not produced.

## Resource accounting, cleanup, and cost

| Resource type | Created by this run | Destruction |
|---|---|---|
| Droplet / volume / snapshot / firewall | None; no IDs | Not applicable |
| Uploaded SSH key / local throwaway SSH private key | None | Not applicable |
| Spaces key / bucket / objects | None | Not applicable |
| `signet-drill` tag | Not created | Not applicable |
| `signet-drill` doctl context | Not present after failed init | Confirmed absent by local `auth list` above |

**No cloud destruction receipt is claimed.** Authenticated `compute ... list` and Spaces inventory calls were not run after the mandatory authentication stop. Thus there is no account-wide proof that nothing tagged `signet-drill` exists. The narrower evidence is that this run issued no resource-create commands and generated no throwaway SSH key. Existing resources, contexts, and the owner's token file were preserved.

**Incremental cost for this run: approximately $0.00.** Droplet runtime was 0 hours × hourly price = $0; no billable storage or Spaces resources were created. No current price lookup or account billing reconciliation was performed.

## Tooling changes, verification, and limits

- Deployment tooling fixes: **none**. The authentication failure is not attributed to a repository defect.
- Only this review document is changed. No daemon, runbook, scripts, units, or release inputs changed.
- Version decision: **no bump** for Signet (declared 0.8.0 in `VERSION_MANIFEST.md`) or other components; evidence-only documentation changes no shipped artifact.
- No build, test suite, deployment gate, or Oracle tooling-fix review was run. There was no tooling fix to review.
- RepoPrompt's loaded roots excluded the assigned worktree; shell file access was used without rebinding or editing another checkout.
- A single Jev `find-lines` query located the existing review's title, boundary, and blocker-table format (partial relevance 0.61; 12,658 input tokens; $0.000532). Only the relevant formatting excerpts were read. This is not product verification.
- Owner action: check the credential file locally without sharing its contents, then explicitly authorize a retry, or leave W4 blocked. No retry or alternate credential route is authorized by this report.

W4 remains incomplete. Resume with the remaining runbook/tooling reads and a successful dedicated-context authentication, then execute the original live-drill scope and immediately record every resource ID in the bead.
