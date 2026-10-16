import copy
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import test_selection as s


def policy():
    profile = {"platform": "linux", "architecture": "x86_64", "compiler": "gcc", "build_type": "Debug", "instrumentation": "none", "source_dir": ".", "configure_args": [], "environment": {}, "capabilities": [], "setup": "synthetic", "local_availability": "host"}
    nodes = {n: {"id": n, "owner_paths": [n], "depends_on": deps, "rationale": "synthetic dependency"} for n, deps in {"core": [], "app": ["core"], "fixture": [], "other": []}.items()}
    checks = {n: {"nodes": owners, "profiles": ["linux"], "kind": "ctest", "cost": "expensive" if n == "app.live" else "ordinary", "requirement": "blocking", "expected_outcome": "pass", "ctest_tests": {"linux": [n]}} for n, owners in {"core.unit": ["core"], "app.unit": ["app"], "app.live": ["app"], "fixture.unit": ["fixture"], "other.unit": ["other"]}.items()}
    rules = [{"prefix": n, "nodes": [n], "effect": "impact", "reason": "synthetic owner"} for n in nodes]
    rules += [{"prefix": "docs", "nodes": [], "effect": "non_execution", "reason": "prose only"}, {"prefix": "tests/selection", "nodes": ["core"], "effect": "global", "reason": "contract change"}]
    return {"schema_version": 1, "nodes": nodes, "path_rules": rules, "test_overrides": [], "profiles": {"linux": profile}, "checks": checks, "release_components": {}}


def change(path, status="M", old=None):
    return {"status": status, "old_path": old if old is not None else (None if status == "A" else path), "new_path": None if status == "D" else path}


def plan(p=None, paths=("app/source.c",), **kwargs):
    return s.make_plan(p or policy(), [change(x) for x in paths], head="a" * 40, base="b" * 40, **kwargs)


def receipt(p, selected):
    lane = selected["lanes"][0]
    result = {"schema_version": 1, "lane_id": lane["id"], "profile": lane["profile"], "profile_hash": lane["profile_hash"], "profile_fingerprint": "fresh-configure-fingerprint", "job_conclusion": "success", "registration_audit": "passed", "input_audit": "passed", "checks": {}}
    result.update({k: copy.deepcopy(selected[k]) for k in ("plan_id", "inventory_hash", "source_identity")})
    for ident in lane["checks"]:
        names = p["checks"][ident].get("ctest_tests", {}).get(lane["profile"], [ident])
        result["checks"][ident] = {"outcome": "passed", "expected_executions": names[:], "observed_executions": names[:]}
    return result


class GitCase(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.repo = Path(self.temp.name)
        self.git("init", "-q")
        self.git("config", "user.name", "Selection Test")
        self.git("config", "user.email", "selection@example.invalid")
        self.p = policy()
        self.write("tests/selection/inventory.json", s.canonical(self.p).decode())
        self.write("app/source.c", "base app\n")
        self.write("core/source.c", "base core\n")
        self.base = self.commit()

    def git(self, *args):
        return subprocess.check_output(["git", "-C", str(self.repo), *args], stderr=subprocess.PIPE).decode().strip()

    def write(self, path, content):
        target = self.repo / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content)

    def commit(self):
        self.git("add", "-A")
        self.git("commit", "-qm", "synthetic commit")
        return self.git("rev-parse", "HEAD")

    def select(self, head=None, **kwargs):
        return s.git_plan(self.repo, self.p, head=head or self.git("rev-parse", "HEAD"), base=kwargs.pop("base", self.base), **kwargs)
