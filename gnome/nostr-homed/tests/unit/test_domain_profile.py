#!/usr/bin/env python3
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
VALIDATOR = ROOT / "packaging/domain/validate_domain_profile.py"
ROUTE = ROOT / "config/domain-route.conf.sample"
SMB = ROOT / "config/smb.conf.winbind.sample"
MANIFEST = ROOT / "packaging/domain/domain-profile.manifest.json.sample"

class DomainProfileTest(unittest.TestCase):
    def run_validator(self, *args):
        return subprocess.run([sys.executable, str(VALIDATOR), *map(str, args)], text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)

    def test_samples_are_strict_and_inert(self):
        result = self.run_validator("--route", ROUTE, "--smb", SMB, "--manifest", MANIFEST)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(json.loads(result.stdout)["activation_ready"])

    def test_short_name_mode_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            bad = Path(temporary) / "smb.conf"
            bad.write_text(SMB.read_text().replace("winbind use default domain = no", "winbind use default domain = yes"))
            self.assertNotEqual(self.run_validator("--route", ROUTE, "--smb", bad).returncode, 0)

    def test_overlap_and_unsafe_home_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            bad = Path(temporary) / "route.conf"
            text = ROUTE.read_text().replace("idmap_default = 1000000-1099999", "idmap_default = 250000-1099999")
            bad.write_text(text)
            self.assertNotEqual(self.run_validator("--route", bad, "--smb", SMB).returncode, 0)
            bad.write_text(ROUTE.read_text().replace("home_template = /home/%D/%U", "home_template = /home/%U"))
            self.assertNotEqual(self.run_validator("--route", bad, "--smb", SMB).returncode, 0)

    def test_unaccepted_offline_login_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            bad = Path(temporary) / "route.conf"
            bad.write_text(ROUTE.read_text().replace("offline_login = false", "offline_login = true"))
            self.assertNotEqual(self.run_validator("--route", bad, "--smb", SMB).returncode, 0)

    def test_template_cannot_activate(self):
        result = self.run_validator("--route", ROUTE, "--smb", SMB, "--manifest", MANIFEST,
                                    "--require-activation-ready", "--root", "/")
        self.assertNotEqual(result.returncode, 0)

    def test_malformed_manifest_is_structured_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "manifest.json"
            for value in [None, [], 42, "bad"]:
                path.write_text(json.dumps(value))
                result = self.run_validator("--route", ROUTE, "--smb", SMB, "--manifest", path)
                self.assertEqual(result.returncode, 1)
                self.assertEqual(json.loads(result.stderr)["status"], "invalid")

    def test_modified_pam_layout_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            pam = root / "etc/pam.d/common-auth"
            pam.parent.mkdir(parents=True)
            pam.write_text("reviewed\n")
            digest = hashlib.sha256(pam.read_bytes()).hexdigest()
            manifest = {
                "schema_version": 1, "activation_ready": True,
                "d4_evidence_sha256": "a" * 64,
                "package_pins": {"libpam-runtime": "1", "libpam-winbind": "1", "samba": "1", "winbind": "1"},
                "pam_files": [{"path": "/etc/pam.d/common-auth", "sha256": digest}],
                "rollback_dir": "/var/lib/nostr-auth/rollback/domain-profile", "notes": "test"
            }
            path = root / "manifest.json"
            path.write_text(json.dumps(manifest))
            accepted = self.run_validator("--route", ROUTE, "--smb", SMB, "--manifest", path,
                                          "--require-activation-ready", "--root", root)
            self.assertEqual(accepted.returncode, 0, accepted.stderr)
            for unsafe_path in ["/etc/pam.d/../common-auth", "/etc/pam.d/link/common-auth"]:
                manifest["pam_files"][0]["path"] = unsafe_path
                path.write_text(json.dumps(manifest))
                result = self.run_validator("--route", ROUTE, "--smb", SMB, "--manifest", path,
                                            "--require-activation-ready", "--root", root)
                self.assertNotEqual(result.returncode, 0)
            link = pam.parent / "link"
            link.symlink_to(pam.parent, target_is_directory=True)
            result = self.run_validator("--route", ROUTE, "--smb", SMB, "--manifest", path,
                                        "--require-activation-ready", "--root", root)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("symlink", result.stderr)
            manifest["pam_files"][0]["path"] = "/etc/pam.d/common-auth"
            path.write_text(json.dumps(manifest))
            pam.write_text("locally modified\n")
            rejected = self.run_validator("--route", ROUTE, "--smb", SMB, "--manifest", path,
                                          "--require-activation-ready", "--root", root)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertIn("differs from reviewed hash", rejected.stderr)

if __name__ == "__main__":
    unittest.main()
