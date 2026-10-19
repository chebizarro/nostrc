# Dependency-aware testing acceptance run ledger

**Versioned evidence snapshot, not a live dashboard.** Machine-readable normalized data is the JSON fence below; counters are derived from its accepted dispositions. Beads remains the task tracker.

## Count at this snapshot

- **0/20 qualifying PR comparisons**: no PR-triggered shadow run, and no PR-triggered repository run in the queried window.
- **0/2 qualifying broad integrations**: five nested broad accounting jobs failed; one selection failure prevented broad launch.
- **6 shadow attempts**: five failed pushes and one canceled manual run. The canceled parent still retained a failed nested broad accounting job.
- **59 small plan/receipt/aggregate artifacts** were retrieved and checked; installer binaries were not downloaded or executed.

See [criterion audit](dependency-aware-testing-ci-2026-10-18-acceptance-audit.md) for bead decisions and concrete blockers. This replaces assumed counters with source-linked observations, not approval to change policy.

## Backfilled root runs

| Run / attempt | Source | Event | Parent result | PR disposition | Nested broad accounting |
|---|---|---|---|---|---|
| [38833746063/1](https://github.com/chebizarro/nostrc/actions/runs/38833746063) | `5467c02d1` | push | failure | not a PR | [failed](https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116770701220) |
| [38845886339/1](https://github.com/chebizarro/nostrc/actions/runs/38845886339) | `33a101276` | push | failure | not a PR | not launched |
| [38856570439/1](https://github.com/chebizarro/nostrc/actions/runs/38856570439) | `8a031f443` | push | failure | not a PR | [failed](https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116841163905) |
| [38873978927/1](https://github.com/chebizarro/nostrc/actions/runs/38873978927) | `a47807050` | workflow_dispatch | cancelled | not a PR | [failed](https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116900093654) |
| [38878691465/1](https://github.com/chebizarro/nostrc/actions/runs/38878691465) | `249f9657f` | push | failure | not a PR | [failed](https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116915712972) |
| [39132650260/1](https://github.com/chebizarro/nostrc/actions/runs/39132650260) | `2743f1735` | push | failure | not a PR | [failed](https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117700034296) |

The reusable broad/worker/release workflows have zero standalone run records. That does **not** mean broad was never executed: five actual nested broad accounting jobs are captured above. No broad result is counted solely from a successful parent or a subset of jobs.

## Refresh and review protocol

1. Fetch the four workflow histories with pagination, repository context, every candidate attempt’s jobs and artifacts (enumerate1..N when run_attempt=N). Verify returned totals and source identities; record retrieval failures/expired artifacts rather than assuming success.
2. Reconcile new run IDs and attempts with this snapshot. Preserve earlier failures/cancellations when reruns exist; bind each artifact to its attempt/source/plan identity and never reuse an earlier receipt as a later attempt. Deduplicate PR numbers and root integrations, not artifacts.
3. Inspect source-bound plans, actual worker conclusions, normalized receipts and broad accounting. Review each legacy/selective difference; record accepted, failed, blocked, ineligible or unreviewed explicitly. No automatic PR acceptance from status color.
4. Derive counters from explicit accepted entries and check them against the displayed summary. Publish a new timestamped snapshot and update `.5` before reporting W11 progress or closing scope. A documentation push may launch a later run; it is outside the frozen snapshot, not silently counted or a reason for recursive ledger commits.
5. Capture missing evidence, measurements or remedies in beads. Never weaken existing admission, skips, sanitizer, cleanup or release rules to make a counter advance.

Read-only discovery used here (replace IDs/attempts from returned metadata; `gh` supplies existing authentication):

```sh
gh api --paginate --slurp "repos/chebizarro/nostrc/actions/workflows/386168204/runs?per_page=100"
gh api --paginate --slurp "repos/chebizarro/nostrc/actions/workflows/386168202/runs?per_page=100"
gh api --paginate --slurp "repos/chebizarro/nostrc/actions/workflows/386168205/runs?per_page=100"
gh api --paginate --slurp "repos/chebizarro/nostrc/actions/workflows/386168209/runs?per_page=100"
gh api --method GET --paginate --slurp repos/chebizarro/nostrc/actions/runs \
  -f per_page=100 -f "created=>=2026-10-15T00:00:00Z"
gh api --paginate --slurp "repos/chebizarro/nostrc/actions/runs/RUN_ID/attempts/ATTEMPT/jobs?per_page=100"
gh api --paginate --slurp "repos/chebizarro/nostrc/actions/runs/RUN_ID/artifacts?per_page=100"
```

The data is a reviewed backfill, not an implemented CI auto-updater. Refresh is owned by the migration session under `.5`; the owner is not expected to tally runs manually. Raw local files are not a permanent artifact store.

## Canonical data

<!-- acceptance-ledger-json:start -->
```json
{
  "schema_version": 1,
  "audit_source_sha": "2743f17354ac13527d8d8919f81abc711396c959",
  "snapshot_recorded_at": "2026-10-19T03:01:13.808922Z",
  "reconciliation_updated_at": "2026-10-19T03:11:57.005478Z",
  "repository": "chebizarro/nostrc",
  "scope": {
    "workflow_ids": [
      386168204,
      386168202,
      386168205,
      386168209
    ],
    "workflow_history": "All API-visible runs for these workflow IDs, every returned attempt=1. No claim to recover deleted/inaccessible history.",
    "repository_context_since": "2026-10-15T00:00:00Z",
    "repository_context_returned": 146,
    "repository_context_pr_run_count": 0,
    "standalone_reusable_history_is_not_execution_count": true,
    "candidate_keys": "workflow run_id plus run_attempt; nested jobs and duplicate artifacts are not separate root runs.",
    "future_runs": "Snapshot only; later runs, reruns, expirations and policy changes require refresh before acceptance claims.",
    "artifacts": "59 small plan/receipt/aggregate archives downloaded, CRC-checked and member-hashed. No application installers or source archives executed.",
    "raw_capture_directory": "/tmp/nostrc-acceptance-audit-20261018",
    "durability": "Curated metadata/verdicts/hashes below are committed. Raw captures are local and GitHub artifacts expire; do not claim raw bytes remain available after retention. No qualifying W11 entry is inferred from unavailable evidence."
  },
  "qualification_policy": {
    "pr": "Distinct representative PR with source/range-bound legacy and selective evidence, reviewed difference classification and regression for any legacy-only failure. A green run alone is insufficient; pushes/manual runs and local test cases are not PRs.",
    "broad": "Complete successful full broad family accounting and reviewed required-workload/receipt union at exact source; advisory distinctions remain explicit. Root-run deduplication prevents counting nested jobs/artifacts as integrations.",
    "counter_rule": "Counters are derived only from explicit accepted dispositions, not status colors, candidate count, unit test counts or complete-request flags.",
    "cutover": "Unchanged W11 evidence plus operator approval; W12 is not authorized by this audit."
  },
  "counts": {
    "qualifying_pr_comparisons": 0,
    "target_pr_comparisons": 20,
    "qualifying_broad_integrations": 0,
    "target_broad_integrations": 2,
    "shadow_attempts": 6,
    "shadow_failed": 5,
    "shadow_cancelled": 1,
    "broad_accounting_failed": 5,
    "broad_not_launched": 1,
    "artifacts_retrieved": 59
  },
  "local_test_evidence": {
    "source_sha": "2743f17354ac13527d8d8919f81abc711396c959",
    "command": "PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests/selection -v",
    "exit_code": 0,
    "tests": 437,
    "skips": 0,
    "failures": 0,
    "duration_seconds": 151.176,
    "raw_log_sha256": "70431a57f103923e3936ec40aa5ce541ad4225e82069dfc0b14ef6ddfcfee65e",
    "test_result_index_sha256": "8ab3ce4e1dad1fd1ebdc832b0c2b95419ba1e6bdcdf0852309c26527833924c3",
    "module_counts": {
      "test_aggregation": 16,
      "test_bp_metrics_reporting": 7,
      "test_broad_setup_contracts": 11,
      "test_cmake_inventory_compat": 6,
      "test_command_execution": 16,
      "test_command_output_bytes": 5,
      "test_command_signal_bytes": 3,
      "test_contained_custody_contract": 20,
      "test_diagnostic_eligibility": 8,
      "test_diagnostic_nonmembers": 14,
      "test_execution_parallelism": 24,
      "test_failure_receipts": 17,
      "test_git_ranges": 20,
      "test_grotto_profiles": 7,
      "test_local_command_adapter": 8,
      "test_mdk_cleanup": 3,
      "test_mdk_detached_cleanup": 4,
      "test_mdk_gate": 9,
      "test_nonmember_ownership": 7,
      "test_phase_environments": 21,
      "test_prepared_profile_contract": 6,
      "test_production_inventory": 8,
      "test_profile_contracts": 16,
      "test_realization": 16,
      "test_realization_exclusions": 7,
      "test_realization_inputs": 8,
      "test_realization_run": 16,
      "test_realization_source_identity": 9,
      "test_realization_support_inputs": 8,
      "test_realization_toolchains": 7,
      "test_release_plan": 12,
      "test_resource_profile_contracts": 4,
      "test_sanitizer_adapter": 9,
      "test_sanitizer_route_refusal": 2,
      "test_selection_command_contract": 11,
      "test_selector": 20,
      "test_selector_checkout_binding": 5,
      "test_selector_cli": 6,
      "test_shared_sanitizer_execution": 5,
      "test_workflow_contracts": 36
    },
    "source_status_after": "clean; exact source unchanged",
    "not_a_pr_or_broad_integration": true,
    "additional_gates": [
      {
        "name": "test-pre-push",
        "exit_code": 0,
        "log_sha256": "4b245ea79ada5162499e10be384b44a2b07f8dce77dd9a6ff9f31a70a722f80d"
      },
      {
        "name": "test-full-gate",
        "exit_code": 0,
        "log_sha256": "db4a542881d058bc3f1faf7f31c699b73a1786ec28ace942b839d51d6715f313"
      },
      {
        "name": "test-linux-gate-lifecycle",
        "exit_code": 0,
        "log_sha256": "ebad66e7a7b87c734ac1f9097bb661c11de80826049c74f670899a823d1ddc59"
      },
      {
        "name": "test-linux-gate-smoke",
        "exit_code": 0,
        "log_sha256": "03b09043fcd7392847932e90a9d9de0e4cb098be95979bae569ba966e47437da"
      },
      {
        "name": "test-sanitizer-gate",
        "exit_code": 0,
        "log_sha256": "df98c5f59b618bdac5e2a63e95cb5209a27a94ac00847cc18a1d5e2c3582c463"
      },
      {
        "name": "test-test-gate",
        "exit_code": 0,
        "log_sha256": "9677bca9f8c14b5572966291d629d8221c3741e774bc2a4d7f0ce814384b9ba4"
      },
      {
        "name": "check-unsequenced-args",
        "exit_code": 0,
        "log_sha256": "ae5d6a6c404c1c31aa46e4d3dcbe582ed6b857ecf0bce2148e48490070ad84ea"
      },
      {
        "name": "check-gui-test-gdk-frame",
        "exit_code": 0,
        "log_sha256": "55b03052fb1a38d28bb8dc76a3e2fe0b5c3c0f81c9e767fc2323801d3c9fe51a"
      },
      {
        "name": "check-linux-ci-packages",
        "exit_code": 0,
        "log_sha256": "83b776b6d4940082be5adfe8d6c138d546cf9fc49633c40e330d430430a956aa"
      },
      {
        "name": "check-privacy",
        "exit_code": 0,
        "log_sha256": "ca0cdaf0baa46808b995e23cdff13c12cd03fd213c8944bd2a5ca5e4c9b04250"
      },
      {
        "name": "actionlint",
        "exit_code": 0,
        "log_sha256": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
      }
    ],
    "independent_release_identity_probe": {
      "observed_at": "2026-10-19T03:05:50.248704Z",
      "result_sha256": "ec147bf977b2846f857720b635fabdef65328c83be45e33a5549198fad8523c9",
      "result": {
        "source_sha": "2743f17354ac13527d8d8919f81abc711396c959",
        "scope": "local synthetic validation contract only; no publication or hosted acceptance",
        "all_success_package_map": {
          "linux.aarch64": "success",
          "linux.x86_64": "success",
          "source": "success"
        },
        "baseline_publish": true,
        "independent_refusals": [
          {
            "field": "tested_sha",
            "publish": false,
            "passed": false,
            "errors": [
              "missing/mismatched broad exact-tag validation"
            ]
          },
          {
            "field": "tag",
            "publish": false,
            "passed": false,
            "errors": [
              "missing/mismatched broad exact-tag validation"
            ]
          },
          {
            "field": "mode",
            "publish": false,
            "passed": false,
            "errors": [
              "missing/mismatched broad exact-tag validation"
            ]
          },
          {
            "field": "inventory_hash",
            "publish": false,
            "passed": false,
            "errors": [
              "missing/mismatched broad exact-tag validation"
            ]
          },
          {
            "field": "passed",
            "publish": false,
            "passed": false,
            "errors": [
              "missing/mismatched broad exact-tag validation"
            ]
          },
          {
            "field": "partial_profiles",
            "publish": false,
            "passed": false,
            "errors": [
              "missing/mismatched broad exact-tag validation"
            ]
          }
        ]
      }
    }
  },
  "operational_snapshot": {
    "branch": {
      "api_path": "repos/chebizarro/nostrc/branches/master",
      "exit_code": 0,
      "response_sha256": "454e63631500a2f8ac91cb61bbbabfdef600b6efc4f20d80f5be332952fa5e13",
      "observed": {
        "protected": false
      }
    },
    "classic-protection": {
      "api_path": "repos/chebizarro/nostrc/branches/master/protection",
      "exit_code": 1,
      "response_sha256": "3f6832090f9cd73706e68d317cc0c2f6ef7bb4a3b48c31bde797e69d525ecb21",
      "observed": {
        "message": "Branch not protected",
        "status": "404"
      }
    },
    "effective-rules": {
      "api_path": "repos/chebizarro/nostrc/rules/branches/master",
      "exit_code": 0,
      "response_sha256": "8b536427324a6fd78cbd4c401a29577dcdf00f4619b6da8252bc0d0e5ac2f3f7",
      "observed": []
    },
    "rulesets": {
      "api_path": "repos/chebizarro/nostrc/rulesets?includes_parents=true",
      "exit_code": 0,
      "response_sha256": "d0bfad170e956e8a7e10603b52fef6e7d28236e7c64003fae0ee5c01068202cc",
      "observed": []
    },
    "release-mode": {
      "api_path": "repos/chebizarro/nostrc/actions/variables/NOSTRC_DEPENDENCY_RELEASE_MODE",
      "exit_code": 1,
      "response_sha256": "0405f0a57fcfc0469e66af3eac338aa52a9fbee0891339f8605ff931dc6fba2a",
      "observed": {
        "message": "Not Found",
        "status": "404"
      }
    },
    "repository_access": {
      "api_path": "repos/chebizarro/nostrc",
      "exit_code": 0,
      "captured_at": "2026-10-19T02:46:57.874720Z",
      "response_sha256": "99622e046818403a3d581a172c54ac4524d0aa02deb0e0674c2bae926b65060c",
      "observed": {
        "default_branch": "master",
        "private": false,
        "permissions": {
          "admin": true,
          "maintain": true,
          "push": true,
          "triage": true,
          "pull": true
        },
        "allow_auto_merge": false
      }
    }
  },
  "queries": [
    {
      "role": "shadow",
      "api_path": "repos/chebizarro/nostrc/actions/workflows/386168204/runs?per_page=100",
      "pages": 1,
      "reported_total": 6,
      "returned": 6,
      "response_sha256": "2ecd29261aea76fa792113ca4b62461cae8e4b140bdaa0bcee9d4110553c8fee",
      "captured_at": "2026-10-19T02:42:40.004579Z"
    },
    {
      "role": "broad",
      "api_path": "repos/chebizarro/nostrc/actions/workflows/386168202/runs?per_page=100",
      "pages": 1,
      "reported_total": 0,
      "returned": 0,
      "response_sha256": "f2aa3f2f9ea19c33f16347ee49859287a9b12cde87a89f7d6927ecb020c6b673",
      "captured_at": "2026-10-19T02:42:40.354859Z"
    },
    {
      "role": "worker",
      "api_path": "repos/chebizarro/nostrc/actions/workflows/386168205/runs?per_page=100",
      "pages": 1,
      "reported_total": 0,
      "returned": 0,
      "response_sha256": "f2aa3f2f9ea19c33f16347ee49859287a9b12cde87a89f7d6927ecb020c6b673",
      "captured_at": "2026-10-19T02:42:40.739187Z"
    },
    {
      "role": "release",
      "api_path": "repos/chebizarro/nostrc/actions/workflows/386168209/runs?per_page=100",
      "pages": 1,
      "reported_total": 0,
      "returned": 0,
      "response_sha256": "f2aa3f2f9ea19c33f16347ee49859287a9b12cde87a89f7d6927ecb020c6b673",
      "captured_at": "2026-10-19T02:42:41.058872Z"
    },
    {
      "role": "repository_window",
      "api_path": "repos/chebizarro/nostrc/actions/runs?per_page=100&created=>=2026-10-15T00:00:00Z",
      "pages": 2,
      "reported_total": 146,
      "returned": 146,
      "response_sha256": "abe44675c168b0678eabd06467c54317ec7bdc2cb6d690997202ff577ed0d39d",
      "captured_at": "2026-10-19T02:50:31.078002Z",
      "event_counts": {
        "push": 133,
        "schedule": 9,
        "workflow_dispatch": 4
      }
    },
    {
      "role": "38833746063-attempt1-jobs",
      "pages": 1,
      "reported_total": 56,
      "returned": 56,
      "response_sha256": "563f0b90430297ad0c5ce2f3c0188052994ad0cae388dbfb9876283e11e82d3e",
      "captured_at": "2026-10-19T02:44:12.051991Z"
    },
    {
      "role": "38833746063-artifacts",
      "pages": 1,
      "reported_total": 24,
      "returned": 24,
      "response_sha256": "eedb8b69ce4fabbff85bb9e0fe8bf9ec2a8ac692d861c18342e18f855f6f6681",
      "captured_at": "2026-10-19T02:44:07.449968Z"
    },
    {
      "role": "38833746063-same-sha-runs",
      "pages": 1,
      "reported_total": 13,
      "returned": 13,
      "response_sha256": "4eceb4131ca24b818fc663866cb172c261274441ac041358764351015d267655",
      "captured_at": "2026-10-19T02:44:08.019976Z"
    },
    {
      "role": "38845886339-attempt1-jobs",
      "pages": 1,
      "reported_total": 10,
      "returned": 10,
      "response_sha256": "08f567f62918431d8db119e9d899ccc25e0254e10c81641f25fe091e5fa66a5f",
      "captured_at": "2026-10-19T02:44:06.400236Z"
    },
    {
      "role": "38845886339-artifacts",
      "pages": 1,
      "reported_total": 0,
      "returned": 0,
      "response_sha256": "af3c25b59e7048d2e4b6ea03a688b2eced4e1786b5ae40f71516bc3ce0d4149e",
      "captured_at": "2026-10-19T02:44:06.459734Z"
    },
    {
      "role": "38845886339-same-sha-runs",
      "pages": 1,
      "reported_total": 12,
      "returned": 12,
      "response_sha256": "514a61123809cc39f8a63912bd4acbfaa5a2c907a0205983a24e3eba12c86e4f",
      "captured_at": "2026-10-19T02:44:07.048312Z"
    },
    {
      "role": "38856570439-attempt1-jobs",
      "pages": 1,
      "reported_total": 59,
      "returned": 59,
      "response_sha256": "b062fefc04e80e870552c2e7fe0a1fbaaa049d952a76e7f27254b265019b7c8d",
      "captured_at": "2026-10-19T02:44:10.456618Z"
    },
    {
      "role": "38856570439-artifacts",
      "pages": 1,
      "reported_total": 29,
      "returned": 29,
      "response_sha256": "3dde8db5aa493bc71b3d7cce9ed42fea2398a2e71316ee8c7a74b7c030621469",
      "captured_at": "2026-10-19T02:44:05.124866Z"
    },
    {
      "role": "38856570439-same-sha-runs",
      "pages": 1,
      "reported_total": 8,
      "returned": 8,
      "response_sha256": "2784f01cba9b3a96d4cea8ba7b7812f22b144d3ee05290380135674c8bc45a3a",
      "captured_at": "2026-10-19T02:44:05.671318Z"
    },
    {
      "role": "38873978927-attempt1-jobs",
      "pages": 1,
      "reported_total": 59,
      "returned": 59,
      "response_sha256": "4be0e5b0f18d878677919b450062643a906443dca293b07d332883f056c258a8",
      "captured_at": "2026-10-19T02:44:06.142123Z"
    },
    {
      "role": "38873978927-artifacts",
      "pages": 1,
      "reported_total": 33,
      "returned": 33,
      "response_sha256": "df7c39ade9f214e42d830c2fcf9a441a4ee49a222e7a17fe39c4e71a1bc1bc28",
      "captured_at": "2026-10-19T02:44:04.625611Z"
    },
    {
      "role": "38873978927-same-sha-runs",
      "pages": 1,
      "reported_total": 2,
      "returned": 2,
      "response_sha256": "98077fc79233f5981efc55399213ee5a9735768954c692604d6411632dcac837",
      "captured_at": "2026-10-19T02:44:04.820443Z"
    },
    {
      "role": "38878691465-attempt1-jobs",
      "pages": 1,
      "reported_total": 59,
      "returned": 59,
      "response_sha256": "83e935791f34378de2c3f90225e2b6125e00f3e394642cf46acb3d2caba9e5cb",
      "captured_at": "2026-10-19T02:44:04.401963Z"
    },
    {
      "role": "38878691465-artifacts",
      "pages": 1,
      "reported_total": 34,
      "returned": 34,
      "response_sha256": "6ebb4b5f438538c48552876f6df757338fd6dfd339da7546ba59600c166e1a21",
      "captured_at": "2026-10-19T02:43:59.615530Z"
    },
    {
      "role": "38878691465-same-sha-runs",
      "pages": 1,
      "reported_total": 17,
      "returned": 17,
      "response_sha256": "3debb92aacf5098912bacf0d548a311853f28d519f2c53dbc4f611e2b852f537",
      "captured_at": "2026-10-19T02:44:00.288501Z"
    },
    {
      "role": "39132650260-attempt1-jobs",
      "pages": 1,
      "reported_total": 61,
      "returned": 61,
      "response_sha256": "fc11496c942e6ded99dc0c7ba9d8da2da142554299c4948fd207253fbafc93c1",
      "captured_at": "2026-10-19T02:44:04.307392Z"
    },
    {
      "role": "39132650260-artifacts",
      "pages": 1,
      "reported_total": 46,
      "returned": 46,
      "response_sha256": "d18c33dcde213fe2dc71544f50e1f27ff789e378cdfdc5c5bfffa10339edf77e",
      "captured_at": "2026-10-19T02:43:58.935498Z"
    },
    {
      "role": "39132650260-same-sha-runs",
      "pages": 1,
      "reported_total": 12,
      "returned": 12,
      "response_sha256": "1bd92f22b650d7f78f79b09299ec25dbc299b0c7ff19196d504c63f0ad878ee5",
      "captured_at": "2026-10-19T02:43:59.271020Z"
    }
  ],
  "runs": [
    {
      "id": 38833746063,
      "run_attempt": 1,
      "event": "push",
      "head_sha": "5467c02d11f74efc1a85c833024efb654bbf98b5",
      "head_branch": "master",
      "status": "completed",
      "conclusion": "failure",
      "created_at": "2026-10-16T01:11:15Z",
      "run_started_at": "2026-10-16T01:11:15Z",
      "updated_at": "2026-10-16T01:56:42Z",
      "html_url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063",
      "workflow_id": 386168204,
      "path": ".github/workflows/dependency-tests.yml",
      "pull_request_numbers": [],
      "qualification": {
        "pr_comparison": "ineligible_event",
        "pr_reason": "No pull_request event/PR identity or reviewed legacy/selective pair; pushes/manual runs are not PR comparisons.",
        "broad_integration": "failed",
        "broad_reason": "Nested required Broad family accounting failed.",
        "accepted_pr_numbers": [],
        "accepted_broad": false,
        "required_workload_union_review": "not_accepted; accounting failure/absence is already disqualifying"
      },
      "job_count": 56,
      "job_conclusion_counts": {
        "success": 37,
        "failure": 18,
        "skipped": 1
      },
      "selection_and_worker_jobs": [
        {
          "id": 116758091484,
          "name": "select",
          "conclusion": "success",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758091484"
        },
        {
          "id": 116758466591,
          "name": "groundhog / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758466591"
        },
        {
          "id": 116758466628,
          "name": "core / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758466628"
        },
        {
          "id": 116758466655,
          "name": "grotto / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758466655"
        },
        {
          "id": 116758466682,
          "name": "mdk08 / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758466682"
        },
        {
          "id": 116758466683,
          "name": "policy / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758466683"
        },
        {
          "id": 116758466736,
          "name": "libgo / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758466736"
        },
        {
          "id": 116758466751,
          "name": "libgo_native / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758466751"
        },
        {
          "id": 116770728893,
          "name": "Dependency gate",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116770728893"
        }
      ],
      "gate_job": {
        "id": 116770728893,
        "name": "Dependency gate",
        "conclusion": "failure",
        "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116770728893"
      },
      "plan": {
        "plan_id": "13177f5cf5ac03e21ad9c92f4c0508112c9739a82aa39fca0a9cee9cc4071d39",
        "inventory_hash": "acab1855f888236d1681f3b217a75a6bf89ba43a00dd157fef2e3b0bed783c9f",
        "context": "ci",
        "effective_mode": "broad",
        "source_identity": {
          "kind": "committed",
          "sha": "5467c02d11f74efc1a85c833024efb654bbf98b5"
        },
        "expected_lane_ids": [
          "core.linux",
          "grotto.linux",
          "groundhog.linux",
          "groundhog.mdk08",
          "libgo.linux.asan",
          "libgo.macos.asan",
          "static.policy"
        ],
        "partial_profiles": [],
        "artifact_id": 12018358112
      },
      "validation": {
        "artifact_id": 12020390181,
        "member": "validation.json",
        "tested_sha": "5467c02d11f74efc1a85c833024efb654bbf98b5",
        "passed": false,
        "complete_requested": true,
        "complete_gate_eligible": null,
        "error_count": 18,
        "first_errors": [
          "selected worker job conclusions missing/skipped/failed/cancelled",
          "duplicate/unexpected lane: None",
          "missing lane: core.linux"
        ]
      },
      "lane_receipts": [
        {
          "artifact_id": 12018835234,
          "member": "libgo.linux.asan/receipt.json",
          "lane_id": null,
          "profile": null,
          "receipt_kind": null,
          "source_identity": {
            "kind": "committed",
            "sha": "5467c02d11f74efc1a85c833024efb654bbf98b5"
          },
          "job_conclusion": null,
          "registration_audit": null,
          "input_audit": null,
          "complete_gate_eligible": true,
          "failure_stage": null,
          "failure_errors": [],
          "input_audit_gap_counts": {},
          "check_count": 0,
          "check_outcome_counts": {},
          "observed_execution_count": 0,
          "positive_checks": {}
        }
      ],
      "artifacts": [
        {
          "artifact_id": 12020390181,
          "name": "dependency-validation-13177f5cf5ac03e21ad9c92f4c0508112c9739a82aa39fca0a9cee9cc4071d39-1",
          "expired": false,
          "metadata_size_bytes": 1300,
          "metadata_digest": "sha256:b8ac2ab8a59fa709281b663779193a950017cbb6ac8b8db12639b979effb067a",
          "download_sha256": "b8ac2ab8a59fa709281b663779193a950017cbb6ac8b8db12639b979effb067a",
          "download_size_bytes": 1300,
          "download_exit_code": 0,
          "role": "validation",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12020390181",
          "member_sha256": {
            "validation.json": "0b5d56b39400d6e75ae18db0d049051ddba2d7949b79b59349e15b40bd2e47a3"
          }
        },
        {
          "artifact_id": 12018835234,
          "name": "dependency-receipt-13177f5cf5ac03e21ad9c92f4c0508112c9739a82aa39fca0a9cee9cc4071d39-libgo.linux.asan-1",
          "expired": false,
          "metadata_size_bytes": 4321,
          "metadata_digest": "sha256:954effad9454a1caab4b1cd019bd9c91a4cdf6005bf35628e69c3000b02dfbe5",
          "download_sha256": "954effad9454a1caab4b1cd019bd9c91a4cdf6005bf35628e69c3000b02dfbe5",
          "download_size_bytes": 4321,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12018835234",
          "member_sha256": {
            "libgo.linux.asan/receipt.json": "91fb0f5eb2486fa40c34b68993469f85cfff44cb11c3681d06b93a3f7b611c2c",
            "libgo.linux.asan/worker.log": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
          }
        },
        {
          "artifact_id": 12018750668,
          "name": "dependency-receipt-13177f5cf5ac03e21ad9c92f4c0508112c9739a82aa39fca0a9cee9cc4071d39-static.policy-1",
          "expired": false,
          "metadata_size_bytes": 1961,
          "metadata_digest": "sha256:3be995285c5a9b5c2e91aeca7bcd1acaecbb0b6a3a7f5cf728627cd028ee32c9",
          "download_sha256": "3be995285c5a9b5c2e91aeca7bcd1acaecbb0b6a3a7f5cf728627cd028ee32c9",
          "download_size_bytes": 1961,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12018750668",
          "member_sha256": {
            "static.policy/worker.log": "4a7cf28857d07d9437cc8577973453f800cd0d1c09c08f87c42a663fbab379fc"
          }
        },
        {
          "artifact_id": 12018681047,
          "name": "dependency-receipt-13177f5cf5ac03e21ad9c92f4c0508112c9739a82aa39fca0a9cee9cc4071d39-core.linux-1",
          "expired": false,
          "metadata_size_bytes": 1979,
          "metadata_digest": "sha256:ddec85e715e67d4c50eeb44c3da2fbdbf9b2e6df1ae2f36d9caae8a69e9a2efa",
          "download_sha256": "ddec85e715e67d4c50eeb44c3da2fbdbf9b2e6df1ae2f36d9caae8a69e9a2efa",
          "download_size_bytes": 1979,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12018681047",
          "member_sha256": {
            "core.linux/worker.log": "40636bf31d482282f16239aa9b28a066fd325262743aaaa10e8efc838203f8fc"
          }
        },
        {
          "artifact_id": 12018632028,
          "name": "dependency-receipt-13177f5cf5ac03e21ad9c92f4c0508112c9739a82aa39fca0a9cee9cc4071d39-grotto.linux-1",
          "expired": false,
          "metadata_size_bytes": 1988,
          "metadata_digest": "sha256:46dee673c27ceab99aa4cd9d450258ad23ebdd6db7a3a41e24a73fe08cf7c23e",
          "download_sha256": "46dee673c27ceab99aa4cd9d450258ad23ebdd6db7a3a41e24a73fe08cf7c23e",
          "download_size_bytes": 1988,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12018632028",
          "member_sha256": {
            "grotto.linux/worker.log": "357760b953ed9794d43776201c4b37dae44cf756fc4f1c0ca6775a24774ffbdf"
          }
        },
        {
          "artifact_id": 12018551806,
          "name": "dependency-receipt-13177f5cf5ac03e21ad9c92f4c0508112c9739a82aa39fca0a9cee9cc4071d39-groundhog.linux-1",
          "expired": false,
          "metadata_size_bytes": 1776,
          "metadata_digest": "sha256:64a3f2cf381d3ed092b24c8e9340e574254870ef193c5b2f6d4d3252e16e7ea1",
          "download_sha256": "64a3f2cf381d3ed092b24c8e9340e574254870ef193c5b2f6d4d3252e16e7ea1",
          "download_size_bytes": 1776,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12018551806",
          "member_sha256": {
            "groundhog.linux/worker.log": "4a7dd6bca1438e9d2a4849654c7b0d15971fd6ef8ee129e5b73a122e4a7cf184"
          }
        },
        {
          "artifact_id": 12018358112,
          "name": "dependency-plan-13177f5cf5ac03e21ad9c92f4c0508112c9739a82aa39fca0a9cee9cc4071d39-1",
          "expired": false,
          "metadata_size_bytes": 5604,
          "metadata_digest": "sha256:07e3fca2cda270976b550693ce50ab8cfc4b5c6558da0e5e4fd5a0f6306b8703",
          "download_sha256": "07e3fca2cda270976b550693ce50ab8cfc4b5c6558da0e5e4fd5a0f6306b8703",
          "download_size_bytes": 5604,
          "download_exit_code": 0,
          "role": "plan",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12018358112",
          "member_sha256": {
            "plan.json": "5f70644abba0d049808df49ae391a549a3abb47ba51bab31ea633bf171973066"
          }
        },
        {
          "artifact_id": 12017873407,
          "name": "dependency-receipt-13177f5cf5ac03e21ad9c92f4c0508112c9739a82aa39fca0a9cee9cc4071d39-groundhog.mdk08-1",
          "expired": false,
          "metadata_size_bytes": 2012,
          "metadata_digest": "sha256:809d325a1f9986216fc73d666bf70f4f71c9ec077be7f2d800ddbaf2a565b8bd",
          "download_sha256": "809d325a1f9986216fc73d666bf70f4f71c9ec077be7f2d800ddbaf2a565b8bd",
          "download_size_bytes": 2012,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12017873407",
          "member_sha256": {
            "groundhog.mdk08/worker.log": "5da03945ef4d668e8ebfd523630fd8a6f5b2f0c866356f61f19eb957b10b2c63"
          }
        },
        {
          "artifact_id": 12017618868,
          "name": "dependency-receipt-13177f5cf5ac03e21ad9c92f4c0508112c9739a82aa39fca0a9cee9cc4071d39-libgo.macos.asan-1",
          "expired": false,
          "metadata_size_bytes": 170,
          "metadata_digest": "sha256:ce50a2393029a64e9195f4bfe850d3eb70fef725c2f70168ca7e1a76f099e3f6",
          "download_sha256": "ce50a2393029a64e9195f4bfe850d3eb70fef725c2f70168ca7e1a76f099e3f6",
          "download_size_bytes": 170,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12017618868",
          "member_sha256": {
            "libgo.macos.asan/worker.log": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
          }
        }
      ],
      "broad": {
        "nested_job_count": 47,
        "job_conclusion_counts": {
          "success": 36,
          "failure": 10,
          "skipped": 1
        },
        "accounting_job": {
          "id": 116770701220,
          "name": "broad / Broad family accounting",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116770701220"
        },
        "failed_jobs": [
          {
            "id": 116758467389,
            "name": "broad / bp_extended / nightly-matrix",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758467389"
          },
          {
            "id": 116758467415,
            "name": "broad / homed / build-and-test (debian-bookworm, debian:bookworm, apt-get update, apt-get install -y build-essent...",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758467415"
          },
          {
            "id": 116758467440,
            "name": "broad / homed / build-and-test (fedora-40, fedora:40, dnf -y update, dnf -y install @development-tools cmake ninj...",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758467440"
          },
          {
            "id": 116758467468,
            "name": "broad / homed / build-and-test (ubuntu-24.04, ubuntu:24.04, apt-get update, apt-get install -y build-essential cm...",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758467468"
          },
          {
            "id": 116758467475,
            "name": "broad / homed / build-and-test (archlinux, archlinux:latest, pacman -Syu --noconfirm, pacman -S --noconfirm base-...",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758467475"
          },
          {
            "id": 116758467498,
            "name": "broad / bp / bp-matrix",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758467498"
          },
          {
            "id": 116758736579,
            "name": "broad / fuzz / Fuzz json-event",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758736579"
          },
          {
            "id": 116758736596,
            "name": "broad / fuzz / Fuzz nip49",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758736596"
          },
          {
            "id": 116758736663,
            "name": "broad / fuzz / Fuzz bip39",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758736663"
          },
          {
            "id": 116770701220,
            "name": "broad / Broad family accounting",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116770701220"
          }
        ],
        "non_success_jobs": [
          {
            "id": 116760531207,
            "name": "broad / fuzz / Generate coverage report",
            "conclusion": "skipped",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116760531207"
          }
        ]
      },
      "same_sha_repository_runs": {
        "query_returned": 13,
        "other_run_ids": [
          38833745853,
          38833745877,
          38833745791,
          38833745813,
          38833745982,
          38833745874,
          38833745941,
          38833745916,
          38833745807,
          38833746195,
          38833745867,
          38833745789
        ],
        "conclusion_counts": {
          "success": 10,
          "failure": 3
        },
        "pairing_review": "Not a PR comparison; same SHA alone does not prove matching ranges/profiles/required coverage."
      },
      "workflow_file_sha256": {
        ".github/workflows/dependency-tests.yml": "d5b47588bd665c9dd14f2074ebfa071831e16a6cefb39743668eed070cd80be0",
        ".github/workflows/dependency-broad.yml": "79d5ce5af5f517d037b83fc5d4c92d3d31172a5ef059202e0a4b8ffe00d383b6"
      }
    },
    {
      "id": 38845886339,
      "run_attempt": 1,
      "event": "push",
      "head_sha": "33a1012767c9db5a46057280178134a544b86af7",
      "head_branch": "master",
      "status": "completed",
      "conclusion": "failure",
      "created_at": "2026-10-16T03:30:47Z",
      "run_started_at": "2026-10-16T03:30:47Z",
      "updated_at": "2026-10-16T03:32:15Z",
      "html_url": "https://github.com/chebizarro/nostrc/actions/runs/38845886339",
      "workflow_id": 386168204,
      "path": ".github/workflows/dependency-tests.yml",
      "pull_request_numbers": [],
      "qualification": {
        "pr_comparison": "ineligible_event",
        "pr_reason": "No pull_request event/PR identity or reviewed legacy/selective pair; pushes/manual runs are not PR comparisons.",
        "broad_integration": "blocked",
        "broad_reason": "Selection failed before broad children were launched.",
        "accepted_pr_numbers": [],
        "accepted_broad": false,
        "required_workload_union_review": "not_accepted; accounting failure/absence is already disqualifying"
      },
      "job_count": 10,
      "job_conclusion_counts": {
        "failure": 2,
        "skipped": 8
      },
      "selection_and_worker_jobs": [
        {
          "id": 116796201556,
          "name": "select",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38845886339/job/116796201556"
        },
        {
          "id": 116796544583,
          "name": "Dependency gate",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38845886339/job/116796544583"
        },
        {
          "id": 116796545238,
          "name": "groundhog",
          "conclusion": "skipped",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38845886339/job/116796545238"
        },
        {
          "id": 116796545266,
          "name": "libgo_native",
          "conclusion": "skipped",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38845886339/job/116796545266"
        },
        {
          "id": 116796545336,
          "name": "policy",
          "conclusion": "skipped",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38845886339/job/116796545336"
        },
        {
          "id": 116796545385,
          "name": "core",
          "conclusion": "skipped",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38845886339/job/116796545385"
        },
        {
          "id": 116796545512,
          "name": "broad",
          "conclusion": "skipped",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38845886339/job/116796545512"
        },
        {
          "id": 116796545607,
          "name": "mdk08",
          "conclusion": "skipped",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38845886339/job/116796545607"
        },
        {
          "id": 116796545608,
          "name": "libgo",
          "conclusion": "skipped",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38845886339/job/116796545608"
        },
        {
          "id": 116796545857,
          "name": "grotto",
          "conclusion": "skipped",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38845886339/job/116796545857"
        }
      ],
      "gate_job": {
        "id": 116796544583,
        "name": "Dependency gate",
        "conclusion": "failure",
        "url": "https://github.com/chebizarro/nostrc/actions/runs/38845886339/job/116796544583"
      },
      "plan": null,
      "validation": null,
      "lane_receipts": [],
      "artifacts": [],
      "broad": {
        "nested_job_count": 0,
        "job_conclusion_counts": {},
        "accounting_job": null,
        "failed_jobs": [],
        "non_success_jobs": []
      },
      "same_sha_repository_runs": {
        "query_returned": 12,
        "other_run_ids": [
          38845885901,
          38845886139,
          38845886143,
          38845886047,
          38845885985,
          38845885972,
          38845886109,
          38845886048,
          38845886024,
          38845886270,
          38845886016
        ],
        "conclusion_counts": {
          "success": 10,
          "failure": 2
        },
        "pairing_review": "Not a PR comparison; same SHA alone does not prove matching ranges/profiles/required coverage."
      },
      "workflow_file_sha256": {
        ".github/workflows/dependency-tests.yml": "d5b47588bd665c9dd14f2074ebfa071831e16a6cefb39743668eed070cd80be0",
        ".github/workflows/dependency-broad.yml": "79d5ce5af5f517d037b83fc5d4c92d3d31172a5ef059202e0a4b8ffe00d383b6"
      }
    },
    {
      "id": 38856570439,
      "run_attempt": 1,
      "event": "push",
      "head_sha": "8a031f443ac84850345a4cf0fb6ab5a8c3b32814",
      "head_branch": "master",
      "status": "completed",
      "conclusion": "failure",
      "created_at": "2026-10-16T05:35:29Z",
      "run_started_at": "2026-10-16T05:35:29Z",
      "updated_at": "2026-10-16T06:18:19Z",
      "html_url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439",
      "workflow_id": 386168204,
      "path": ".github/workflows/dependency-tests.yml",
      "pull_request_numbers": [],
      "qualification": {
        "pr_comparison": "ineligible_event",
        "pr_reason": "No pull_request event/PR identity or reviewed legacy/selective pair; pushes/manual runs are not PR comparisons.",
        "broad_integration": "failed",
        "broad_reason": "Nested required Broad family accounting failed.",
        "accepted_pr_numbers": [],
        "accepted_broad": false,
        "required_workload_union_review": "not_accepted; accounting failure/absence is already disqualifying"
      },
      "job_count": 59,
      "job_conclusion_counts": {
        "success": 39,
        "failure": 19,
        "skipped": 1
      },
      "selection_and_worker_jobs": [
        {
          "id": 116829255485,
          "name": "select",
          "conclusion": "success",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116829255485"
        },
        {
          "id": 116829330340,
          "name": "grotto / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116829330340"
        },
        {
          "id": 116829330347,
          "name": "groundhog_sanitizers / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116829330347"
        },
        {
          "id": 116829330369,
          "name": "core / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116829330369"
        },
        {
          "id": 116829330383,
          "name": "groundhog / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116829330383"
        },
        {
          "id": 116829330398,
          "name": "mdk08 / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116829330398"
        },
        {
          "id": 116829330419,
          "name": "policy / execute",
          "conclusion": "success",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116829330419"
        },
        {
          "id": 116829330482,
          "name": "core_asan / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116829330482"
        },
        {
          "id": 116829330520,
          "name": "libgo / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116829330520"
        },
        {
          "id": 116829330534,
          "name": "libgo_native / execute",
          "conclusion": "success",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116829330534"
        },
        {
          "id": 116829330573,
          "name": "core_native / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116829330573"
        },
        {
          "id": 116841190994,
          "name": "Dependency gate",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116841190994"
        }
      ],
      "gate_job": {
        "id": 116841190994,
        "name": "Dependency gate",
        "conclusion": "failure",
        "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116841190994"
      },
      "plan": {
        "plan_id": "3b0264bf4f094946e192da6f7db6340e25e16cc6af97084c0253017426e321df",
        "inventory_hash": "00045270c2a511ce96c674fe4a835842ed27719fa3cbcf6a4dc7a3cbd70fca5d",
        "context": "ci",
        "effective_mode": "broad",
        "source_identity": {
          "kind": "committed",
          "sha": "8a031f443ac84850345a4cf0fb6ab5a8c3b32814"
        },
        "expected_lane_ids": [
          "core.linux",
          "core.linux.asan",
          "core.macos.asan",
          "grotto.linux",
          "groundhog.linux",
          "groundhog.linux.sanitizers",
          "groundhog.mdk08",
          "libgo.linux.asan",
          "libgo.macos.asan",
          "static.policy"
        ],
        "partial_profiles": [],
        "artifact_id": 12027855083
      },
      "validation": {
        "artifact_id": 12028398157,
        "member": "validation.json",
        "tested_sha": "8a031f443ac84850345a4cf0fb6ab5a8c3b32814",
        "passed": false,
        "complete_requested": true,
        "complete_gate_eligible": false,
        "error_count": 25,
        "first_errors": [
          "inventory coverage/enforcement readiness cannot satisfy complete gate",
          "selected worker job conclusions missing/skipped/failed/cancelled",
          "duplicate/unexpected lane: None"
        ]
      },
      "lane_receipts": [
        {
          "artifact_id": 12027775498,
          "member": "libgo.linux.asan/receipt.json",
          "lane_id": null,
          "profile": null,
          "receipt_kind": null,
          "source_identity": {
            "kind": "committed",
            "sha": "8a031f443ac84850345a4cf0fb6ab5a8c3b32814"
          },
          "job_conclusion": null,
          "registration_audit": null,
          "input_audit": null,
          "complete_gate_eligible": false,
          "failure_stage": null,
          "failure_errors": [],
          "input_audit_gap_counts": {},
          "check_count": 0,
          "check_outcome_counts": {},
          "observed_execution_count": 0,
          "positive_checks": {}
        },
        {
          "artifact_id": 12027745788,
          "member": "static.policy/receipt.json",
          "lane_id": "static.policy",
          "profile": "static.policy",
          "receipt_kind": null,
          "source_identity": {
            "kind": "committed",
            "sha": "8a031f443ac84850345a4cf0fb6ab5a8c3b32814"
          },
          "job_conclusion": "success",
          "registration_audit": "passed",
          "input_audit": "passed",
          "complete_gate_eligible": false,
          "failure_stage": null,
          "failure_errors": [],
          "input_audit_gap_counts": {},
          "check_count": 1,
          "check_outcome_counts": {
            "passed": 1
          },
          "observed_execution_count": 1,
          "positive_checks": {
            "selection.contract": {
              "outcome": "passed",
              "observed_execution_count": 1
            }
          }
        },
        {
          "artifact_id": 12027323348,
          "member": "libgo.macos.asan/receipt.json",
          "lane_id": "libgo.macos.asan",
          "profile": "libgo.macos.asan",
          "receipt_kind": null,
          "source_identity": {
            "kind": "committed",
            "sha": "8a031f443ac84850345a4cf0fb6ab5a8c3b32814"
          },
          "job_conclusion": "success",
          "registration_audit": "passed",
          "input_audit": "passed",
          "complete_gate_eligible": false,
          "failure_stage": null,
          "failure_errors": [],
          "input_audit_gap_counts": {},
          "check_count": 2,
          "check_outcome_counts": {
            "passed": 2
          },
          "observed_execution_count": 32,
          "positive_checks": {
            "libgo.channel": {
              "outcome": "passed",
              "observed_execution_count": 1
            },
            "libgo.native_standalone": {
              "outcome": "passed",
              "observed_execution_count": 31
            }
          }
        },
        {
          "artifact_id": 12027248137,
          "member": "core.linux.asan/receipt.json",
          "lane_id": null,
          "profile": null,
          "receipt_kind": null,
          "source_identity": {
            "kind": "committed",
            "sha": "8a031f443ac84850345a4cf0fb6ab5a8c3b32814"
          },
          "job_conclusion": null,
          "registration_audit": null,
          "input_audit": null,
          "complete_gate_eligible": false,
          "failure_stage": null,
          "failure_errors": [],
          "input_audit_gap_counts": {},
          "check_count": 0,
          "check_outcome_counts": {},
          "observed_execution_count": 0,
          "positive_checks": {}
        },
        {
          "artifact_id": 12026609380,
          "member": "core.macos.asan/receipt.json",
          "lane_id": null,
          "profile": null,
          "receipt_kind": null,
          "source_identity": {
            "kind": "committed",
            "sha": "8a031f443ac84850345a4cf0fb6ab5a8c3b32814"
          },
          "job_conclusion": null,
          "registration_audit": null,
          "input_audit": null,
          "complete_gate_eligible": false,
          "failure_stage": null,
          "failure_errors": [],
          "input_audit_gap_counts": {},
          "check_count": 0,
          "check_outcome_counts": {},
          "observed_execution_count": 0,
          "positive_checks": {}
        }
      ],
      "artifacts": [
        {
          "artifact_id": 12028398157,
          "name": "dependency-validation-3b0264bf4f094946e192da6f7db6340e25e16cc6af97084c0253017426e321df-1",
          "expired": false,
          "metadata_size_bytes": 2080,
          "metadata_digest": "sha256:da8f733bfbeb5447b9c73ab387d4c8482230f1c6aa86c6fbe26c1f2abfd29171",
          "download_sha256": "da8f733bfbeb5447b9c73ab387d4c8482230f1c6aa86c6fbe26c1f2abfd29171",
          "download_size_bytes": 2080,
          "download_exit_code": 0,
          "role": "validation",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12028398157",
          "member_sha256": {
            "validation.json": "3277f02015f88e019ced874f11591e49aaebf5b3e499798d91526b7de264cfd7"
          }
        },
        {
          "artifact_id": 12027925197,
          "name": "dependency-receipt-3b0264bf4f094946e192da6f7db6340e25e16cc6af97084c0253017426e321df-groundhog.linux-1",
          "expired": false,
          "metadata_size_bytes": 1775,
          "metadata_digest": "sha256:d5f9c550fb5f4e4306ffc712b944fd4b6d219e2bb6e61acc7cf74b4ec5075cdc",
          "download_sha256": "d5f9c550fb5f4e4306ffc712b944fd4b6d219e2bb6e61acc7cf74b4ec5075cdc",
          "download_size_bytes": 1775,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12027925197",
          "member_sha256": {
            "groundhog.linux/worker.log": "edc07daa0324b386c10c5caafba13252e8094183aba0fecd224152bbf7125aca"
          }
        },
        {
          "artifact_id": 12027870329,
          "name": "dependency-receipt-3b0264bf4f094946e192da6f7db6340e25e16cc6af97084c0253017426e321df-groundhog.linux.sanitizers-1",
          "expired": false,
          "metadata_size_bytes": 1767,
          "metadata_digest": "sha256:dfedda03e0fa0b2f8b23fa988b67f35feb377b0917bcba0da667e0bc16dbd576",
          "download_sha256": "dfedda03e0fa0b2f8b23fa988b67f35feb377b0917bcba0da667e0bc16dbd576",
          "download_size_bytes": 1767,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12027870329",
          "member_sha256": {
            "groundhog.linux.sanitizers/worker.log": "868e377727999df01fab488474c4bde7ac9afab47ff625a838c9e2202744259a"
          }
        },
        {
          "artifact_id": 12027860350,
          "name": "dependency-receipt-3b0264bf4f094946e192da6f7db6340e25e16cc6af97084c0253017426e321df-core.linux-1",
          "expired": false,
          "metadata_size_bytes": 1977,
          "metadata_digest": "sha256:333a0918aa837fae0281061f7ab4b2a584326583886bc830fcc3542aa7646373",
          "download_sha256": "333a0918aa837fae0281061f7ab4b2a584326583886bc830fcc3542aa7646373",
          "download_size_bytes": 1977,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12027860350",
          "member_sha256": {
            "core.linux/worker.log": "4446cc84df12ad917964d4f3c0755e5c9e960d11725d83d17114ca94211fce32"
          }
        },
        {
          "artifact_id": 12027855083,
          "name": "dependency-plan-3b0264bf4f094946e192da6f7db6340e25e16cc6af97084c0253017426e321df-1",
          "expired": false,
          "metadata_size_bytes": 5261,
          "metadata_digest": "sha256:bd8753986fd193769a5576d1aae7af9d98b05e3a8787ae81ca641770428e6ba0",
          "download_sha256": "bd8753986fd193769a5576d1aae7af9d98b05e3a8787ae81ca641770428e6ba0",
          "download_size_bytes": 5261,
          "download_exit_code": 0,
          "role": "plan",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12027855083",
          "member_sha256": {
            "plan.json": "44a9071a16871d05c386b0aa0d697eb857a0cf62f746951789d60d60009ba3d4"
          }
        },
        {
          "artifact_id": 12027775498,
          "name": "dependency-receipt-3b0264bf4f094946e192da6f7db6340e25e16cc6af97084c0253017426e321df-libgo.linux.asan-1",
          "expired": false,
          "metadata_size_bytes": 5424,
          "metadata_digest": "sha256:50572effd00ef3b53d51f5629edd3745d87484a71678bf7e58b8c5bb705e2384",
          "download_sha256": "50572effd00ef3b53d51f5629edd3745d87484a71678bf7e58b8c5bb705e2384",
          "download_size_bytes": 5424,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12027775498",
          "member_sha256": {
            "libgo.linux.asan/receipt.json": "93f550bd53d009ae2994bd824aaa51f0f7258cd4a37b077990e9246d4f5b22d8",
            "libgo.linux.asan/worker.log": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
          }
        },
        {
          "artifact_id": 12027745788,
          "name": "dependency-receipt-3b0264bf4f094946e192da6f7db6340e25e16cc6af97084c0253017426e321df-static.policy-1",
          "expired": false,
          "metadata_size_bytes": 8661,
          "metadata_digest": "sha256:7080b2730d648ee04d49ba790c3e452902859722a51b4d3515e40d7fd834ac3a",
          "download_sha256": "7080b2730d648ee04d49ba790c3e452902859722a51b4d3515e40d7fd834ac3a",
          "download_size_bytes": 8661,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12027745788",
          "member_sha256": {
            "static.policy/receipt.json": "c6fdeffd240ba8b138c3c4d0afde99334ad1dcc543b7dc821576812a1503d1a4",
            "static.policy/worker.log": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
          }
        },
        {
          "artifact_id": 12027457503,
          "name": "dependency-receipt-3b0264bf4f094946e192da6f7db6340e25e16cc6af97084c0253017426e321df-groundhog.mdk08-1",
          "expired": false,
          "metadata_size_bytes": 2012,
          "metadata_digest": "sha256:7f7f63c38e4f3e974beba0c4a558eff86ddb58c6ce9224561d5ee515aefdb0d5",
          "download_sha256": "7f7f63c38e4f3e974beba0c4a558eff86ddb58c6ce9224561d5ee515aefdb0d5",
          "download_size_bytes": 2012,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12027457503",
          "member_sha256": {
            "groundhog.mdk08/worker.log": "53767ea03c688d0ee5ba59e52eeb1186aae9d724f22f4e6593619c6116664748"
          }
        },
        {
          "artifact_id": 12027452461,
          "name": "dependency-receipt-3b0264bf4f094946e192da6f7db6340e25e16cc6af97084c0253017426e321df-grotto.linux-1",
          "expired": false,
          "metadata_size_bytes": 1988,
          "metadata_digest": "sha256:4b5d22686effd46db2c43fdf191cd3179bb8417c18d433bc41c843cd3d86e2d4",
          "download_sha256": "4b5d22686effd46db2c43fdf191cd3179bb8417c18d433bc41c843cd3d86e2d4",
          "download_size_bytes": 1988,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12027452461",
          "member_sha256": {
            "grotto.linux/worker.log": "2851323a45aca37bd15887212124609e18e71f2ec07b6c5aa0c3ab9a6f11d655"
          }
        },
        {
          "artifact_id": 12027323348,
          "name": "dependency-receipt-3b0264bf4f094946e192da6f7db6340e25e16cc6af97084c0253017426e321df-libgo.macos.asan-1",
          "expired": false,
          "metadata_size_bytes": 20323,
          "metadata_digest": "sha256:5a74861f3df21dcb03668e867de5efe363e2f96a4a179b2c4cc53722f6ffb14a",
          "download_sha256": "5a74861f3df21dcb03668e867de5efe363e2f96a4a179b2c4cc53722f6ffb14a",
          "download_size_bytes": 20323,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12027323348",
          "member_sha256": {
            "libgo.macos.asan/receipt.json": "c5fbe6b294507260c5230e99a21b6eefee40de48168f3de78185b8ab3899205a",
            "libgo.macos.asan/worker.log": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
          }
        },
        {
          "artifact_id": 12027248137,
          "name": "dependency-receipt-3b0264bf4f094946e192da6f7db6340e25e16cc6af97084c0253017426e321df-core.linux.asan-1",
          "expired": false,
          "metadata_size_bytes": 80343,
          "metadata_digest": "sha256:360863bb953b128bb875b71a8ec292051cb7ac45db9f18a79851f4eeb31f740e",
          "download_sha256": "360863bb953b128bb875b71a8ec292051cb7ac45db9f18a79851f4eeb31f740e",
          "download_size_bytes": 80343,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12027248137",
          "member_sha256": {
            "core.linux.asan/receipt.json": "d367c0655464f578f0f51d5306099a2a749de3518e1723f3134616a40ed0d60f",
            "core.linux.asan/worker.log": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
          }
        },
        {
          "artifact_id": 12026609380,
          "name": "dependency-receipt-3b0264bf4f094946e192da6f7db6340e25e16cc6af97084c0253017426e321df-core.macos.asan-1",
          "expired": false,
          "metadata_size_bytes": 73562,
          "metadata_digest": "sha256:a6b7d7373d1fe5a86daf32a85e721733503b6f43670d26f73d7b54f273d5bb34",
          "download_sha256": "a6b7d7373d1fe5a86daf32a85e721733503b6f43670d26f73d7b54f273d5bb34",
          "download_size_bytes": 73562,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12026609380",
          "member_sha256": {
            "core.macos.asan/receipt.json": "48309a0192fb93454baaad6b04783e018c06eaa944ff008bd2d66797d6e71b03",
            "core.macos.asan/worker.log": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
          }
        }
      ],
      "broad": {
        "nested_job_count": 47,
        "job_conclusion_counts": {
          "success": 36,
          "failure": 10,
          "skipped": 1
        },
        "accounting_job": {
          "id": 116841163905,
          "name": "broad / Broad family accounting",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116841163905"
        },
        "failed_jobs": [
          {
            "id": 116829330879,
            "name": "broad / homed / build-and-test (fedora-40, fedora:40, dnf -y update, dnf -y install @development-tools cmake ninj...",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116829330879"
          },
          {
            "id": 116829330888,
            "name": "broad / bp_extended / nightly-matrix",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116829330888"
          },
          {
            "id": 116829330914,
            "name": "broad / homed / build-and-test (debian-bookworm, debian:bookworm, apt-get update, apt-get install -y build-essent...",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116829330914"
          },
          {
            "id": 116829330915,
            "name": "broad / homed / build-and-test (ubuntu-24.04, ubuntu:24.04, apt-get update, apt-get install -y build-essential cm...",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116829330915"
          },
          {
            "id": 116829330967,
            "name": "broad / homed / build-and-test (archlinux, archlinux:latest, pacman -Syu --noconfirm, pacman -S --noconfirm base-...",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116829330967"
          },
          {
            "id": 116829331088,
            "name": "broad / bp / bp-matrix",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116829331088"
          },
          {
            "id": 116831017659,
            "name": "broad / fuzz / Fuzz bip39",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116831017659"
          },
          {
            "id": 116831017669,
            "name": "broad / fuzz / Fuzz json-event",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116831017669"
          },
          {
            "id": 116831017860,
            "name": "broad / fuzz / Fuzz nip49",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116831017860"
          },
          {
            "id": 116841163905,
            "name": "broad / Broad family accounting",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116841163905"
          }
        ],
        "non_success_jobs": [
          {
            "id": 116831289438,
            "name": "broad / fuzz / Generate coverage report",
            "conclusion": "skipped",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116831289438"
          }
        ]
      },
      "same_sha_repository_runs": {
        "query_returned": 8,
        "other_run_ids": [
          38874039865,
          38871961965,
          38856570251,
          38856570237,
          38856570261,
          38856570245,
          38856570188
        ],
        "conclusion_counts": {
          "success": 6,
          "failure": 2
        },
        "pairing_review": "Not a PR comparison; same SHA alone does not prove matching ranges/profiles/required coverage."
      },
      "workflow_file_sha256": {
        ".github/workflows/dependency-tests.yml": "215dd788507eb2fff04dd3ed70193126006ec642bf949b3689fc8b1415e486e2",
        ".github/workflows/dependency-broad.yml": "79d5ce5af5f517d037b83fc5d4c92d3d31172a5ef059202e0a4b8ffe00d383b6"
      }
    },
    {
      "id": 38873978927,
      "run_attempt": 1,
      "event": "workflow_dispatch",
      "head_sha": "a478070509569c26917dd5fb13a548b1f68eade9",
      "head_branch": "ci/testing-final-integration-20261016",
      "status": "completed",
      "conclusion": "cancelled",
      "created_at": "2026-10-16T08:29:01Z",
      "run_started_at": "2026-10-16T08:29:01Z",
      "updated_at": "2026-10-16T09:19:49Z",
      "html_url": "https://github.com/chebizarro/nostrc/actions/runs/38873978927",
      "workflow_id": 386168204,
      "path": ".github/workflows/dependency-tests.yml",
      "pull_request_numbers": [],
      "qualification": {
        "pr_comparison": "ineligible_event",
        "pr_reason": "No pull_request event/PR identity or reviewed legacy/selective pair; pushes/manual runs are not PR comparisons.",
        "broad_integration": "failed",
        "broad_reason": "Nested required Broad family accounting failed.",
        "accepted_pr_numbers": [],
        "accepted_broad": false,
        "required_workload_union_review": "not_accepted; accounting failure/absence is already disqualifying"
      },
      "job_count": 59,
      "job_conclusion_counts": {
        "success": 45,
        "failure": 13,
        "skipped": 1
      },
      "selection_and_worker_jobs": [
        {
          "id": 116884499254,
          "name": "select",
          "conclusion": "success",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116884499254"
        },
        {
          "id": 116884595889,
          "name": "groundhog_sanitizers / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116884595889"
        },
        {
          "id": 116884595890,
          "name": "groundhog / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116884595890"
        },
        {
          "id": 116884595942,
          "name": "core_native / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116884595942"
        },
        {
          "id": 116884595995,
          "name": "grotto / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116884595995"
        },
        {
          "id": 116884596024,
          "name": "core_asan / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116884596024"
        },
        {
          "id": 116884596077,
          "name": "libgo_native / execute",
          "conclusion": "success",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116884596077"
        },
        {
          "id": 116884596080,
          "name": "policy / execute",
          "conclusion": "success",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116884596080"
        },
        {
          "id": 116884596182,
          "name": "mdk08 / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116884596182"
        },
        {
          "id": 116884596184,
          "name": "core / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116884596184"
        },
        {
          "id": 116884596359,
          "name": "libgo / execute",
          "conclusion": "success",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116884596359"
        },
        {
          "id": 116900256052,
          "name": "Dependency gate",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116900256052"
        }
      ],
      "gate_job": {
        "id": 116900256052,
        "name": "Dependency gate",
        "conclusion": "failure",
        "url": "https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116900256052"
      },
      "plan": {
        "plan_id": "3aba1a4e60bdf008fe6079e12d9baeaa67bd06f29517b4d319ee0a6e57112744",
        "inventory_hash": "6b272061d8c62dec6cc67813576d3d5f0c6923a859fba672971c66a213e0edbc",
        "context": "ci",
        "effective_mode": "broad",
        "source_identity": {
          "kind": "committed",
          "sha": "a478070509569c26917dd5fb13a548b1f68eade9"
        },
        "expected_lane_ids": [
          "core.linux",
          "core.linux.asan",
          "core.macos.asan",
          "grotto.linux",
          "groundhog.linux",
          "groundhog.linux.sanitizers",
          "groundhog.mdk08",
          "libgo.linux.asan",
          "libgo.macos.asan",
          "static.policy"
        ],
        "partial_profiles": [],
        "artifact_id": 12034472127
      },
      "validation": {
        "artifact_id": 12036857589,
        "member": "validation.json",
        "tested_sha": "a478070509569c26917dd5fb13a548b1f68eade9",
        "passed": false,
        "complete_requested": true,
        "complete_gate_eligible": false,
        "error_count": 24,
        "first_errors": [
          "inventory coverage/enforcement readiness cannot satisfy complete gate",
          "selected worker job conclusions missing/skipped/failed/cancelled",
          "duplicate/unexpected lane: None"
        ]
      },
      "lane_receipts": [
        {
          "artifact_id": 12034791870,
          "member": "groundhog.linux.sanitizers/receipt.json",
          "lane_id": null,
          "profile": null,
          "receipt_kind": null,
          "source_identity": {
            "kind": "committed",
            "sha": "a478070509569c26917dd5fb13a548b1f68eade9"
          },
          "job_conclusion": null,
          "registration_audit": null,
          "input_audit": null,
          "complete_gate_eligible": false,
          "failure_stage": null,
          "failure_errors": [],
          "input_audit_gap_counts": {},
          "check_count": 0,
          "check_outcome_counts": {},
          "observed_execution_count": 0,
          "positive_checks": {}
        },
        {
          "artifact_id": 12034733294,
          "member": "core.linux.asan/receipt.json",
          "lane_id": null,
          "profile": null,
          "receipt_kind": null,
          "source_identity": {
            "kind": "committed",
            "sha": "a478070509569c26917dd5fb13a548b1f68eade9"
          },
          "job_conclusion": null,
          "registration_audit": null,
          "input_audit": null,
          "complete_gate_eligible": false,
          "failure_stage": null,
          "failure_errors": [],
          "input_audit_gap_counts": {},
          "check_count": 0,
          "check_outcome_counts": {},
          "observed_execution_count": 0,
          "positive_checks": {}
        },
        {
          "artifact_id": 12034417836,
          "member": "libgo.linux.asan/receipt.json",
          "lane_id": "libgo.linux.asan",
          "profile": "libgo.linux.asan",
          "receipt_kind": null,
          "source_identity": {
            "kind": "committed",
            "sha": "a478070509569c26917dd5fb13a548b1f68eade9"
          },
          "job_conclusion": "success",
          "registration_audit": "passed",
          "input_audit": "passed",
          "complete_gate_eligible": false,
          "failure_stage": null,
          "failure_errors": [],
          "input_audit_gap_counts": {},
          "check_count": 2,
          "check_outcome_counts": {
            "passed": 2
          },
          "observed_execution_count": 33,
          "positive_checks": {
            "libgo.channel": {
              "outcome": "passed",
              "observed_execution_count": 1
            },
            "libgo.native_standalone": {
              "outcome": "passed",
              "observed_execution_count": 32
            }
          }
        },
        {
          "artifact_id": 12034264843,
          "member": "core.macos.asan/receipt.json",
          "lane_id": null,
          "profile": null,
          "receipt_kind": null,
          "source_identity": {
            "kind": "committed",
            "sha": "a478070509569c26917dd5fb13a548b1f68eade9"
          },
          "job_conclusion": null,
          "registration_audit": null,
          "input_audit": null,
          "complete_gate_eligible": false,
          "failure_stage": null,
          "failure_errors": [],
          "input_audit_gap_counts": {},
          "check_count": 0,
          "check_outcome_counts": {},
          "observed_execution_count": 0,
          "positive_checks": {}
        },
        {
          "artifact_id": 12034124206,
          "member": "libgo.macos.asan/receipt.json",
          "lane_id": "libgo.macos.asan",
          "profile": "libgo.macos.asan",
          "receipt_kind": null,
          "source_identity": {
            "kind": "committed",
            "sha": "a478070509569c26917dd5fb13a548b1f68eade9"
          },
          "job_conclusion": "success",
          "registration_audit": "passed",
          "input_audit": "passed",
          "complete_gate_eligible": false,
          "failure_stage": null,
          "failure_errors": [],
          "input_audit_gap_counts": {},
          "check_count": 2,
          "check_outcome_counts": {
            "passed": 2
          },
          "observed_execution_count": 32,
          "positive_checks": {
            "libgo.channel": {
              "outcome": "passed",
              "observed_execution_count": 1
            },
            "libgo.native_standalone": {
              "outcome": "passed",
              "observed_execution_count": 31
            }
          }
        },
        {
          "artifact_id": 12033829723,
          "member": "static.policy/receipt.json",
          "lane_id": "static.policy",
          "profile": "static.policy",
          "receipt_kind": null,
          "source_identity": {
            "kind": "committed",
            "sha": "a478070509569c26917dd5fb13a548b1f68eade9"
          },
          "job_conclusion": "success",
          "registration_audit": "passed",
          "input_audit": "passed",
          "complete_gate_eligible": false,
          "failure_stage": null,
          "failure_errors": [],
          "input_audit_gap_counts": {},
          "check_count": 1,
          "check_outcome_counts": {
            "passed": 1
          },
          "observed_execution_count": 1,
          "positive_checks": {
            "selection.contract": {
              "outcome": "passed",
              "observed_execution_count": 1
            }
          }
        }
      ],
      "artifacts": [
        {
          "artifact_id": 12036857589,
          "name": "dependency-validation-3aba1a4e60bdf008fe6079e12d9baeaa67bd06f29517b4d319ee0a6e57112744-1",
          "expired": false,
          "metadata_size_bytes": 4894,
          "metadata_digest": "sha256:63f3e13b30c9537282b954bd02f8a3bb384853c696c904f47b5743d412c23bd5",
          "download_sha256": "63f3e13b30c9537282b954bd02f8a3bb384853c696c904f47b5743d412c23bd5",
          "download_size_bytes": 4894,
          "download_exit_code": 0,
          "role": "validation",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12036857589",
          "member_sha256": {
            "validation.json": "99e22ac4dde85a7a3efb7c68c5074f85be8e8ebc2f0f195f582ba76e2662956c"
          }
        },
        {
          "artifact_id": 12035130067,
          "name": "dependency-receipt-3aba1a4e60bdf008fe6079e12d9baeaa67bd06f29517b4d319ee0a6e57112744-groundhog.mdk08-1",
          "expired": false,
          "metadata_size_bytes": 2109,
          "metadata_digest": "sha256:3fe33129e0f5c864c4cdcb1518c06d748182841ac720871e467c919a33547299",
          "download_sha256": "3fe33129e0f5c864c4cdcb1518c06d748182841ac720871e467c919a33547299",
          "download_size_bytes": 2109,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12035130067",
          "member_sha256": {
            "groundhog.mdk08/worker.log": "ac96f17d194a78de4f445d277556bcffaff7dbfbc5071bec4c3168ff782bd6da"
          }
        },
        {
          "artifact_id": 12034842125,
          "name": "dependency-receipt-3aba1a4e60bdf008fe6079e12d9baeaa67bd06f29517b4d319ee0a6e57112744-core.linux-1",
          "expired": false,
          "metadata_size_bytes": 2074,
          "metadata_digest": "sha256:6b10f50468d43a7f3e779056548b85ae1334c7b3bcb9463b124a5e664ca9b2f1",
          "download_sha256": "6b10f50468d43a7f3e779056548b85ae1334c7b3bcb9463b124a5e664ca9b2f1",
          "download_size_bytes": 2074,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12034842125",
          "member_sha256": {
            "core.linux/worker.log": "c8d5bc89c749148810cbbc4cabc615f81f41bca3e1e939c7a778dd8d890892eb"
          }
        },
        {
          "artifact_id": 12034791870,
          "name": "dependency-receipt-3aba1a4e60bdf008fe6079e12d9baeaa67bd06f29517b4d319ee0a6e57112744-groundhog.linux.sanitizers-1",
          "expired": false,
          "metadata_size_bytes": 68668,
          "metadata_digest": "sha256:beda93545659efc8f976960edbc00dc078c48290bbbac4cc3f497b20fc9d177a",
          "download_sha256": "beda93545659efc8f976960edbc00dc078c48290bbbac4cc3f497b20fc9d177a",
          "download_size_bytes": 68668,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12034791870",
          "member_sha256": {
            "groundhog.linux.sanitizers/receipt.json": "7244df156e6a17bcfb4d8878607a42745aba1f93666cad955bba80fca1f3036e",
            "groundhog.linux.sanitizers/worker.log": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
          }
        },
        {
          "artifact_id": 12034733294,
          "name": "dependency-receipt-3aba1a4e60bdf008fe6079e12d9baeaa67bd06f29517b4d319ee0a6e57112744-core.linux.asan-1",
          "expired": false,
          "metadata_size_bytes": 34640,
          "metadata_digest": "sha256:4171015a677e4f7e7c8b65d2690a4e490c89d4332636a4d54b43f71a4236cc64",
          "download_sha256": "4171015a677e4f7e7c8b65d2690a4e490c89d4332636a4d54b43f71a4236cc64",
          "download_size_bytes": 34640,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12034733294",
          "member_sha256": {
            "core.linux.asan/receipt.json": "5495484822f6e16f6b625ddc6aa2d948060686655593f191252aeb3dbad5557d",
            "core.linux.asan/worker.log": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
          }
        },
        {
          "artifact_id": 12034472127,
          "name": "dependency-plan-3aba1a4e60bdf008fe6079e12d9baeaa67bd06f29517b4d319ee0a6e57112744-1",
          "expired": false,
          "metadata_size_bytes": 6776,
          "metadata_digest": "sha256:c216c2bb4dff51a9a1e4603b228dcb8240966e339159f42e18f9a9d2fd5c67b7",
          "download_sha256": "c216c2bb4dff51a9a1e4603b228dcb8240966e339159f42e18f9a9d2fd5c67b7",
          "download_size_bytes": 6776,
          "download_exit_code": 0,
          "role": "plan",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12034472127",
          "member_sha256": {
            "plan.json": "408d0ea27eb604eb6bc954747afe26c01e99360a4ff5859a41b08fe3ec0d1fc0"
          }
        },
        {
          "artifact_id": 12034417836,
          "name": "dependency-receipt-3aba1a4e60bdf008fe6079e12d9baeaa67bd06f29517b4d319ee0a6e57112744-libgo.linux.asan-1",
          "expired": false,
          "metadata_size_bytes": 24305,
          "metadata_digest": "sha256:cd9df3e930dab5d193a4b099950d3587851365d61d030bc6c39ed92a4046b92c",
          "download_sha256": "cd9df3e930dab5d193a4b099950d3587851365d61d030bc6c39ed92a4046b92c",
          "download_size_bytes": 24305,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12034417836",
          "member_sha256": {
            "libgo.linux.asan/receipt.json": "94488c8891032cf926de8a0a60e5133ffb37b6d47eb47cba7249dc0eaad992d5",
            "libgo.linux.asan/worker.log": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
          }
        },
        {
          "artifact_id": 12034264843,
          "name": "dependency-receipt-3aba1a4e60bdf008fe6079e12d9baeaa67bd06f29517b4d319ee0a6e57112744-core.macos.asan-1",
          "expired": false,
          "metadata_size_bytes": 34601,
          "metadata_digest": "sha256:7d3c9506cdad652ef213d10b1f5c79d245edea889fdc1a1438b1d8d738887a09",
          "download_sha256": "7d3c9506cdad652ef213d10b1f5c79d245edea889fdc1a1438b1d8d738887a09",
          "download_size_bytes": 34601,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12034264843",
          "member_sha256": {
            "core.macos.asan/receipt.json": "ea07f17ac8975583d02e2d7fb5578adf35bebf32f8da653dc684734b360355c6",
            "core.macos.asan/worker.log": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
          }
        },
        {
          "artifact_id": 12034124206,
          "name": "dependency-receipt-3aba1a4e60bdf008fe6079e12d9baeaa67bd06f29517b4d319ee0a6e57112744-libgo.macos.asan-1",
          "expired": false,
          "metadata_size_bytes": 27466,
          "metadata_digest": "sha256:0274f5bd9fc93411a2746fa9e0bc8d5d5e04607c6c10f43751502ad875176817",
          "download_sha256": "0274f5bd9fc93411a2746fa9e0bc8d5d5e04607c6c10f43751502ad875176817",
          "download_size_bytes": 27466,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12034124206",
          "member_sha256": {
            "libgo.macos.asan/worker.log": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
            "libgo.macos.asan/receipt.json": "325b24a9b926cfa69bebbc2c177244ebf43d737c832d570f0c33504d2c659348"
          }
        },
        {
          "artifact_id": 12033953011,
          "name": "dependency-receipt-3aba1a4e60bdf008fe6079e12d9baeaa67bd06f29517b4d319ee0a6e57112744-groundhog.linux-1",
          "expired": false,
          "metadata_size_bytes": 234,
          "metadata_digest": "sha256:aede9821e2d1ac46165ff539a58ec07357d62234830b002b948c0453c92d3fcf",
          "download_sha256": "aede9821e2d1ac46165ff539a58ec07357d62234830b002b948c0453c92d3fcf",
          "download_size_bytes": 234,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12033953011",
          "member_sha256": {
            "groundhog.linux/worker.log": "c3248197fc46c39acc93fb321661c79fa213f9f3ab8d6d7c86ed0ff9fb1d050e"
          }
        },
        {
          "artifact_id": 12033829723,
          "name": "dependency-receipt-3aba1a4e60bdf008fe6079e12d9baeaa67bd06f29517b4d319ee0a6e57112744-static.policy-1",
          "expired": false,
          "metadata_size_bytes": 20630,
          "metadata_digest": "sha256:dc9d81720c68c9bee696dcc4a1a04160ac5e49b90a8f2af626eae063bff88dac",
          "download_sha256": "dc9d81720c68c9bee696dcc4a1a04160ac5e49b90a8f2af626eae063bff88dac",
          "download_size_bytes": 20630,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12033829723",
          "member_sha256": {
            "static.policy/receipt.json": "8182e76c2295c5cf4b9a0fb82fe0a2166dc25c5d55f7b17f91fd78dd03848cc9",
            "static.policy/worker.log": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
          }
        },
        {
          "artifact_id": 12033788282,
          "name": "dependency-receipt-3aba1a4e60bdf008fe6079e12d9baeaa67bd06f29517b4d319ee0a6e57112744-grotto.linux-1",
          "expired": false,
          "metadata_size_bytes": 2085,
          "metadata_digest": "sha256:7f7e0e4991ec61ae0253ea4fbab0a99dd52c78c56999662d6b1014330bd3cac6",
          "download_sha256": "7f7e0e4991ec61ae0253ea4fbab0a99dd52c78c56999662d6b1014330bd3cac6",
          "download_size_bytes": 2085,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12033788282",
          "member_sha256": {
            "grotto.linux/worker.log": "08b424f999d5834b8de7e169b4c51c66500943b1e03f61e97833e0f1008a2038"
          }
        }
      ],
      "broad": {
        "nested_job_count": 47,
        "job_conclusion_counts": {
          "success": 41,
          "failure": 5,
          "skipped": 1
        },
        "accounting_job": {
          "id": 116900093654,
          "name": "broad / Broad family accounting",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116900093654"
        },
        "failed_jobs": [
          {
            "id": 116884596656,
            "name": "broad / groundhog / groundhog-sanitizers",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116884596656"
          },
          {
            "id": 116884597017,
            "name": "broad / bp / bp-matrix",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116884597017"
          },
          {
            "id": 116884597175,
            "name": "broad / bp_extended / nightly-matrix",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116884597175"
          },
          {
            "id": 116885248265,
            "name": "broad / fuzz / Fuzz json-event",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116885248265"
          },
          {
            "id": 116900093654,
            "name": "broad / Broad family accounting",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116900093654"
          }
        ],
        "non_success_jobs": [
          {
            "id": 116889115593,
            "name": "broad / fuzz / Generate coverage report",
            "conclusion": "skipped",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116889115593"
          }
        ]
      },
      "same_sha_repository_runs": {
        "query_returned": 2,
        "other_run_ids": [
          38873970340
        ],
        "conclusion_counts": {
          "cancelled": 1,
          "success": 1
        },
        "pairing_review": "Not a PR comparison; same SHA alone does not prove matching ranges/profiles/required coverage."
      },
      "workflow_file_sha256": {
        ".github/workflows/dependency-tests.yml": "215dd788507eb2fff04dd3ed70193126006ec642bf949b3689fc8b1415e486e2",
        ".github/workflows/dependency-broad.yml": "79d5ce5af5f517d037b83fc5d4c92d3d31172a5ef059202e0a4b8ffe00d383b6"
      }
    },
    {
      "id": 38878691465,
      "run_attempt": 1,
      "event": "push",
      "head_sha": "249f9657fb230947ba486fb3cd92894316f9acc3",
      "head_branch": "master",
      "status": "completed",
      "conclusion": "failure",
      "created_at": "2026-10-16T09:10:58Z",
      "run_started_at": "2026-10-16T09:10:58Z",
      "updated_at": "2026-10-16T09:53:52Z",
      "html_url": "https://github.com/chebizarro/nostrc/actions/runs/38878691465",
      "workflow_id": 386168204,
      "path": ".github/workflows/dependency-tests.yml",
      "pull_request_numbers": [],
      "qualification": {
        "pr_comparison": "ineligible_event",
        "pr_reason": "No pull_request event/PR identity or reviewed legacy/selective pair; pushes/manual runs are not PR comparisons.",
        "broad_integration": "failed",
        "broad_reason": "Nested required Broad family accounting failed.",
        "accepted_pr_numbers": [],
        "accepted_broad": false,
        "required_workload_union_review": "not_accepted; accounting failure/absence is already disqualifying"
      },
      "job_count": 59,
      "job_conclusion_counts": {
        "success": 45,
        "failure": 13,
        "skipped": 1
      },
      "selection_and_worker_jobs": [
        {
          "id": 116899872177,
          "name": "select",
          "conclusion": "success",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116899872177"
        },
        {
          "id": 116899991146,
          "name": "groundhog_sanitizers / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116899991146"
        },
        {
          "id": 116899991148,
          "name": "grotto / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116899991148"
        },
        {
          "id": 116899991244,
          "name": "core / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116899991244"
        },
        {
          "id": 116899991264,
          "name": "groundhog / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116899991264"
        },
        {
          "id": 116899991307,
          "name": "core_asan / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116899991307"
        },
        {
          "id": 116899991318,
          "name": "mdk08 / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116899991318"
        },
        {
          "id": 116899991384,
          "name": "libgo / execute",
          "conclusion": "success",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116899991384"
        },
        {
          "id": 116899991387,
          "name": "policy / execute",
          "conclusion": "success",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116899991387"
        },
        {
          "id": 116899991439,
          "name": "libgo_native / execute",
          "conclusion": "success",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116899991439"
        },
        {
          "id": 116899992047,
          "name": "core_native / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116899992047"
        },
        {
          "id": 116915750038,
          "name": "Dependency gate",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116915750038"
        }
      ],
      "gate_job": {
        "id": 116915750038,
        "name": "Dependency gate",
        "conclusion": "failure",
        "url": "https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116915750038"
      },
      "plan": {
        "plan_id": "ef5fe08b9b15da727973302b5e13b6c79376062eba33b579d6d0b81da195425c",
        "inventory_hash": "e074ff04674faa90f3335b9a5177a5c236ebf5d74f5e8ab81cf4e8943c73a95a",
        "context": "ci",
        "effective_mode": "broad",
        "source_identity": {
          "kind": "committed",
          "sha": "249f9657fb230947ba486fb3cd92894316f9acc3"
        },
        "expected_lane_ids": [
          "core.linux",
          "core.linux.asan",
          "core.macos.asan",
          "grotto.linux",
          "groundhog.linux",
          "groundhog.linux.sanitizers",
          "groundhog.mdk08",
          "libgo.linux.asan",
          "libgo.macos.asan",
          "static.policy"
        ],
        "partial_profiles": [],
        "artifact_id": 12036446528
      },
      "validation": {
        "artifact_id": 12038423327,
        "member": "validation.json",
        "tested_sha": "249f9657fb230947ba486fb3cd92894316f9acc3",
        "passed": false,
        "complete_requested": true,
        "complete_gate_eligible": false,
        "error_count": 23,
        "first_errors": [
          "inventory coverage/enforcement readiness cannot satisfy complete gate",
          "selected worker job conclusions missing/skipped/failed/cancelled",
          "duplicate/unexpected lane: None"
        ]
      },
      "lane_receipts": [
        {
          "artifact_id": 12037146583,
          "member": "static.policy/receipt.json",
          "lane_id": "static.policy",
          "profile": "static.policy",
          "receipt_kind": null,
          "source_identity": {
            "kind": "committed",
            "sha": "249f9657fb230947ba486fb3cd92894316f9acc3"
          },
          "job_conclusion": "success",
          "registration_audit": "passed",
          "input_audit": "passed",
          "complete_gate_eligible": false,
          "failure_stage": null,
          "failure_errors": [],
          "input_audit_gap_counts": {},
          "check_count": 1,
          "check_outcome_counts": {
            "passed": 1
          },
          "observed_execution_count": 1,
          "positive_checks": {
            "selection.contract": {
              "outcome": "passed",
              "observed_execution_count": 1
            }
          }
        },
        {
          "artifact_id": 12036653196,
          "member": "core.linux.asan/receipt.json",
          "lane_id": null,
          "profile": null,
          "receipt_kind": null,
          "source_identity": {
            "kind": "committed",
            "sha": "249f9657fb230947ba486fb3cd92894316f9acc3"
          },
          "job_conclusion": null,
          "registration_audit": null,
          "input_audit": null,
          "complete_gate_eligible": false,
          "failure_stage": null,
          "failure_errors": [],
          "input_audit_gap_counts": {},
          "check_count": 0,
          "check_outcome_counts": {},
          "observed_execution_count": 0,
          "positive_checks": {}
        },
        {
          "artifact_id": 12036349109,
          "member": "core.macos.asan/receipt.json",
          "lane_id": null,
          "profile": null,
          "receipt_kind": null,
          "source_identity": {
            "kind": "committed",
            "sha": "249f9657fb230947ba486fb3cd92894316f9acc3"
          },
          "job_conclusion": null,
          "registration_audit": null,
          "input_audit": null,
          "complete_gate_eligible": false,
          "failure_stage": null,
          "failure_errors": [],
          "input_audit_gap_counts": {},
          "check_count": 0,
          "check_outcome_counts": {},
          "observed_execution_count": 0,
          "positive_checks": {}
        },
        {
          "artifact_id": 12036264453,
          "member": "libgo.linux.asan/receipt.json",
          "lane_id": "libgo.linux.asan",
          "profile": "libgo.linux.asan",
          "receipt_kind": null,
          "source_identity": {
            "kind": "committed",
            "sha": "249f9657fb230947ba486fb3cd92894316f9acc3"
          },
          "job_conclusion": "success",
          "registration_audit": "passed",
          "input_audit": "passed",
          "complete_gate_eligible": false,
          "failure_stage": null,
          "failure_errors": [],
          "input_audit_gap_counts": {},
          "check_count": 2,
          "check_outcome_counts": {
            "passed": 2
          },
          "observed_execution_count": 33,
          "positive_checks": {
            "libgo.channel": {
              "outcome": "passed",
              "observed_execution_count": 1
            },
            "libgo.native_standalone": {
              "outcome": "passed",
              "observed_execution_count": 32
            }
          }
        },
        {
          "artifact_id": 12035779772,
          "member": "libgo.macos.asan/receipt.json",
          "lane_id": "libgo.macos.asan",
          "profile": "libgo.macos.asan",
          "receipt_kind": null,
          "source_identity": {
            "kind": "committed",
            "sha": "249f9657fb230947ba486fb3cd92894316f9acc3"
          },
          "job_conclusion": "success",
          "registration_audit": "passed",
          "input_audit": "passed",
          "complete_gate_eligible": false,
          "failure_stage": null,
          "failure_errors": [],
          "input_audit_gap_counts": {},
          "check_count": 2,
          "check_outcome_counts": {
            "passed": 2
          },
          "observed_execution_count": 32,
          "positive_checks": {
            "libgo.channel": {
              "outcome": "passed",
              "observed_execution_count": 1
            },
            "libgo.native_standalone": {
              "outcome": "passed",
              "observed_execution_count": 31
            }
          }
        }
      ],
      "artifacts": [
        {
          "artifact_id": 12038423327,
          "name": "dependency-validation-ef5fe08b9b15da727973302b5e13b6c79376062eba33b579d6d0b81da195425c-1",
          "expired": false,
          "metadata_size_bytes": 4894,
          "metadata_digest": "sha256:c09d2bf2bfc586edae4c43f0f24e3abab5445f1840d4aa1541b8c4e249a53a6e",
          "download_sha256": "c09d2bf2bfc586edae4c43f0f24e3abab5445f1840d4aa1541b8c4e249a53a6e",
          "download_size_bytes": 4894,
          "download_exit_code": 0,
          "role": "validation",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12038423327",
          "member_sha256": {
            "validation.json": "150999c9d3b3cb35679a3639cf7b97a47c3786cc096fe6b9bc6d7bb771922a3c"
          }
        },
        {
          "artifact_id": 12037146583,
          "name": "dependency-receipt-ef5fe08b9b15da727973302b5e13b6c79376062eba33b579d6d0b81da195425c-static.policy-1",
          "expired": false,
          "metadata_size_bytes": 20996,
          "metadata_digest": "sha256:3f01782ae7ea8c72a75af3869b1ae80e65eab7f947d31edfc612c2951dbb55d8",
          "download_sha256": "3f01782ae7ea8c72a75af3869b1ae80e65eab7f947d31edfc612c2951dbb55d8",
          "download_size_bytes": 20996,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12037146583",
          "member_sha256": {
            "static.policy/receipt.json": "62164fd907dd307e073170de6afb482a5db7799c67b24c7a907d54bb6e77a7eb",
            "static.policy/worker.log": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
          }
        },
        {
          "artifact_id": 12037010902,
          "name": "dependency-receipt-ef5fe08b9b15da727973302b5e13b6c79376062eba33b579d6d0b81da195425c-grotto.linux-1",
          "expired": false,
          "metadata_size_bytes": 2086,
          "metadata_digest": "sha256:364ce819dfe917d4c2adc4a36b18c312ee9d2e16ce129f05de80d0b56234d339",
          "download_sha256": "364ce819dfe917d4c2adc4a36b18c312ee9d2e16ce129f05de80d0b56234d339",
          "download_size_bytes": 2086,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12037010902",
          "member_sha256": {
            "grotto.linux/worker.log": "5f7348cbc88e7e0dcb44c275c7084fc300473f2e160c6aa9820743e807cdca65"
          }
        },
        {
          "artifact_id": 12036853940,
          "name": "dependency-receipt-ef5fe08b9b15da727973302b5e13b6c79376062eba33b579d6d0b81da195425c-groundhog.linux.sanitizers-1",
          "expired": false,
          "metadata_size_bytes": 244,
          "metadata_digest": "sha256:9514bfb9b49af672b8d5116d7107f41b9cba66ec53c0abd54c18b42de7533ec7",
          "download_sha256": "9514bfb9b49af672b8d5116d7107f41b9cba66ec53c0abd54c18b42de7533ec7",
          "download_size_bytes": 244,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12036853940",
          "member_sha256": {
            "groundhog.linux.sanitizers/worker.log": "e6d6319e1f3904807c413aec43dddcd5a935f6ec040b05d95fe02eb5e2a7582f"
          }
        },
        {
          "artifact_id": 12036847585,
          "name": "dependency-receipt-ef5fe08b9b15da727973302b5e13b6c79376062eba33b579d6d0b81da195425c-groundhog.mdk08-1",
          "expired": false,
          "metadata_size_bytes": 2111,
          "metadata_digest": "sha256:04fda5fab6ef6499c9d09525a58efb1dac10aae06ce7516c5724e8607dedf49d",
          "download_sha256": "04fda5fab6ef6499c9d09525a58efb1dac10aae06ce7516c5724e8607dedf49d",
          "download_size_bytes": 2111,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12036847585",
          "member_sha256": {
            "groundhog.mdk08/worker.log": "9b7df63ce591094a4d57151aec289394e68f59a778bd74003d21d2705d526898"
          }
        },
        {
          "artifact_id": 12036762746,
          "name": "dependency-receipt-ef5fe08b9b15da727973302b5e13b6c79376062eba33b579d6d0b81da195425c-core.linux-1",
          "expired": false,
          "metadata_size_bytes": 2076,
          "metadata_digest": "sha256:c37db5c991690742e643ab37dde00df0a61503c5315d4f99e0cd813fc628b9c1",
          "download_sha256": "c37db5c991690742e643ab37dde00df0a61503c5315d4f99e0cd813fc628b9c1",
          "download_size_bytes": 2076,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12036762746",
          "member_sha256": {
            "core.linux/worker.log": "f647884ad3f253de75cc3d4596809d501ed43d368edd514842168d95b85f890b"
          }
        },
        {
          "artifact_id": 12036653196,
          "name": "dependency-receipt-ef5fe08b9b15da727973302b5e13b6c79376062eba33b579d6d0b81da195425c-core.linux.asan-1",
          "expired": false,
          "metadata_size_bytes": 34641,
          "metadata_digest": "sha256:2c13708682ddefab42c9169de3cc334d19116bfd6279c37dc578a8abf91dcdc0",
          "download_sha256": "2c13708682ddefab42c9169de3cc334d19116bfd6279c37dc578a8abf91dcdc0",
          "download_size_bytes": 34641,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12036653196",
          "member_sha256": {
            "core.linux.asan/receipt.json": "4de4b3c65f559818d1afa46a5fa8c41662e4049f927b549ef6ce5557c0623202",
            "core.linux.asan/worker.log": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
          }
        },
        {
          "artifact_id": 12036638304,
          "name": "dependency-receipt-ef5fe08b9b15da727973302b5e13b6c79376062eba33b579d6d0b81da195425c-groundhog.linux-1",
          "expired": false,
          "metadata_size_bytes": 234,
          "metadata_digest": "sha256:5a8ca7e5958f48bf133b4436254a7a006c60fc9fed9b170d4d1eb5ba35cef176",
          "download_sha256": "5a8ca7e5958f48bf133b4436254a7a006c60fc9fed9b170d4d1eb5ba35cef176",
          "download_size_bytes": 234,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12036638304",
          "member_sha256": {
            "groundhog.linux/worker.log": "c3248197fc46c39acc93fb321661c79fa213f9f3ab8d6d7c86ed0ff9fb1d050e"
          }
        },
        {
          "artifact_id": 12036446528,
          "name": "dependency-plan-ef5fe08b9b15da727973302b5e13b6c79376062eba33b579d6d0b81da195425c-1",
          "expired": false,
          "metadata_size_bytes": 10774,
          "metadata_digest": "sha256:ed53641475f0bd33b30d52e2a25e49d8c6f42027d890f5e76d47697c3cc64285",
          "download_sha256": "ed53641475f0bd33b30d52e2a25e49d8c6f42027d890f5e76d47697c3cc64285",
          "download_size_bytes": 10774,
          "download_exit_code": 0,
          "role": "plan",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12036446528",
          "member_sha256": {
            "plan.json": "25aabe135c71bb69c86e16c1f28f441cdb2e489f21dec969cae158b866aa3c1a"
          }
        },
        {
          "artifact_id": 12036349109,
          "name": "dependency-receipt-ef5fe08b9b15da727973302b5e13b6c79376062eba33b579d6d0b81da195425c-core.macos.asan-1",
          "expired": false,
          "metadata_size_bytes": 34594,
          "metadata_digest": "sha256:59de5695381658d0f39da2accb1e6334ab20c22e684cb130eeb4a0c0315dc6dd",
          "download_sha256": "59de5695381658d0f39da2accb1e6334ab20c22e684cb130eeb4a0c0315dc6dd",
          "download_size_bytes": 34594,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12036349109",
          "member_sha256": {
            "core.macos.asan/receipt.json": "517976102c3525162898a9965c12bdcb3187b9e77771944d7ab2096d49f08a9a",
            "core.macos.asan/worker.log": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
          }
        },
        {
          "artifact_id": 12036264453,
          "name": "dependency-receipt-ef5fe08b9b15da727973302b5e13b6c79376062eba33b579d6d0b81da195425c-libgo.linux.asan-1",
          "expired": false,
          "metadata_size_bytes": 24300,
          "metadata_digest": "sha256:509b6f70a372c9f208d88c31e39607b1da260b28a4d0765be8604d3353a1d05e",
          "download_sha256": "509b6f70a372c9f208d88c31e39607b1da260b28a4d0765be8604d3353a1d05e",
          "download_size_bytes": 24300,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12036264453",
          "member_sha256": {
            "libgo.linux.asan/receipt.json": "2ac043618f0a468b08d1108927b9c4298994e12a2567bc8dd963b3122e823dd8",
            "libgo.linux.asan/worker.log": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
          }
        },
        {
          "artifact_id": 12035779772,
          "name": "dependency-receipt-ef5fe08b9b15da727973302b5e13b6c79376062eba33b579d6d0b81da195425c-libgo.macos.asan-1",
          "expired": false,
          "metadata_size_bytes": 27472,
          "metadata_digest": "sha256:a63a73ab8e0325c57d6393ac42b6ee0a0935417a2e518b14d2ba8faee33f4768",
          "download_sha256": "a63a73ab8e0325c57d6393ac42b6ee0a0935417a2e518b14d2ba8faee33f4768",
          "download_size_bytes": 27472,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12035779772",
          "member_sha256": {
            "libgo.macos.asan/receipt.json": "7b17538a1d3a4bcded8272d951e0e9a3823798b40093d20b13b6b2ea5e9cf2df",
            "libgo.macos.asan/worker.log": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
          }
        }
      ],
      "broad": {
        "nested_job_count": 47,
        "job_conclusion_counts": {
          "success": 41,
          "failure": 5,
          "skipped": 1
        },
        "accounting_job": {
          "id": 116915712972,
          "name": "broad / Broad family accounting",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116915712972"
        },
        "failed_jobs": [
          {
            "id": 116899992180,
            "name": "broad / bp_extended / nightly-matrix",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116899992180"
          },
          {
            "id": 116899992250,
            "name": "broad / bp / bp-matrix",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116899992250"
          },
          {
            "id": 116899992256,
            "name": "broad / groundhog / groundhog-sanitizers",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116899992256"
          },
          {
            "id": 116902162597,
            "name": "broad / fuzz / Fuzz json-event",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116902162597"
          },
          {
            "id": 116915712972,
            "name": "broad / Broad family accounting",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116915712972"
          }
        ],
        "non_success_jobs": [
          {
            "id": 116905942425,
            "name": "broad / fuzz / Generate coverage report",
            "conclusion": "skipped",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116905942425"
          }
        ]
      },
      "same_sha_repository_runs": {
        "query_returned": 17,
        "other_run_ids": [
          39111022109,
          39109895428,
          39109224867,
          39015855938,
          39014974619,
          38878691249,
          38878691293,
          38878691258,
          38878691277,
          38878691312,
          38878691304,
          38878691362,
          38878691863,
          38878691317,
          38878691295,
          38878691261
        ],
        "conclusion_counts": {
          "success": 15,
          "failure": 2
        },
        "pairing_review": "Not a PR comparison; same SHA alone does not prove matching ranges/profiles/required coverage."
      },
      "workflow_file_sha256": {
        ".github/workflows/dependency-tests.yml": "215dd788507eb2fff04dd3ed70193126006ec642bf949b3689fc8b1415e486e2",
        ".github/workflows/dependency-broad.yml": "79d5ce5af5f517d037b83fc5d4c92d3d31172a5ef059202e0a4b8ffe00d383b6"
      }
    },
    {
      "id": 39132650260,
      "run_attempt": 1,
      "event": "push",
      "head_sha": "2743f17354ac13527d8d8919f81abc711396c959",
      "head_branch": "master",
      "status": "completed",
      "conclusion": "failure",
      "created_at": "2026-10-18T13:41:27Z",
      "run_started_at": "2026-10-18T13:41:27Z",
      "updated_at": "2026-10-18T14:24:47Z",
      "html_url": "https://github.com/chebizarro/nostrc/actions/runs/39132650260",
      "workflow_id": 386168204,
      "path": ".github/workflows/dependency-tests.yml",
      "pull_request_numbers": [],
      "qualification": {
        "pr_comparison": "ineligible_event",
        "pr_reason": "No pull_request event/PR identity or reviewed legacy/selective pair; pushes/manual runs are not PR comparisons.",
        "broad_integration": "failed",
        "broad_reason": "Nested required Broad family accounting failed.",
        "accepted_pr_numbers": [],
        "accepted_broad": false,
        "required_workload_union_review": "not_accepted; accounting failure/absence is already disqualifying"
      },
      "job_count": 61,
      "job_conclusion_counts": {
        "success": 48,
        "failure": 12,
        "skipped": 1
      },
      "selection_and_worker_jobs": [
        {
          "id": 117690793121,
          "name": "select",
          "conclusion": "success",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117690793121"
        },
        {
          "id": 117691042406,
          "name": "grotto / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117691042406"
        },
        {
          "id": 117691042416,
          "name": "core_native / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117691042416"
        },
        {
          "id": 117691042489,
          "name": "groundhog / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117691042489"
        },
        {
          "id": 117691042494,
          "name": "mdk08 / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117691042494"
        },
        {
          "id": 117691042501,
          "name": "groundhog_sanitizers / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117691042501"
        },
        {
          "id": 117691042514,
          "name": "core / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117691042514"
        },
        {
          "id": 117691042549,
          "name": "grotto_asan / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117691042549"
        },
        {
          "id": 117691042569,
          "name": "policy / execute",
          "conclusion": "success",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117691042569"
        },
        {
          "id": 117691042625,
          "name": "core_asan / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117691042625"
        },
        {
          "id": 117691042628,
          "name": "libgo_native / execute",
          "conclusion": "success",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117691042628"
        },
        {
          "id": 117691042638,
          "name": "mdk011 / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117691042638"
        },
        {
          "id": 117691042658,
          "name": "libgo / execute",
          "conclusion": "success",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117691042658"
        },
        {
          "id": 117700054727,
          "name": "Dependency gate",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117700054727"
        }
      ],
      "gate_job": {
        "id": 117700054727,
        "name": "Dependency gate",
        "conclusion": "failure",
        "url": "https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117700054727"
      },
      "plan": {
        "plan_id": "3590a9cf34ef97302aaf6d04f2f65efbe66b884a0e5092f4f8ad606a7234485c",
        "inventory_hash": "c2ef7ee5281253cae2df2da3fb85a2ef83b8d4f7bb21c1ce20abf8d986bcd9ab",
        "context": "ci",
        "effective_mode": "broad",
        "source_identity": {
          "kind": "committed",
          "sha": "2743f17354ac13527d8d8919f81abc711396c959"
        },
        "expected_lane_ids": [
          "core.linux",
          "core.linux.asan",
          "core.macos.asan",
          "grotto.linux",
          "grotto.linux.asan_ubsan",
          "groundhog.linux",
          "groundhog.linux.sanitizers",
          "groundhog.mdk011",
          "groundhog.mdk08",
          "libgo.linux.asan",
          "libgo.macos.asan",
          "static.policy"
        ],
        "partial_profiles": [],
        "artifact_id": 12138098321
      },
      "validation": {
        "artifact_id": 12140195214,
        "member": "validation.json",
        "tested_sha": "2743f17354ac13527d8d8919f81abc711396c959",
        "passed": false,
        "complete_requested": true,
        "complete_gate_eligible": false,
        "error_count": 467,
        "first_errors": [
          "inventory coverage/enforcement readiness cannot satisfy complete gate",
          "selected worker job conclusions missing/skipped/failed/cancelled",
          "core.linux: lane failure at configure"
        ]
      },
      "lane_receipts": [
        {
          "artifact_id": 12139515154,
          "member": "static.policy/receipt.json",
          "lane_id": "static.policy",
          "profile": "static.policy",
          "receipt_kind": null,
          "source_identity": {
            "kind": "committed",
            "sha": "2743f17354ac13527d8d8919f81abc711396c959"
          },
          "job_conclusion": "success",
          "registration_audit": "passed",
          "input_audit": "passed",
          "complete_gate_eligible": false,
          "failure_stage": null,
          "failure_errors": [],
          "input_audit_gap_counts": {},
          "check_count": 1,
          "check_outcome_counts": {
            "passed": 1
          },
          "observed_execution_count": 1,
          "positive_checks": {
            "selection.contract": {
              "outcome": "passed",
              "observed_execution_count": 1
            }
          }
        },
        {
          "artifact_id": 12139196110,
          "member": "core.linux/receipt.json",
          "lane_id": "core.linux",
          "profile": "core.linux",
          "receipt_kind": "lane_failure_v1",
          "source_identity": {
            "kind": "committed",
            "sha": "2743f17354ac13527d8d8919f81abc711396c959"
          },
          "job_conclusion": "failure",
          "registration_audit": "not_performed",
          "input_audit": "not_completed",
          "complete_gate_eligible": false,
          "failure_stage": "configure",
          "failure_errors": [
            {
              "message": "profile build type Debug differs from realized ''",
              "type": "RealizationError"
            }
          ],
          "input_audit_gap_counts": {},
          "check_count": 3,
          "check_outcome_counts": {
            "missing": 3
          },
          "observed_execution_count": 0,
          "positive_checks": {}
        },
        {
          "artifact_id": 12139046572,
          "member": "groundhog.linux.sanitizers/receipt.json",
          "lane_id": "groundhog.linux.sanitizers",
          "profile": "groundhog.linux.sanitizers",
          "receipt_kind": "lane_failure_v1",
          "source_identity": {
            "kind": "committed",
            "sha": "2743f17354ac13527d8d8919f81abc711396c959"
          },
          "job_conclusion": "failure",
          "registration_audit": "not_performed",
          "input_audit": "not_completed",
          "complete_gate_eligible": false,
          "failure_stage": "resolve",
          "failure_errors": [
            {
              "message": "diagnostic nonmember fresh registration partition differs",
              "type": "RealizationError"
            }
          ],
          "input_audit_gap_counts": {},
          "check_count": 62,
          "check_outcome_counts": {
            "missing": 62
          },
          "observed_execution_count": 0,
          "positive_checks": {}
        },
        {
          "artifact_id": 12138776972,
          "member": "libgo.macos.asan/receipt.json",
          "lane_id": "libgo.macos.asan",
          "profile": "libgo.macos.asan",
          "receipt_kind": null,
          "source_identity": {
            "kind": "committed",
            "sha": "2743f17354ac13527d8d8919f81abc711396c959"
          },
          "job_conclusion": "success",
          "registration_audit": "passed",
          "input_audit": "passed",
          "complete_gate_eligible": false,
          "failure_stage": null,
          "failure_errors": [],
          "input_audit_gap_counts": {},
          "check_count": 2,
          "check_outcome_counts": {
            "passed": 2
          },
          "observed_execution_count": 32,
          "positive_checks": {
            "libgo.channel": {
              "outcome": "passed",
              "observed_execution_count": 1
            },
            "libgo.native_standalone": {
              "outcome": "passed",
              "observed_execution_count": 31
            }
          }
        },
        {
          "artifact_id": 12138657091,
          "member": "groundhog.mdk011/receipt.json",
          "lane_id": "groundhog.mdk011",
          "profile": "groundhog.mdk011",
          "receipt_kind": "lane_failure_v1",
          "source_identity": {
            "kind": "committed",
            "sha": "2743f17354ac13527d8d8919f81abc711396c959"
          },
          "job_conclusion": "failure",
          "registration_audit": "not_performed",
          "input_audit": "not_completed",
          "complete_gate_eligible": false,
          "failure_stage": "capability",
          "failure_errors": [
            {
              "message": "live peer/image lifecycle requires the existing guarded adapter",
              "type": "CheckFailure"
            }
          ],
          "input_audit_gap_counts": {},
          "check_count": 16,
          "check_outcome_counts": {
            "missing": 16
          },
          "observed_execution_count": 0,
          "positive_checks": {}
        },
        {
          "artifact_id": 12138638626,
          "member": "core.linux.asan/receipt.json",
          "lane_id": "core.linux.asan",
          "profile": "core.linux.asan",
          "receipt_kind": "lane_failure_v1",
          "source_identity": {
            "kind": "committed",
            "sha": "2743f17354ac13527d8d8919f81abc711396c959"
          },
          "job_conclusion": "failure",
          "registration_audit": "failed",
          "input_audit": "failed",
          "complete_gate_eligible": false,
          "failure_stage": "preaudit",
          "failure_errors": [
            {
              "message": "pre-build input audit failed; original findings retained",
              "type": "CheckFailure"
            }
          ],
          "input_audit_gap_counts": {
            "unclaimed_registration": 3
          },
          "check_count": 48,
          "check_outcome_counts": {
            "missing": 48
          },
          "observed_execution_count": 0,
          "positive_checks": {}
        },
        {
          "artifact_id": 12138633221,
          "member": "groundhog.linux/receipt.json",
          "lane_id": "groundhog.linux",
          "profile": "groundhog.linux",
          "receipt_kind": "lane_failure_v1",
          "source_identity": {
            "kind": "committed",
            "sha": "2743f17354ac13527d8d8919f81abc711396c959"
          },
          "job_conclusion": "failure",
          "registration_audit": "failed",
          "input_audit": "failed",
          "complete_gate_eligible": false,
          "failure_stage": "preaudit",
          "failure_errors": [
            {
              "message": "pre-build input audit failed; original findings retained",
              "type": "CheckFailure"
            }
          ],
          "input_audit_gap_counts": {
            "unclaimed_registration": 408,
            "missing_impact_edge": 9473,
            "check_descriptor_owner_disagreement": 1,
            "target_mapping": 1
          },
          "check_count": 10,
          "check_outcome_counts": {
            "missing": 10
          },
          "observed_execution_count": 0,
          "positive_checks": {}
        },
        {
          "artifact_id": 12138577637,
          "member": "grotto.linux.asan_ubsan/receipt.json",
          "lane_id": "grotto.linux.asan_ubsan",
          "profile": "grotto.linux.asan_ubsan",
          "receipt_kind": "lane_failure_v1",
          "source_identity": {
            "kind": "committed",
            "sha": "2743f17354ac13527d8d8919f81abc711396c959"
          },
          "job_conclusion": "failure",
          "registration_audit": "failed",
          "input_audit": "failed",
          "complete_gate_eligible": false,
          "failure_stage": "preaudit",
          "failure_errors": [
            {
              "message": "pre-build input audit failed; original findings retained",
              "type": "CheckFailure"
            }
          ],
          "input_audit_gap_counts": {
            "unclaimed_registration": 322,
            "target_mapping": 1,
            "missing_impact_edge": 330,
            "generated_runtime_without_target": 2
          },
          "check_count": 4,
          "check_outcome_counts": {
            "missing": 4
          },
          "observed_execution_count": 0,
          "positive_checks": {}
        },
        {
          "artifact_id": 12138467933,
          "member": "groundhog.mdk08/receipt.json",
          "lane_id": "groundhog.mdk08",
          "profile": "groundhog.mdk08",
          "receipt_kind": "lane_failure_v1",
          "source_identity": {
            "kind": "committed",
            "sha": "2743f17354ac13527d8d8919f81abc711396c959"
          },
          "job_conclusion": "failure",
          "registration_audit": "not_performed",
          "input_audit": "not_completed",
          "complete_gate_eligible": false,
          "failure_stage": "configure",
          "failure_errors": [
            {
              "message": "profile build type Debug differs from realized ''",
              "type": "RealizationError"
            }
          ],
          "input_audit_gap_counts": {},
          "check_count": 1,
          "check_outcome_counts": {
            "missing": 1
          },
          "observed_execution_count": 0,
          "positive_checks": {}
        },
        {
          "artifact_id": 12138424589,
          "member": "grotto.linux/receipt.json",
          "lane_id": "grotto.linux",
          "profile": "grotto.linux",
          "receipt_kind": "lane_failure_v1",
          "source_identity": {
            "kind": "committed",
            "sha": "2743f17354ac13527d8d8919f81abc711396c959"
          },
          "job_conclusion": "failure",
          "registration_audit": "failed",
          "input_audit": "failed",
          "complete_gate_eligible": false,
          "failure_stage": "preaudit",
          "failure_errors": [
            {
              "message": "pre-build input audit failed; original findings retained",
              "type": "CheckFailure"
            }
          ],
          "input_audit_gap_counts": {
            "unclaimed_registration": 402,
            "missing_impact_edge": 9104,
            "generated_runtime_without_target": 2
          },
          "check_count": 4,
          "check_outcome_counts": {
            "missing": 4
          },
          "observed_execution_count": 0,
          "positive_checks": {}
        },
        {
          "artifact_id": 12138414686,
          "member": "libgo.linux.asan/receipt.json",
          "lane_id": "libgo.linux.asan",
          "profile": "libgo.linux.asan",
          "receipt_kind": null,
          "source_identity": {
            "kind": "committed",
            "sha": "2743f17354ac13527d8d8919f81abc711396c959"
          },
          "job_conclusion": "success",
          "registration_audit": "passed",
          "input_audit": "passed",
          "complete_gate_eligible": false,
          "failure_stage": null,
          "failure_errors": [],
          "input_audit_gap_counts": {},
          "check_count": 2,
          "check_outcome_counts": {
            "passed": 2
          },
          "observed_execution_count": 33,
          "positive_checks": {
            "libgo.channel": {
              "outcome": "passed",
              "observed_execution_count": 1
            },
            "libgo.native_standalone": {
              "outcome": "passed",
              "observed_execution_count": 32
            }
          }
        },
        {
          "artifact_id": 12138179654,
          "member": "core.macos.asan/receipt.json",
          "lane_id": "core.macos.asan",
          "profile": "core.macos.asan",
          "receipt_kind": "lane_failure_v1",
          "source_identity": {
            "kind": "committed",
            "sha": "2743f17354ac13527d8d8919f81abc711396c959"
          },
          "job_conclusion": "failure",
          "registration_audit": "failed",
          "input_audit": "failed",
          "complete_gate_eligible": false,
          "failure_stage": "preaudit",
          "failure_errors": [
            {
              "message": "pre-build input audit failed; original findings retained",
              "type": "CheckFailure"
            }
          ],
          "input_audit_gap_counts": {
            "unclaimed_registration": 3
          },
          "check_count": 48,
          "check_outcome_counts": {
            "missing": 48
          },
          "observed_execution_count": 0,
          "positive_checks": {}
        }
      ],
      "artifacts": [
        {
          "artifact_id": 12140195214,
          "name": "dependency-validation-3590a9cf34ef97302aaf6d04f2f65efbe66b884a0e5092f4f8ad606a7234485c-1",
          "expired": false,
          "metadata_size_bytes": 510512,
          "metadata_digest": "sha256:a190e29a0e3e242e9125db7d8a905c13c1bd325e3a84c1b28d697e988ae10f4a",
          "download_sha256": "a190e29a0e3e242e9125db7d8a905c13c1bd325e3a84c1b28d697e988ae10f4a",
          "download_size_bytes": 510512,
          "download_exit_code": 0,
          "role": "validation",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12140195214",
          "member_sha256": {
            "validation.json": "fb2a85ee6e84a1242ed87a7b89c5a4d8aac31833000a31235661516cd237ab05"
          }
        },
        {
          "artifact_id": 12139515154,
          "name": "dependency-receipt-3590a9cf34ef97302aaf6d04f2f65efbe66b884a0e5092f4f8ad606a7234485c-static.policy-1",
          "expired": false,
          "metadata_size_bytes": 22105,
          "metadata_digest": "sha256:763fb1481bbfe61469699a0578260dd23f8dad63470a80680058081e430d71c2",
          "download_sha256": "763fb1481bbfe61469699a0578260dd23f8dad63470a80680058081e430d71c2",
          "download_size_bytes": 22105,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12139515154",
          "member_sha256": {
            "static.policy/receipt.json": "6979128aa356ff102413ef0385e1703904f23d54e8cd91d1f5c5ee82091518a6",
            "static.policy/worker.log": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
          }
        },
        {
          "artifact_id": 12139196110,
          "name": "dependency-receipt-3590a9cf34ef97302aaf6d04f2f65efbe66b884a0e5092f4f8ad606a7234485c-core.linux-1",
          "expired": false,
          "metadata_size_bytes": 5628,
          "metadata_digest": "sha256:2320fef83a4af6543ac7074d5b1fa118917434bb19f42d822f672210770ec938",
          "download_sha256": "2320fef83a4af6543ac7074d5b1fa118917434bb19f42d822f672210770ec938",
          "download_size_bytes": 5628,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12139196110",
          "member_sha256": {
            "core.linux/receipt.json": "a08b10d71c8e83de07d668962a72cbe7e2718bdd1c8d7d3b4bda38c7d4ff1ddf",
            "core.linux/worker.log": "2709cb6f0bb486e513f02d6fb2b573e9f9885baa3268e8daf66e72594d85fe5f"
          }
        },
        {
          "artifact_id": 12139046572,
          "name": "dependency-receipt-3590a9cf34ef97302aaf6d04f2f65efbe66b884a0e5092f4f8ad606a7234485c-groundhog.linux.sanitizers-1",
          "expired": false,
          "metadata_size_bytes": 7379,
          "metadata_digest": "sha256:9d827bacb3ca61d0d5dc518c0330c59a9e0f0f2ca738404ba9ad33d492e94dfe",
          "download_sha256": "9d827bacb3ca61d0d5dc518c0330c59a9e0f0f2ca738404ba9ad33d492e94dfe",
          "download_size_bytes": 7379,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12139046572",
          "member_sha256": {
            "groundhog.linux.sanitizers/receipt.json": "47dd5a471b5a2fcaee9906e74145f6714c491b4294c3fe14067a1ebdeaaac11d",
            "groundhog.linux.sanitizers/worker.log": "389e76c63f8828b5a7a5f6faf9587d06aff79d72a44ecab7151c0eaac496496c"
          }
        },
        {
          "artifact_id": 12138776972,
          "name": "dependency-receipt-3590a9cf34ef97302aaf6d04f2f65efbe66b884a0e5092f4f8ad606a7234485c-libgo.macos.asan-1",
          "expired": false,
          "metadata_size_bytes": 27753,
          "metadata_digest": "sha256:e47302b122455ecd016df8ede806437758edf4e6799657c7297430d397647c51",
          "download_sha256": "e47302b122455ecd016df8ede806437758edf4e6799657c7297430d397647c51",
          "download_size_bytes": 27753,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12138776972",
          "member_sha256": {
            "libgo.macos.asan/receipt.json": "fcca8e3d3c6bda0a7aad8ae354a687efc6b654bcdea6f29d60f9338c94d76ae5",
            "libgo.macos.asan/worker.log": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
          }
        },
        {
          "artifact_id": 12138657091,
          "name": "dependency-receipt-3590a9cf34ef97302aaf6d04f2f65efbe66b884a0e5092f4f8ad606a7234485c-groundhog.mdk011-1",
          "expired": false,
          "metadata_size_bytes": 16372,
          "metadata_digest": "sha256:47281329eeb1a3f4843234f43b30e650bb77da6493f196d6b4cc8b5190ffbf30",
          "download_sha256": "47281329eeb1a3f4843234f43b30e650bb77da6493f196d6b4cc8b5190ffbf30",
          "download_size_bytes": 16372,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12138657091",
          "member_sha256": {
            "groundhog.mdk011/receipt.json": "72c417b73ca074e05cb1ed9d63912be2623c9fa9c8c9c2dcff59f4e057d69274",
            "groundhog.mdk011/worker.log": "c3248197fc46c39acc93fb321661c79fa213f9f3ab8d6d7c86ed0ff9fb1d050e"
          }
        },
        {
          "artifact_id": 12138638626,
          "name": "dependency-receipt-3590a9cf34ef97302aaf6d04f2f65efbe66b884a0e5092f4f8ad606a7234485c-core.linux.asan-1",
          "expired": false,
          "metadata_size_bytes": 57851,
          "metadata_digest": "sha256:43cb9d13ea132282b4a2933daa5282406f0901b1e37203574cd2dac0f1a8091c",
          "download_sha256": "43cb9d13ea132282b4a2933daa5282406f0901b1e37203574cd2dac0f1a8091c",
          "download_size_bytes": 57851,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12138638626",
          "member_sha256": {
            "core.linux.asan/receipt.json": "63f8a77e5c63d61cdc6bca17fe5b1e97ce3448af10cacfba58e40cef6ea05672",
            "core.linux.asan/worker.log": "173430c24f2ae508d76f8d596ffe08f3a12825f6f07420e0018ddb2d32374e15"
          }
        },
        {
          "artifact_id": 12138633221,
          "name": "dependency-receipt-3590a9cf34ef97302aaf6d04f2f65efbe66b884a0e5092f4f8ad606a7234485c-groundhog.linux-1",
          "expired": false,
          "metadata_size_bytes": 176345,
          "metadata_digest": "sha256:3764ed320d7ceb848ff59b66762996de548a7105844e91793071e5502ba90517",
          "download_sha256": "3764ed320d7ceb848ff59b66762996de548a7105844e91793071e5502ba90517",
          "download_size_bytes": 176345,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12138633221",
          "member_sha256": {
            "groundhog.linux/receipt.json": "3cc5042f5821ce8db35bcc0e7d156acb7b47112be26f2e80bd924e1c9c90d1e5",
            "groundhog.linux/worker.log": "173430c24f2ae508d76f8d596ffe08f3a12825f6f07420e0018ddb2d32374e15"
          }
        },
        {
          "artifact_id": 12138577637,
          "name": "dependency-receipt-3590a9cf34ef97302aaf6d04f2f65efbe66b884a0e5092f4f8ad606a7234485c-grotto.linux.asan_ubsan-1",
          "expired": false,
          "metadata_size_bytes": 96535,
          "metadata_digest": "sha256:190646958bf1e1a1d77e0d8dc07cdca8583c49d0b13a2693a14723f544aa7565",
          "download_sha256": "190646958bf1e1a1d77e0d8dc07cdca8583c49d0b13a2693a14723f544aa7565",
          "download_size_bytes": 96535,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12138577637",
          "member_sha256": {
            "grotto.linux.asan_ubsan/receipt.json": "4d04dca81a81f990d3607449c63a41ccb9d2f146d421d2ed93b6ce81f4c96e22",
            "grotto.linux.asan_ubsan/worker.log": "173430c24f2ae508d76f8d596ffe08f3a12825f6f07420e0018ddb2d32374e15"
          }
        },
        {
          "artifact_id": 12138467933,
          "name": "dependency-receipt-3590a9cf34ef97302aaf6d04f2f65efbe66b884a0e5092f4f8ad606a7234485c-groundhog.mdk08-1",
          "expired": false,
          "metadata_size_bytes": 5588,
          "metadata_digest": "sha256:87797abab8f814def91e83cfe376fbfc30fd7daa2ddb1e52814a5df76b599b0b",
          "download_sha256": "87797abab8f814def91e83cfe376fbfc30fd7daa2ddb1e52814a5df76b599b0b",
          "download_size_bytes": 5588,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12138467933",
          "member_sha256": {
            "groundhog.mdk08/receipt.json": "d8b494cd11f97f3a0b4063ec4888a8b63c48c4a4299475c4e494e70b437e9a67",
            "groundhog.mdk08/worker.log": "2709cb6f0bb486e513f02d6fb2b573e9f9885baa3268e8daf66e72594d85fe5f"
          }
        },
        {
          "artifact_id": 12138424589,
          "name": "dependency-receipt-3590a9cf34ef97302aaf6d04f2f65efbe66b884a0e5092f4f8ad606a7234485c-grotto.linux-1",
          "expired": false,
          "metadata_size_bytes": 192926,
          "metadata_digest": "sha256:1d16caad632dc3fa33a2719d34b78357f98026007dbc43d15e45f6ff8d3957d9",
          "download_sha256": "1d16caad632dc3fa33a2719d34b78357f98026007dbc43d15e45f6ff8d3957d9",
          "download_size_bytes": 192926,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12138424589",
          "member_sha256": {
            "grotto.linux/receipt.json": "07b0649aba3cd9d0039b1473cfa0882b3a88250596f33a8e68455dacd12c95e2",
            "grotto.linux/worker.log": "173430c24f2ae508d76f8d596ffe08f3a12825f6f07420e0018ddb2d32374e15"
          }
        },
        {
          "artifact_id": 12138414686,
          "name": "dependency-receipt-3590a9cf34ef97302aaf6d04f2f65efbe66b884a0e5092f4f8ad606a7234485c-libgo.linux.asan-1",
          "expired": false,
          "metadata_size_bytes": 24613,
          "metadata_digest": "sha256:206cf7385d089a737bf6f583d0de7eb862af46b068e0a0c82d96c22ba695adac",
          "download_sha256": "206cf7385d089a737bf6f583d0de7eb862af46b068e0a0c82d96c22ba695adac",
          "download_size_bytes": 24613,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12138414686",
          "member_sha256": {
            "libgo.linux.asan/receipt.json": "82a68ee215f72015ef1e2900904eccc23e88b3c5ea37aaaa0fefee59d8c584b8",
            "libgo.linux.asan/worker.log": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
          }
        },
        {
          "artifact_id": 12138179654,
          "name": "dependency-receipt-3590a9cf34ef97302aaf6d04f2f65efbe66b884a0e5092f4f8ad606a7234485c-core.macos.asan-1",
          "expired": false,
          "metadata_size_bytes": 57895,
          "metadata_digest": "sha256:3d82cbe3544f555a0bc4117ceab170a7077f1320b522bbf2bdedb18762d673e2",
          "download_sha256": "3d82cbe3544f555a0bc4117ceab170a7077f1320b522bbf2bdedb18762d673e2",
          "download_size_bytes": 57895,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12138179654",
          "member_sha256": {
            "core.macos.asan/receipt.json": "624a133e74ccb1aab18940a4c2c1036757dad9c134d5c4ae6e331c61af82b513",
            "core.macos.asan/worker.log": "173430c24f2ae508d76f8d596ffe08f3a12825f6f07420e0018ddb2d32374e15"
          }
        },
        {
          "artifact_id": 12138098321,
          "name": "dependency-plan-3590a9cf34ef97302aaf6d04f2f65efbe66b884a0e5092f4f8ad606a7234485c-1",
          "expired": false,
          "metadata_size_bytes": 9147,
          "metadata_digest": "sha256:1d0e7ad8d510aaa5f105d61b2c8992691240d52961715b1f3829383e99628703",
          "download_sha256": "1d0e7ad8d510aaa5f105d61b2c8992691240d52961715b1f3829383e99628703",
          "download_size_bytes": 9147,
          "download_exit_code": 0,
          "role": "plan",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12138098321",
          "member_sha256": {
            "plan.json": "b147c6654944cca6c7ee85d2a9383695199aecacd628f5933ef287619b7a185b"
          }
        }
      ],
      "broad": {
        "nested_job_count": 47,
        "job_conclusion_counts": {
          "success": 44,
          "failure": 2,
          "skipped": 1
        },
        "accounting_job": {
          "id": 117700034296,
          "name": "broad / Broad family accounting",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117700034296"
        },
        "failed_jobs": [
          {
            "id": 117691683008,
            "name": "broad / fuzz / Fuzz json-event",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117691683008"
          },
          {
            "id": 117700034296,
            "name": "broad / Broad family accounting",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117700034296"
          }
        ],
        "non_success_jobs": [
          {
            "id": 117694708765,
            "name": "broad / fuzz / Generate coverage report",
            "conclusion": "skipped",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117694708765"
          }
        ]
      },
      "same_sha_repository_runs": {
        "query_returned": 12,
        "other_run_ids": [
          39132650008,
          39132649987,
          39132650081,
          39132650000,
          39132649950,
          39132650144,
          39132650026,
          39132650266,
          39132650074,
          39132650119,
          39132650079
        ],
        "conclusion_counts": {
          "success": 11,
          "failure": 1
        },
        "pairing_review": "Not a PR comparison; same SHA alone does not prove matching ranges/profiles/required coverage."
      },
      "workflow_file_sha256": {
        ".github/workflows/dependency-tests.yml": "a18010ceab7be2d96632ef8266c24fcb2e71e970dbdb4c09905ad17265f45f15",
        ".github/workflows/dependency-broad.yml": "79d5ce5af5f517d037b83fc5d4c92d3d31172a5ef059202e0a4b8ffe00d383b6"
      }
    }
  ],
  "artifact_integrity": {
    "archives_checked": 59,
    "published_digests_matching": 59,
    "crc_failures": 0
  },
  "latest_broad_failure_log": {
    "run_id": 39132650260,
    "job_id": 117691683008,
    "url": "https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117691683008",
    "raw_log_sha256": "ce24a08fc9b1b4fef01091d8ce4f9878f44fca74c46c86681e36f04bc2f89d7f",
    "observed": "LeakSanitizer514 bytes/15 allocations through json_parser_load_from_data and fuzz_json_event.c:55; exit77. Input leak-7396472d70f69483739531904dd1db7271706023.",
    "cause_limit": "Actual observed call path, not a newly established upstream root cause or remedy.",
    "coverage_report_job": {
      "id": 117694708765,
      "conclusion": "skipped",
      "not_a_pass": true
    }
  }
}
```
<!-- acceptance-ledger-json:end -->
