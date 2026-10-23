"""Probe reporting contracts; these are not positive custody coverage."""
import json
import os
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
PROBE = ROOT / 'tests/selection/namespace_host_probe.py'


class NamespaceHostProbeTests(unittest.TestCase):
    def invoke(self, *args):
        result = subprocess.run([sys.executable, str(PROBE), *args], capture_output=True, timeout=40)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertTrue(records, result.stderr)
        self.assertTrue(all(record.get('time_utc') for record in records))
        self.assertEqual(records[-1]['probe_exit'], result.returncode, result.stderr)
        return result, records

    def test_nonlinux_or_root_refuses_without_namespace_child(self):
        if sys.platform == 'linux' and os.geteuid() != 0:
            # The supported-launcher branch is exercised below, never skipped.
            result, records = self.invoke()
            self.assertIn(result.returncode, (0, 125), result.stderr)
            self.assertEqual(records[0]['kind'], 'namespace_host_probe_v1')
        else:
            result, records = self.invoke()
            self.assertEqual(result.returncode, 125, result.stderr)
            self.assertEqual(len(records), 1)
            self.assertIn('ordinary nonroot Linux', records[0]['refusal'])

    if sys.platform == 'linux' and os.geteuid() != 0:
        def test_real_original_namespace_ownership_and_mapping_or_explicit_refusal(self):
            result, records = self.invoke()
            header, before, after = records[:3]
            self.assertEqual(header['scope'], 'diagnostic_not_custody_or_profile_acceptance')
            self.assertEqual(len(header['source_sha256']), 3)
            self.assertTrue(all(len(value) == 64 for value in header['source_sha256'].values()))
            self.assertEqual(before['stage'], 'before-unshare')
            self.assertEqual(before['uid'], [os.geteuid()] * 3)
            self.assertEqual(before['dumpable'], 1)
            self.assertEqual(after['stage'], 'after-unshare')
            self.assertEqual(after['dumpable'], 1)
            self.assertEqual(after['original_userns_proc_owners']['setgroups']['uid'], os.geteuid())
            if result.returncode == 0:
                self.assertEqual([r['path'] for r in records if r.get('stage') == 'write-ok'],
                                 ['setgroups', 'uid_map', 'gid_map'])
                self.assertEqual(records[-2]['uid'], before['uid'])
                self.assertEqual(records[-2]['gid'], before['gid'])
            else:
                self.assertEqual(result.returncode, 125, result.stderr)
                refusal = records[-2]
                self.assertTrue(after['unshare_errno'] or refusal.get('errno') or refusal.get('error'), records)

        def test_nondumpable_never_reenabled_and_original_observer_sees_root_owner(self):
            result, records = self.invoke('--nondumpable')
            self.assertIn(result.returncode, (0, 125), result.stderr)
            stages = [r for r in records if 'stage' in r]
            self.assertGreaterEqual(len(stages), 2)
            self.assertTrue(all(r['dumpable'] == 0 for r in stages))
            self.assertEqual(stages[0]['original_userns_proc_owners']['setgroups']['uid'], 0)
            self.assertEqual(stages[1]['original_userns_proc_owners']['setgroups']['uid'], 0)
            self.assertEqual(stages[0]['uid'], [os.geteuid()] * 3)


if __name__ == '__main__':
    unittest.main()
