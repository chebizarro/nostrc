"""The Groundhog CMake inverse is byte-exact and refuses every other byte sequence."""
import hashlib
import unittest

from selector_support import ROOT, s
import groundhog_cmake_support as support


class GroundhogCmakeSupport(unittest.TestCase):
    def test_witness_is_pinned_and_current_file_projects_to_the_audited_bytes(self):
        self.assertEqual(hashlib.sha256((ROOT / support.WITNESS).read_bytes()).hexdigest(), support.SHA256)
        current = (ROOT / support.PATH).read_bytes()
        previous = support.before_groundhog_cmake(current)
        self.assertEqual(hashlib.sha256(previous).hexdigest(), support.witness()['before_sha256'])
        self.assertNotEqual(previous, current)
        self.assertEqual(support.before_groundhog_cmake(previous), previous)

    def test_unknown_or_tampered_bytes_refuse(self):
        current = (ROOT / support.PATH).read_bytes()
        previous = support.before_groundhog_cmake(current)
        for raw in (current + b'\n', previous + b'\n', current[:-1], b''):
            with self.subTest(size=len(raw)), self.assertRaises(s.ContractError):
                support.before_groundhog_cmake(raw)


if __name__ == '__main__':
    unittest.main()
