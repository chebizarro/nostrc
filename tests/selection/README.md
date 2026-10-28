# Selection regression suites

Ordinary CI runs `python3 scripts/test-selection-operational.py`. The manual
`.github/workflows/namespace-policy-regression.yml` route still runs full discovery.
`selection.contract` retains the explicit diagnostic source-input closure; its
legacy adapter runs the `test_selector*.py` pattern. Other modules are also directly
invocable. Source-input enrollment is not proof that a module ran.

```sh
PYTHONPATH=tests/selection python3 -m unittest <module>
python3 scripts/test-selection-operational.py
PYTHONPATH=tests/selection python3 -m unittest discover -s tests/selection -v
```

## Cleanup inventory (nostrc-80m1f, including nostrc-v6u8g)

Baseline at `686796cf8`: **124 test modules: 66 passing, 58 failing**. The original
operational runner passed **182 tests in 13 modules**. Raw per-module commands used
`PYTHONPATH=tests/selection`; the first causal error is recorded below. The worktree
initially lacked pinned submodule checkouts, which were materialized before final
manual verification. Import-path and inherited-PYTHONPATH failures were invocation
problems, not new protocol or generator behavior.

Disposition: **16 modules retired; 42 previously failing modules repaired;**
**109 remaining modules, including one new consolidated regression module.**
The operational list represents **40 modules** (was 13), with
**10 explicitly selected classes** in mixed modules. Full manual discovery
continues to include the other classes. Platform/capability skips are reported by
unittest and are not native, hosted, production, signing, or release acceptance.

### Contract boundaries

- The exact NIP-65 historical inverse also removes its recorded
  `selection.contract` TestInventory input from commit `73e8bc7fc`; index/value,
  idempotence and refusal checks remain. It is never production admission.
- Current membership tests read current inventory directly. Obsolete whole-policy
  and source-byte inverse chains were retired, not repeatedly re-pinned to whatever
  the checkout happened to contain. Retained exports are regression data, not fresh
  current-source or execution attestations. Existing stale native review records
  remain rejected; cleanup does not renew their authority.
- Current workflow job hashes and missing job records are accounted explicitly.
  Existing coverage gaps and unsupported reasons remain incomplete. Accounting a
  normal runner or orchestration job does not make it an attested native profile.
- The macOS NIP-5F nonvalidation binding is renewed only for the current registration
  file and legacy workflow envelope. The TCP source and descriptor bytes are
  unchanged; BUILD_APPS=OFF, sanitizer, signer-environment, exact-registration,
  prelaunch and source-drift refusal tests remain. Linux-only registration gating
  does not change that macOS branch. No exclusion becomes signing or pass credit.
- Static checkout still needs no submodules for its enrolled classes. Full retained
  native source-closure classes stay on direct/manual discovery with exact pinned
  Gitlinks. The namespace diagnostic checkout retains `submodules: recursive`.
- No workflow YAML behavior changes. No independently releasable component is
  affected: **no version bump** for any manifest component.

### Per-module disposition

`CI` means enrolled in the ordinary runner. `manual-only` is intentional, not an
unrecorded omission. Retired rows name the current replacement coverage. The final
executed results and commit identities are recorded in bead nostrc-80m1f.

| Module | Before / first error | Disposition | Reason |
| --- | --- | --- | --- |
| `test_adapter_custody` | pass | manual-only | Extended host/service/custody lifecycle diagnostic; platform/kernel/resource cells remain explicit manual work. |
| `test_aggregation` | pass | manual-only | Extended attestation/receipt/profile qualification or nonvalidation matrix; remains explicitly invoked. |
| `test_bp_metrics_reporting` | pass | manual-only | Retained workload/metrics reporting matrix; synthetic parser evidence is not a benchmark or profile qualification. |
| `test_broad_setup_contracts` | pass | manual-only | Extended source/enrollment or native-descriptor matrix; current ordinary entrypoints have bounded operational regressions. |
| `test_bud02_registration_rename` | pass | manual-only | Extended source/enrollment or native-descriptor matrix; current ordinary entrypoints have bounded operational regressions. |
| `test_ci_selected_execution` | pass | CI: whole module | Current selector/workflow/runner regression; no historical inverse or custody qualification. |
| `test_closeout_interfaces` | fail: test_selection.ContractError: W11 batch2 integration source changed: .github/workflows/dependency-worker.yml | retired | Superseded closeout inverse snapshot; live default-build, setup and capability behavior is covered by test_command_execution, test_worker_signer_prerequisites and test_workflow_closeout. |
| `test_closeout_producers` | fail: test_selection.ContractError: groundhog cmake source changed: gnome/groundhog/CMakeLists.txt | CI: whole module | Current selector/workflow/runner regression; no historical inverse or custody qualification. |
| `test_cmake_inventory_compat` | pass | CI: whole module | Current selector/workflow/runner regression; no historical inverse or custody qualification. |
| `test_command_execution` | pass | manual-only | Extended native build/input-audit/process matrix; bounded current CLI/runner checks are in the operational suite. |
| `test_command_output_bytes` | pass | manual-only | Extended native build/input-audit/process matrix; bounded current CLI/runner checks are in the operational suite. |
| `test_command_signal_bytes` | pass | manual-only | Extended native build/input-audit/process matrix; bounded current CLI/runner checks are in the operational suite. |
| `test_configure_outputs` | pass | manual-only | Extended native build/input-audit/process matrix; bounded current CLI/runner checks are in the operational suite. |
| `test_contained_custody_contract` | fail: AssertionError: 18 != 13 | manual-only | Contained-domain custody/refusal matrix; requires the separate custody environment for positive execution. |
| `test_core_source_producers` | fail: test_selection.ContractError: closeout selected closure changed or non-selection authority changed: groundhog.linux | CI: whole module | Current selector/workflow/runner regression; no historical inverse or custody qualification. |
| `test_coreasan_app_enrollment` | fail: AssertionError: 'b4a72885e8b0d3b66474ac6aca33e6984cb2e48fd653459e177c0ef444545655' != '8005399d706f7e43fe43984ae58be767574a03c7fb2ba441655e3c2d9861ce5 | retired | Replaced obsolete whole-policy enrollment inverse with test_current_app_membership: current unique claims, peer memberships, capabilities, default app/NIP65 nodes and legacy ASan exclusions. |
| `test_ctest_runtime_inputs` | pass | manual-only | Extended native build/input-audit/process matrix; bounded current CLI/runner checks are in the operational suite. |
| `test_current_app_membership` | new | CI: whole module | Consolidated current app ownership, peer claims, capability requirements, default nodes and legacy ASan exclusions. |
| `test_darwin_boundary_contract` | pass | manual-only | Extended host/service/custody lifecycle diagnostic; platform/kernel/resource cells remain explicit manual work. |
| `test_darwin_custody` | pass | manual-only | Extended host/service/custody lifecycle diagnostic; platform/kernel/resource cells remain explicit manual work. |
| `test_darwin_gate_wiring` | pass | manual-only | Extended host/service/custody lifecycle diagnostic; platform/kernel/resource cells remain explicit manual work. |
| `test_darwin_private_bus` | pass | manual-only | Extended host/service/custody lifecycle diagnostic; platform/kernel/resource cells remain explicit manual work. |
| `test_default_producers` | fail: AssertionError: [{'kind': 'unproven_default_custom_producer', 'output': 'label.ui', 'reason': 'generator Python/import path override is not admitted'} | CI: whole module | Current selector/workflow/runner regression; no historical inverse or custody qualification. |
| `test_diagnostic_eligibility` | pass | manual-only | Extended attestation/receipt/profile qualification or nonvalidation matrix; remains explicitly invoked. |
| `test_diagnostic_nonmembers` | pass | manual-only | Extended attestation/receipt/profile qualification or nonvalidation matrix; remains explicitly invoked. |
| `test_encrypted_media_membership` | fail: test_selection.ContractError: native custody command input enrollment changed | CI: `EncryptedMediaMembership` | Other classes remain manual-only: retained native source/export/profile matrix; pinned Gitlink sources where required. |
| `test_execution_parallelism` | pass | manual-only | Extended native build/input-audit/process matrix; bounded current CLI/runner checks are in the operational suite. |
| `test_failure_receipts` | pass | manual-only | Extended attestation/receipt/profile qualification or nonvalidation matrix; remains explicitly invoked. |
| `test_fuzz_json_glib_setup` | fail: ValueError: unaccounted/stale workflow jobs: ['.github/workflows/ci-selective.yml:appimage', '.github/workflows/ci-selective.yml:bp', '.github/workflo | CI: `FuzzJsonGlibSetupTests` | Historical enrollment/golden-refusal class remains manual-only. |
| `test_git_ranges` | pass | CI: whole module | Current selector/workflow/runner regression; no historical inverse or custody qualification. |
| `test_grotto_crypto_membership` | fail: test_selection.ContractError: native custody command input enrollment changed | CI: `GrottoCryptoMembership` | Other classes remain manual-only: retained native source/export/profile matrix; pinned Gitlink sources where required. |
| `test_grotto_nip49_membership` | fail: test_selection.ContractError: native custody command input enrollment changed | CI: `GrottoNip49Membership` | Other classes remain manual-only: retained native source/export/profile matrix; pinned Gitlink sources where required. |
| `test_grotto_profiles` | fail: ValueError: list.index(x): x not in list | retired | Obsolete literal legacy-profile parity and retained export census; current real Grotto configure/selection/execution and unrelated omissions are covered by test_ci_selected_execution and test_workflow_defaults. |
| `test_groundhog_app_enrollment` | fail: AssertionError: Lists differ: ['blo[1666 chars] 'nip70', 'nip70.test_inputs', 'nip73', 'nip73[556 chars]api'] != ['blo[1666 chars] 'nip65', 'nip65.tes | retired | Replaced obsolete whole-policy enrollment inverse with test_current_app_membership: current unique claims, peer memberships, capabilities, default app/NIP65 nodes and legacy ASan exclusions. |
| `test_groundhog_cmake_support` | fail: test_selection.ContractError: groundhog cmake source changed: gnome/groundhog/CMakeLists.txt | retired | Obsolete inverse to a superseded Groundhog CMake source layout; live producer fragments are exercised by test_closeout_producers and test_core_source_producers. |
| `test_gtk_ownership` | pass | manual-only | Extended source/enrollment or native-descriptor matrix; current ordinary entrypoints have bounded operational regressions. |
| `test_headless_account_policy_membership` | fail: test_selection.ContractError: native custody command input enrollment changed | CI: whole module | Current selector/workflow/runner regression; no historical inverse or custody qualification. |
| `test_headless_identity_membership` | fail: test_selection.ContractError: native custody command input enrollment changed | CI: `HeadlessIdentityMembership` | Other classes remain manual-only: retained native source/export/profile matrix; pinned Gitlink sources where required. |
| `test_headless_model_membership` | fail: test_selection.ContractError: native custody command input enrollment changed | CI: `HeadlessModelMembership` | Other classes remain manual-only: retained native source/export/profile matrix; pinned Gitlink sources where required. |
| `test_headless_transport_membership` | fail: test_selection.ContractError: native custody command input enrollment changed | CI: `HeadlessTransportMembership` | Other classes remain manual-only: retained native source/export/profile matrix; pinned Gitlink sources where required. |
| `test_homed_ci_worker` | pass | manual-only | Extended source/enrollment or native-descriptor matrix; current ordinary entrypoints have bounded operational regressions. |
| `test_homed_profile_contract` | fail: test_selection.ContractError: homed source enrollment changed: gnome/nostr-homed/CMakeLists.txt | CI: whole module | Current selector/workflow/runner regression; no historical inverse or custody qualification. |
| `test_inventory_option_drift` | pass | manual-only | Extended source/enrollment or native-descriptor matrix; current ordinary entrypoints have bounded operational regressions. |
| `test_local_command_adapter` | fail: TimeoutError: timed out | manual-only | Explicit attested outer-adapter route, cancellation and custody receipts; not the default ordinary route. |
| `test_local_selective_delivery` | pass | CI: whole module | Current selector/workflow/runner regression; no historical inverse or custody qualification. |
| `test_macos_nip5f_tcp_applicability` | fail: test_selection_macos_nip5f_tcp.ApplicabilityError: legacy workflow source drifted | manual-only | Source-bound legacy nonvalidation/applicability authority matrix, not an ordinary selection gate. |
| `test_macos_nip5f_tcp_engine` | fail: test_selection_macos_nip5f_tcp.ApplicabilityError: legacy workflow source drifted | manual-only | Legacy applicability engine matrix; positive cells need native macOS arm64 Clang with ASan/UBSan. |
| `test_mdk011_app_enrollment` | fail: AssertionError: '1162739945fe9506995543765d7026c198c49083fec7775559ac3256cedb2a18' != 'c8165e5a92638a2a10718cc1b03fb8e625969d5b56f4a3777de12a921dbc120 | retired | Replaced obsolete whole-policy enrollment inverse with test_current_app_membership: current unique claims, peer memberships, capabilities, default app/NIP65 nodes and legacy ASan exclusions. |
| `test_mdk_cleanup` | pass | manual-only | Extended host/service/custody lifecycle diagnostic; platform/kernel/resource cells remain explicit manual work. |
| `test_mdk_detached_cleanup` | pass | manual-only | Extended host/service/custody lifecycle diagnostic; platform/kernel/resource cells remain explicit manual work. |
| `test_mdk_gate` | pass | manual-only | Extended host/service/custody lifecycle diagnostic; platform/kernel/resource cells remain explicit manual work. |
| `test_mdk_selected_route` | pass | manual-only | Extended host/service/custody lifecycle diagnostic; platform/kernel/resource cells remain explicit manual work. |
| `test_media_crypto_integration` | fail: test_selection.ContractError: native custody command input enrollment changed | retired | Obsolete composition of enrollment renewals; current media, account-policy and crypto families are checked directly by their membership modules. |
| `test_namespace_apparmor_provisioning` | pass | manual-only | Extended host/service/custody lifecycle diagnostic; platform/kernel/resource cells remain explicit manual work. |
| `test_namespace_custody` | pass | manual-only | Extended host/service/custody lifecycle diagnostic; platform/kernel/resource cells remain explicit manual work. |
| `test_namespace_host_probe` | pass | manual-only | Extended host/service/custody lifecycle diagnostic; platform/kernel/resource cells remain explicit manual work. |
| `test_namespace_policy_contract` | fail: AssertionError: Lists differ: [] != ['python3 tests/selection/static_frontend_[581 chars]ard'] | manual-only | Explicit namespace diagnostic/controller and supervisor policy matrix; separate from ordinary Static. |
| `test_namespace_testhost_lifecycle` | pass | manual-only | Extended host/service/custody lifecycle diagnostic; platform/kernel/resource cells remain explicit manual work. |
| `test_namespace_wrapper_adapter` | fail: AssertionError: False != True : test-gate: legacy evidence: /private/var/folders/mt/rcd9cp6j61sbdjzt0r576yv40000gn/T/nostrc-command-adapter-ek5y0mw6/e | manual-only | Actual Linux PID-namespace/private-service custody; requires controlled kernel policy and service tools. |
| `test_native_services` | pass | manual-only | Extended host/service/custody lifecycle diagnostic; platform/kernel/resource cells remain explicit manual work. |
| `test_nip49_runtime_descriptor` | pass | manual-only | Extended source/enrollment or native-descriptor matrix; current ordinary entrypoints have bounded operational regressions. |
| `test_nip55l_selection` | pass | CI: whole module | Current selector/workflow/runner regression; no historical inverse or custody qualification. |
| `test_nonmember_ownership` | fail: test_selection.ContractError: native custody command input enrollment changed | manual-only | Extended nonmember/metadata audit matrix; no ordinary profile admission or acceptance credit. |
| `test_normal_execution_contract` | pass | manual-only | Extended attestation/receipt/profile qualification or nonvalidation matrix; remains explicitly invoked. |
| `test_normal_transport` | fail: ValueError: substring not found | CI: whole module | Current selector/workflow/runner regression; no historical inverse or custody qualification. |
| `test_ordinary_gate_wiring` | pass | CI: whole module | Current selector/workflow/runner regression; no historical inverse or custody qualification. |
| `test_ordinary_go_full_ownership` | pass | manual-only | Extended attestation/receipt/profile qualification or nonvalidation matrix; remains explicitly invoked. |
| `test_ordinary_nonmember_subsets` | fail: AssertionError: 'ee3bdc9bd4db0e832ba13858ae824ef3c8f859b328c4cd5c8f473f5113e843c6' != '5333dff7e6ec84e03bbf7d3926788d77592c1317ab297d9636581c684ad99d6 | manual-only | Historical nonmember authority, exact omission and release-refusal matrix; not ordinary CI acceptance. |
| `test_package_boundary_selection` | pass | CI: whole module | Current selector/workflow/runner regression; no historical inverse or custody qualification. |
| `test_phase_environments` | pass | manual-only | Extended attestation/receipt/profile qualification or nonvalidation matrix; remains explicitly invoked. |
| `test_post1640_integration` | fail: ValueError: unaccounted/stale workflow jobs: ['.github/workflows/ci-selective.yml:appimage', '.github/workflows/ci-selective.yml:bp', '.github/workflo | retired | Superseded post-1640 inverse-only snapshot; current workflow and selector behavior is covered by test_workflow_defaults and test_ci_selected_execution. |
| `test_post608c_integration` | fail: ValueError: unaccounted/stale workflow jobs: ['.github/workflows/ci-selective.yml:appimage', '.github/workflows/ci-selective.yml:bp', '.github/workflo | retired | Superseded post-608c inverse-only snapshot; current namespace orchestration and workflow behavior is covered by test_namespace_policy_contract and test_workflow_closeout. |
| `test_prepared_profile_contract` | pass | manual-only | Extended attestation/receipt/profile qualification or nonvalidation matrix; remains explicitly invoked. |
| `test_production_inventory` | fail: test_selection.ContractError: native custody command input enrollment changed | CI: whole module | Current selector/workflow/runner regression; no historical inverse or custody qualification. |
| `test_profile_contracts` | fail: test_selection.ContractError: native custody command input enrollment changed | CI: `WorkflowMatrixAccounting` | Other classes remain manual-only: retained native source/export/profile matrix; pinned Gitlink sources where required. |
| `test_realization` | fail: ModuleNotFoundError: No module named 'test_selection_macos_nip5f_tcp' | CI: whole module | Current selector/workflow/runner regression; no historical inverse or custody qualification. |
| `test_realization_exclusions` | fail: ModuleNotFoundError: No module named 'test_selection' | manual-only | Extended excluded-registration/nonvalidation authority matrix; never converts exclusions into passes. |
| `test_realization_inputs` | pass | manual-only | Extended native build/input-audit/process matrix; bounded current CLI/runner checks are in the operational suite. |
| `test_realization_run` | fail: ModuleNotFoundError: No module named 'test_selection_macos_nip5f_tcp' | CI: whole module | Current selector/workflow/runner regression; no historical inverse or custody qualification. |
| `test_realization_source_identity` | fail: ModuleNotFoundError: No module named 'test_selection_macos_nip5f_tcp' | CI: whole module | Current selector/workflow/runner regression; no historical inverse or custody qualification. |
| `test_realization_support_inputs` | fail: ModuleNotFoundError: No module named 'test_selection_macos_nip5f_tcp' | CI: whole module | Current selector/workflow/runner regression; no historical inverse or custody qualification. |
| `test_realization_toolchains` | fail: ModuleNotFoundError: No module named 'test_selection_macos_nip5f_tcp' | CI: whole module | Current selector/workflow/runner regression; no historical inverse or custody qualification. |
| `test_release_plan` | pass | manual-only | Extended attestation/receipt/profile qualification or nonvalidation matrix; remains explicitly invoked. |
| `test_resource_placement` | fail: test_selection.ContractError: W11 batch2 integration source changed: scripts/test-gate.sh | manual-only | Native namespace/private-resource socket placement and cleanup matrix; controlled host policy required. |
| `test_resource_profile_contracts` | fail: AssertionError: '502b66551aaf3c1b8ea1db84fd5b07eb0f283d9826e870ab4053a48c350c649a' != '7c64f106ea4956438f1a54ac442d616500c027d27e071ca7e2fb4e45da329d7 | manual-only | Retained native resource export/profile membership matrix; not a current production export or source-byte attestation. |
| `test_rollout_prerequisites` | pass | manual-only | Explicit qualification/rollout/signer-boundary diagnostic matrix; never an ordinary delivery gate. |
| `test_runtime_diagnostic_inputs` | pass | manual-only | Extended source/enrollment or native-descriptor matrix; current ordinary entrypoints have bounded operational regressions. |
| `test_runtime_integration_contract` | fail: AssertionError: {'gro[4397 chars]test-bud02-auth', 'test-hanami-blossom-batch',[6452 chars]6'}}} != {'gro[4397 chars]test-hanami-blossom-batch', 'test | retired | Superseded runtime-enrollment inverse snapshot; actual runtime input binding is exercised by test_ctest_runtime_inputs and test_command_execution. |
| `test_sanitizer_adapter` | pass | manual-only | Extended attestation/receipt/profile qualification or nonvalidation matrix; remains explicitly invoked. |
| `test_sanitizer_partition_drift` | pass | manual-only | Extended attestation/receipt/profile qualification or nonvalidation matrix; remains explicitly invoked. |
| `test_sanitizer_route_refusal` | pass | manual-only | Extended attestation/receipt/profile qualification or nonvalidation matrix; remains explicitly invoked. |
| `test_selection_command_contract` | fail: ValueError: command source closure changed: ['{source}/bindings/rust/porthome-sys/Cargo.toml', '{source}/bindings/rust/vault-sys/Cargo.toml', '{source | manual-only | Full diagnostic suite input-enrollment/audit matrix; does not force universal diagnostics onto ordinary affected inputs. |
| `test_selector` | pass | CI: whole module | Current selector/workflow/runner regression; no historical inverse or custody qualification. |
| `test_selector_checkout_binding` | pass | CI: whole module | Current selector/workflow/runner regression; no historical inverse or custody qualification. |
| `test_selector_cli` | pass | CI: whole module | Current selector/workflow/runner regression; no historical inverse or custody qualification. |
| `test_service_impact` | pass | CI: whole module | Current selector/workflow/runner regression; no historical inverse or custody qualification. |
| `test_shared_dependency_edges` | pass | CI: whole module | Current selector/workflow/runner regression; no historical inverse or custody qualification. |
| `test_shared_sanitizer_execution` | pass | manual-only | Extended attestation/receipt/profile qualification or nonvalidation matrix; remains explicitly invoked. |
| `test_signer_credential_scope` | pass | manual-only | Explicit qualification/rollout/signer-boundary diagnostic matrix; never an ordinary delivery gate. |
| `test_signer_scope_proof` | pass | manual-only | Explicit qualification/rollout/signer-boundary diagnostic matrix; never an ordinary delivery gate. |
| `test_store_key_membership` | fail: test_selection.ContractError: native custody command input enrollment changed | CI: `StoreKeyMembership` | Other classes remain manual-only: retained native source/export/profile matrix; pinned Gitlink sources where required. |
| `test_supported_profile_membership` | fail: test_selection.ContractError: native custody command input enrollment changed | CI: `SupportedCapabilityDescriptors` | Other classes remain manual-only: retained native source/export/profile matrix; pinned Gitlink sources where required. |
| `test_vector_family_integration` | fail: test_selection.ContractError: native custody command input enrollment changed | retired | Obsolete vector-enrollment composition; canonical vector/descriptor and actual family membership remain covered by test_nip49_runtime_descriptor and test_grotto_crypto_membership. |
| `test_w11_batch2_integration` | fail: ValueError: unaccounted/stale workflow jobs: ['.github/workflows/ci-selective.yml:appimage', '.github/workflows/ci-selective.yml:bp', '.github/workflo | retired | Obsolete batch2 inverse-only source/enrollment snapshot; live worker qualification and signer refusals are exercised by test_normal_transport and test_worker_signer. |
| `test_w11_evidence` | pass | manual-only | Explicit qualification/rollout/signer-boundary diagnostic matrix; never an ordinary delivery gate. |
| `test_w11_heartbeat_enrollment` | fail: AssertionError: 'bbb3eed4ff6e47769ffb9510ae0fdf1e926a6b716777f2e7cf3bd8f74616fe1e' != '10a675396b64c928f3d5bb79025ab2f6be0acbacaa3410155090e186ecdcdcb | manual-only | Record-only diagnostic source/workflow enrollment; not ordinary latency, scheduling or acceptance coverage. |
| `test_w11_matrix` | pass | manual-only | Explicit qualification/rollout/signer-boundary diagnostic matrix; never an ordinary delivery gate. |
| `test_w11_matrix_integration` | fail: test_selection.ContractError: W11 batch2 integration source changed: scripts/test_selection_realization.py | retired | Obsolete matrix enrollment inverse snapshot; the live diagnostic driver and failures are exercised by test_w11_matrix. |
| `test_w11_measurement` | pass | manual-only | Explicit qualification/rollout/signer-boundary diagnostic matrix; never an ordinary delivery gate. |
| `test_w11_policy_controller` | fail: ValueError: unaccounted/stale workflow jobs: ['.github/workflows/ci-selective.yml:appimage', '.github/workflows/ci-selective.yml:bp', '.github/workflo | manual-only | Explicit Docker regression-domain/attestation controller contracts; not an ordinary runner backend. |
| `test_w11_post_1f7_integration` | fail: AssertionError: '0801e216aeeede9dde555a108f1ca6bcb4bfe08e95ae75b27196bbab3b82ad07' != 'cb38eb2125d008c2f425aa1fc7245e9a7834051907ccb186aa90e7023bc38b4 | retired | Obsolete post-1f7 byte-inverse snapshot; live namespace record ownership/refusals are exercised by test_namespace_policy_contract. |
| `test_w11_post_419_integration` | fail: AssertionError: 'a3f56c3adae584295795b100f888e98dc0efad80aaeb58c5acfd195df2410948' != 'dd00b87e7df1aede8e13ac40b867583fbb8074c3f50726f4a45c4025decf218 | retired | Obsolete post-419 byte-inverse snapshot; live namespace record ownership/refusals are exercised by test_namespace_policy_contract. |
| `test_w11_post_h_integration` | fail: ValueError: unaccounted/stale workflow jobs: ['.github/workflows/ci-selective.yml:appimage', '.github/workflows/ci-selective.yml:bp', '.github/workflo | retired | Obsolete post-H source/enrollment inverse snapshot; current qualification and orchestration behavior is covered by test_normal_transport and test_namespace_policy_contract. |
| `test_w11_receipt_retention` | pass | manual-only | Explicit qualification/rollout/signer-boundary diagnostic matrix; never an ordinary delivery gate. |
| `test_w11_retained_plan_adapter` | pass | manual-only | Explicit qualification/rollout/signer-boundary diagnostic matrix; never an ordinary delivery gate. |
| `test_w11_submodule_materialization` | pass | manual-only | Explicit qualification/rollout/signer-boundary diagnostic matrix; never an ordinary delivery gate. |
| `test_w11_worker_enrollment` | fail: test_selection.ContractError: W11 batch2 integration source changed: .github/workflows/dependency-worker.yml | manual-only | Diagnostic signer fixture/custody boundary and source enrollment; not signer readiness or hosted qualification. |
| `test_w11_worker_host_guard` | pass | manual-only | Explicit qualification/rollout/signer-boundary diagnostic matrix; never an ordinary delivery gate. |
| `test_w11_worker_setup` | fail: test_selection.ContractError: native custody command input enrollment changed | manual-only | Physical pinned-vector checkout and package-toolchain identity diagnostics; separate from ordinary affected execution. |
| `test_worker_prerequisites` | fail: ValueError: Grotto Blueprint pin not found exactly once | CI: whole module | Current selector/workflow/runner regression; no historical inverse or custody qualification. |
| `test_worker_signer` | pass | manual-only | Explicit qualification/rollout/signer-boundary diagnostic matrix; never an ordinary delivery gate. |
| `test_worker_signer_prerequisites` | fail: test_selection.ContractError: W11 batch2 integration source changed: .github/workflows/dependency-worker.yml | CI: whole module | Current selector/workflow/runner regression; no historical inverse or custody qualification. |
| `test_workflow_closeout` | fail: AssertionError: Items in the first set but not the second: | CI: whole module | Current selector/workflow/runner regression; no historical inverse or custody qualification. |
| `test_workflow_contracts` | fail: test_selection.ContractError: profile has no Actions worker; coverage incomplete | CI: whole module | Current selector/workflow/runner regression; no historical inverse or custody qualification. |
| `test_workflow_defaults` | pass | CI: whole module | Current selector/workflow/runner regression; no historical inverse or custody qualification. |

### Retired cases in surviving modules

These cases asserted obsolete enrollment/source-snapshot equivalence. Current
behavior, fixture/source binding, negative admission and execution checks remain;
the table above identifies operational versus manual ownership. In particular,
the store-key regression still selects both MDK consumers without an obsolete exact
lane list. Redundant graph paths are no longer mistaken for missing dependencies, and current
ordinary-only profiles are not silently assigned nonexistent attestation workers.

- `test_core_source_producers`: `test_source_projection_is_byte_exact_and_rejects_unrelated_changes`.
- `test_encrypted_media_membership`: `test_only_three_selected_names_change_never_omission_authority`, `test_historical_bytes_and_base_source_witness_remain_exact`.
- `test_fuzz_json_glib_setup`: `test_exact_sources_jobs_and_finite_historical_inverse`.
- `test_grotto_crypto_membership`: `test_six_only_arrays_and_strict_full_record_projection`, `test_workflow_projection_requires_exact_prior_check_list`, `test_historical_vector_gap_and_interop_exclusion_remain_recorded`, `test_history_sources_and_other_authorities_are_preserved`.
- `test_grotto_nip49_membership`: `test_precise_selected_arrays_and_full_prior_record_projection`, `test_exact_descriptor_source_renewal_and_mutations_refuse`, `test_source_bound_history_other_authorities_and_capabilities_preserved`.
- `test_headless_account_policy_membership`: `test_exact_four_name_projection_preserves_omission_and_source_authority`, `test_historical_bytes_and_live_source_controls_remain_exact`, `test_exported_command_artifact_and_complete_compiled_input_closure`.
- `test_headless_identity_membership`: `test_selected_closure_renewal_preserves_all_other_authority`.
- `test_headless_model_membership`: `test_reviewed_subset_expands_selected_closure_not_omissions`, `test_existing_workflow_authorizes_names_not_full_application_or_mdk`.
- `test_headless_transport_membership`: `test_selected_coverage_only_projection_and_tampering_refuse`, `test_source_bound_immutable_history_graph_profiles_and_authorities`, `test_historical_nodes_refuse_unknown_edges_and_missing_reviewed_edges`.
- `test_homed_profile_contract`: `test_historical_policy_and_sources_reverse_exactly_without_authority_renewal`.
- `test_nonmember_ownership`: `test_all_1923_frozen_relationships_have_registration_local_coverage`, `test_nonmembers_are_still_exactly_unexecuted_and_not_gate_ready`.
- `test_ordinary_nonmember_subsets`: `test_historical_production_policy_cannot_promote_ordinary_evidence`.
- `test_production_inventory`: `test_all_observed_failed_hosted_source_edges_reach_their_checks`, `test_real_postbuild_witness_changes_select_their_actual_consumers`.
- `test_profile_contracts`: `test_two_actual_matrix_entries_have_distinct_faithful_profiles`, `test_effective_job_environment_and_runner_setup_defaults`, `test_root_membership_comes_from_actual_source_owner_exports`, `test_sanitizer_members_are_observed_and_have_source_grounded_target_inputs`.
- `test_resource_placement`: `test_live_sources_and_runtime_inputs_match_the_witness`, `test_inverse_reverses_only_named_edits`.
- `test_selection_command_contract`: `test_each_actual_input_has_semantic_impact_on_command`.
- `test_store_key_membership`: `test_export_provenance_keeps_historical_tooling_and_unchanged_graph_sources`, `test_store_source_savings_preserve_preexisting_mdk_dependency_paths`, `test_libgo_presence_does_not_authorize_cross_profile_execution`.
- `test_supported_profile_membership`: `test_actual_inventory_valid_and_historical_bytes_immutable`, `test_complete_primary_census_without_fixture_or_omission_laundering`, `test_every_real_compiled_runtime_and_fixture_input_has_check_local_impact`, `test_no_invented_production_graph_profile_or_sanitizer_authority`, `test_unknown_contract_and_policy_mutations_are_not_projected_away`, `test_postbuild_families_have_exact_input_owners_and_removal_negatives`.
- `test_w11_worker_enrollment`: `test_exact_source_refresh_preserves_original_omission_authority`.
- `test_w11_worker_setup`: `test_exact_source_enrollment_preserves_ownership_and_historical_bindings`.
- `test_worker_prerequisites`: `test_exact_policy_inverse_reaches_base_and_is_idempotent`, `test_exact_source_inverse_and_unreviewed_byte_refusal`, `test_jobs_inverse_projects_only_the_exact_reviewed_hashes`, `test_missing_duplicate_mutated_and_mixed_enrollment_refuse`, `test_new_runtime_inputs_have_live_command_ownership`, `test_requirement_is_derived_from_actual_grotto_and_groundhog_source`, `test_every_grotto_profile_gets_the_pinned_compiler_before_execution`, `test_hosted_failing_worker_bytes_lack_groundhog_linux_only`, `test_missing_drifted_masked_or_duplicated_pin_is_refused`, `test_apps_disabled_profiles_carry_no_requirement`, `test_hosted_failing_worker_bytes_lack_the_package`.
- `test_worker_signer_prerequisites`: `test_checkpoint_worker_lacked_exactly_these_prerequisites`.

### `scripts/test_selection*` implementation modules

These twelve files contain implementation APIs, not unittest cases. With Python
3.14, `python3 -m unittest <implementation>` returns exit 5 for zero tests; that is
not ModuleNotFoundError or a failed case. Import them with `PYTHONPATH=scripts`.
The repaired tests import the scripts directory explicitly before loading engine
modules. No test case is deleted merely to hide an import-path error.

| Module | Inventory result |
| --- | --- |
| `test_selection` | zero cases; implementation import checked separately |
| `test_selection_custody` | zero cases; implementation import checked separately |
| `test_selection_execution` | zero cases; implementation import checked separately |
| `test_selection_keyring` | zero cases; implementation import checked separately |
| `test_selection_legacy` | zero cases; implementation import checked separately |
| `test_selection_macos_nip5f_tcp` | zero cases; implementation import checked separately |
| `test_selection_mdk` | zero cases; implementation import checked separately |
| `test_selection_policy` | zero cases; implementation import checked separately |
| `test_selection_producers` | zero cases; implementation import checked separately |
| `test_selection_realization` | zero cases; implementation import checked separately |
| `test_selection_services` | zero cases; implementation import checked separately |
| `test_selection_subset` | zero cases; implementation import checked separately |
