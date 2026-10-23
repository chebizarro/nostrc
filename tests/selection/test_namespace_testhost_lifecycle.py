"""Ownership/transport failure contracts; actual Docker seam is exercised separately."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import namespace_testhost_lifecycle as lifecycle


class NamespaceTesthostLifecycleTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.control = Path(self.tmp.name)
        self.image = 'sha256:' + 'a' * 64
        (self.control / 'image.id').write_text(self.image)
        lifecycle.prepare(self.control)
        self.state = json.loads((self.control / 'owner.json').read_text())
        self.cid = 'b' * 64

    def observed(self):
        return dict(Id=self.cid, Name='/' + self.state['name'], Image=self.image,
                    Config=dict(User='1001:1001', Labels={lifecycle.LABEL: self.state['token']}),
                    HostConfig=dict(Privileged=False, CapAdd=None, NetworkMode='none', ReadonlyRootfs=True))

    def result(self):
        return json.loads((self.control / 'cleanup.json').read_text())

    def test_prepare_is_persisted_without_creating_or_contacting_docker(self):
        with patch.object(lifecycle, 'docker') as docker:
            lifecycle.prepare(self.control)
        docker.assert_not_called()
        self.assertEqual(len((self.control / 'owner.token').read_text().strip()), 32)
        self.assertTrue((self.control / 'container.name').read_text().startswith('nostrc-namespace-policy-'))

    def test_missing_and_empty_cid_reconcile_only_exact_token_and_image(self):
        for value in (None, ''):
            with self.subTest(cid_file=value):
                if value is not None:
                    (self.control / 'container.cid').write_text(value)
                with patch.object(lifecycle, 'docker', side_effect=[
                        self.cid, json.dumps([self.observed()]), self.cid, '']) as docker:
                    self.assertEqual(lifecycle.cleanup(self.control), 0)
                calls = docker.call_args_list
                self.assertIn('label=' + lifecycle.LABEL + '=' + self.state['token'], calls[0].args)
                self.assertEqual(calls[2].args, ('container', 'rm', '--force', '--volumes', self.cid))
                self.assertTrue(self.result()['cleanup_complete'])
                self.assertFalse(self.result()['complete'])

    def test_recorded_cid_mismatch_refuses_before_inspection_or_removal(self):
        (self.control / 'container.cid').write_text('c' * 64)
        with patch.object(lifecycle, 'docker', return_value=self.cid) as docker:
            self.assertEqual(lifecycle.cleanup(self.control), 1)
        self.assertEqual(docker.call_count, 1)
        self.assertFalse(self.result()['cleanup_complete'])

    def test_mismatched_image_or_unsafe_setup_never_authorizes_removal(self):
        for field in ('image', 'privileged', 'writable_root'):
            with self.subTest(field=field):
                observed = self.observed()
                if field == 'image':
                    observed['Image'] = 'sha256:' + 'c' * 64
                elif field == 'privileged':
                    observed['HostConfig']['Privileged'] = True
                else:
                    observed['HostConfig']['ReadonlyRootfs'] = False
                with patch.object(lifecycle, 'docker', side_effect=[self.cid, json.dumps([observed])]) as docker:
                    self.assertEqual(lifecycle.cleanup(self.control), 1)
                self.assertEqual(docker.call_count, 2)
                self.assertFalse(self.result()['cleanup_complete'])

    def test_failed_daemon_query_is_not_absence_or_cleanup_success(self):
        with patch.object(lifecycle, 'docker', side_effect=RuntimeError('daemon unavailable')):
            self.assertEqual(lifecycle.cleanup(self.control), 1)
        self.assertFalse(self.result()['cleanup_complete'])
        self.assertIn('daemon unavailable', self.result()['error'])

    def test_ambiguous_token_or_failed_removal_stays_incomplete(self):
        for replies in ([self.cid + '\n' + 'c' * 64],
                        [self.cid, json.dumps([self.observed()]), RuntimeError('reply lost')]):
            with self.subTest(replies=len(replies)):
                with patch.object(lifecycle, 'docker', side_effect=replies):
                    self.assertEqual(lifecycle.cleanup(self.control), 1)
                self.assertFalse(self.result()['cleanup_complete'])


if __name__ == '__main__':
    unittest.main()
