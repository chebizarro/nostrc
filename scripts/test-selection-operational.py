#!/usr/bin/env python3
"""Run ordinary selector/runner regressions, not universal qualification.

The manual namespace diagnostic workflow still runs full unittest discovery.
Keep this list tied to current selector/workflow/runner behavior: retained data
may supply regression inputs, but historical source-enrollment, attestation and
custody matrices stay manual-only. Mixed modules use class-qualified entries; their retained source closures need
pinned submodule checkouts and stay on full manual discovery.
See tests/selection/README.md for module dispositions and manual-only reasons.
"""
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
MODULES = (
    "test_selector",
    "test_nip55l_selection",
    "test_git_ranges",
    "test_selector_cli",
    "test_selector_checkout_binding",
    "test_shared_dependency_edges",
    "test_package_boundary_selection",
    "test_service_impact",
    "test_cmake_inventory_compat",
    "test_local_selective_delivery",
    "test_workflow_defaults",
    "test_ci_selected_execution",
    "test_ordinary_gate_wiring",
    "test_closeout_producers",
    "test_core_source_producers",
    "test_current_app_membership",
    "test_default_producers",
    "test_encrypted_media_membership.EncryptedMediaMembership",
    "test_fuzz_json_glib_setup.FuzzJsonGlibSetupTests",
    "test_grotto_crypto_membership.GrottoCryptoMembership",
    "test_grotto_nip49_membership.GrottoNip49Membership",
    "test_headless_account_policy_membership",
    "test_headless_identity_membership.HeadlessIdentityMembership",
    "test_headless_model_membership.HeadlessModelMembership",
    "test_headless_transport_membership.HeadlessTransportMembership",
    "test_homed_profile_contract",
    "test_normal_transport",
    "test_production_inventory",
    "test_profile_contracts.WorkflowMatrixAccounting",
    "test_realization",
    "test_realization_run",
    "test_realization_source_identity",
    "test_realization_support_inputs",
    "test_realization_toolchains",
    "test_store_key_membership.StoreKeyMembership",
    "test_supported_profile_membership.SupportedCapabilityDescriptors",
    "test_worker_prerequisites",
    "test_worker_signer_prerequisites",
    "test_workflow_closeout",
    "test_workflow_contracts",
)


def main():
    sys.path.insert(0, str(ROOT / "tests/selection"))
    suite = unittest.defaultTestLoader.loadTestsFromNames(MODULES)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
