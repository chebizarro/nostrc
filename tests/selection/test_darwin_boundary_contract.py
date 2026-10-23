"""Boundary authoring only; no native execution/profile acceptance credit."""
import importlib.util
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import test_selection_realization as realization
import test_selection as selection

spec = importlib.util.spec_from_file_location('planner_darwin_boundary', ROOT / 'scripts/test-plan.py')
planner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(planner)


class DarwinBoundaryContract(unittest.TestCase):
    def test_public_parser_accepts_explicit_diagnostic_boundary(self):
        for command in ('validate', 'resolve', 'check-inputs', 'run'):
            args = [command, '--diagnostic-boundary', 'darwin_seatbelt_v1', '--build-dir', '/unused']
            if command in ('resolve', 'run'):
                args += ['--plan', '/unused-plan']
            self.assertEqual(planner.parser().parse_args(args).diagnostic_boundary, 'darwin_seatbelt_v1')

    def test_boundary_is_incomplete_and_nested_nonpromotable(self):
        evidence = realization._diagnostic_boundary({'diagnostic_boundary': 'darwin_seatbelt_v1'})
        self.assertEqual(evidence, {'diagnostic_boundary': 'darwin_seatbelt_v1', 'complete': False,
                                   'complete_gate_eligible': False, 'untraced_profile_parity': False})
        evidence.update(complete=True, complete_gate_eligible=True)
        self.assertTrue(selection._nested_incomplete({'nested': [evidence]}))

    def test_unknown_boundary_still_refuses(self):
        with self.assertRaisesRegex(realization.RealizationError, 'unknown diagnostic'):
            realization._diagnostic_boundary({'diagnostic_boundary': 'darwin_native_complete_v1'})


if __name__ == '__main__':
    unittest.main()
