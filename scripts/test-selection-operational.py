#!/usr/bin/env python3
"""Run ordinary selector/runner regressions, not universal qualification.

The complete experimental suite remains available via unittest discovery and the
explicit namespace diagnostic workflow. Keep this list tied to live operational
interfaces; do not add source-enrollment, attestation or custody matrices here.
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
)


def main():
    sys.path.insert(0, str(ROOT / "tests/selection"))
    suite = unittest.defaultTestLoader.loadTestsFromNames(MODULES)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
