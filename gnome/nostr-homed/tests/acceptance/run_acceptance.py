#!/usr/bin/env python3
"""Fail-closed runner for installed Nostr login and Samba acceptance labs."""

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import secrets
import signal
import stat
import subprocess
import sys

PASS = "pass"
FAIL = "fail"
SKIP = "skip"
UNAVAILABLE = "unavailable"
VALID_STATUSES = {PASS, FAIL, SKIP, UNAVAILABLE}
OPT_IN = "I_UNDERSTAND_THIS_RUNS_AGAINST_A_DISPOSABLE_LAB"
PLACEHOLDERS = {"", "tbd", "todo", "unknown", "unavailable", "*"}


class ContractError(Exception):
    pass


def utc_now():
    return dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z")


def load_matrix(path):
    try:
        matrix = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ContractError("matrix cannot be read as JSON: %s" % exc) from exc
    if matrix.get("schema_version") != 1:
        raise ContractError("unsupported matrix schema_version")
    for key in ("matrix_id", "roles", "suites", "cases"):
        if key not in matrix:
            raise ContractError("matrix is missing %s" % key)
    seen = set()
    suite_case_count = {suite: 0 for suite in matrix["suites"]}
    for case in matrix["cases"]:
        if set(case) != {"id", "suite", "required"}:
            raise ContractError("case entries permit only id, suite, and required")
        if case["id"] in seen or case["suite"] not in matrix["suites"]:
            raise ContractError("duplicate case id or unknown suite")
        if not isinstance(case["required"], bool):
            raise ContractError("case required must be boolean")
        seen.add(case["id"])
        suite_case_count[case["suite"]] += 1
    if any(count == 0 for count in suite_case_count.values()):
        raise ContractError("every suite must contain at least one case")
    return matrix


def selected_suites(matrix, suite):
    if suite:
        if suite not in matrix["suites"]:
            raise ContractError("unknown suite: %s" % suite)
        return [suite]
    return sorted(matrix["suites"])


def pin_is_exact(value):
    if value is None:
        return False
    if isinstance(value, str):
        return value.strip().lower() not in PLACEHOLDERS
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def readiness(matrix, suites, require_adapters):
    problems = []
    role_names = set()
    for suite in suites:
        spec = matrix["suites"][suite]
        role_names.update(spec.get("required_roles", []))
        adapter_env = spec.get("adapter_env")
        timeout = spec.get("timeout_seconds")
        if not isinstance(timeout, int) or isinstance(timeout, bool) or timeout <= 0:
            problems.append({"kind": "contract", "suite": suite, "detail": "timeout_seconds must be a positive integer"})
        if not adapter_env:
            problems.append({"kind": "contract", "suite": suite, "detail": "adapter_env missing"})
        elif require_adapters:
            adapter = os.environ.get(adapter_env, "")
            if not adapter:
                problems.append({"kind": "adapter", "suite": suite, "detail": "%s is unset" % adapter_env})
            else:
                path = Path(adapter)
                if not path.is_absolute() or path.is_symlink() or not path.is_file() or not os.access(str(path), os.X_OK):
                    problems.append({"kind": "adapter", "suite": suite, "detail": "%s must name an absolute, non-symlink executable file" % adapter_env})
    for name in sorted(role_names):
        role = matrix["roles"].get(name)
        if not role:
            problems.append({"kind": "contract", "role": name, "detail": "role missing"})
            continue
        if role.get("state") != "ready":
            problems.append({"kind": "unavailable", "role": name, "detail": role.get("reason", "role is not ready")})
        pins = role.get("pins", {})
        for pin in role.get("required_pins", []):
            value = pins.get(pin)
            if not pin_is_exact(value):
                problems.append({"kind": "pin", "role": name, "detail": "%s is not pinned" % pin})
            elif pin.endswith("sha256") and (not isinstance(value, str) or len(value) != 64 or any(c not in "0123456789abcdefABCDEF" for c in value)):
                problems.append({"kind": "pin", "role": name, "detail": "%s is not a 64-digit SHA-256" % pin})
    return problems


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def safe_evidence_files(root):
    files = []
    for path in sorted(root.rglob("*")):
        mode = path.lstat().st_mode
        if stat.S_ISLNK(mode):
            raise ContractError("evidence may not contain symlinks")
        if stat.S_ISREG(mode):
            files.append(path)
        elif not stat.S_ISDIR(mode):
            raise ContractError("evidence contains a non-regular file")
    return files


def scan_canary(files, canary):
    needle = canary.encode("utf-8")
    for path in files:
        carry = b""
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                combined = carry + chunk
                if needle in combined:
                    return False
                carry = combined[-max(0, len(needle) - 1):]
    return True


def adapter_environment(matrix_path, evidence_dir, results_path, canary):
    allowed = ("HOME", "LANG", "LC_ALL", "PATH", "SSH_AUTH_SOCK", "TMPDIR")
    env = {key: os.environ[key] for key in allowed if key in os.environ}
    env.update({
        "NOSTR_ACCEPTANCE_MATRIX": str(matrix_path),
        "NOSTR_ACCEPTANCE_EVIDENCE_DIR": str(evidence_dir),
        "NOSTR_ACCEPTANCE_RESULTS": str(results_path),
        "NOSTR_ACCEPTANCE_TEST_SECRET_CANARY": canary,
    })
    return env


def run_suite(matrix, matrix_path, suite, root):
    suite_root = root / suite
    suite_root.mkdir(mode=0o700)
    results_path = suite_root / "suite-results.json"
    canary = secrets.token_urlsafe(32)
    adapter = os.environ[matrix["suites"][suite]["adapter_env"]]
    process = subprocess.Popen(
        [adapter, "--matrix", str(matrix_path), "--evidence-dir", str(suite_root), "--results", str(results_path)],
        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        env=adapter_environment(matrix_path, suite_root, results_path, canary), start_new_session=True,
    )
    expected = {c["id"]: c for c in matrix["cases"] if c["suite"] == suite}
    try:
        returncode = process.wait(timeout=matrix["suites"][suite]["timeout_seconds"])
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
        return [{"id": case_id, "status": FAIL, "evidence": []} for case_id in sorted(expected)]
    if returncode != 0:
        status = SKIP if returncode == 77 else UNAVAILABLE if returncode == 69 else FAIL
        return [{"id": case_id, "status": status, "evidence": []} for case_id in sorted(expected)]
    try:
        payload = json.loads(results_path.read_text(encoding="utf-8"))
        if set(payload) != {"schema_version", "suite", "results"} or payload["schema_version"] != 1 or payload["suite"] != suite:
            raise ContractError("invalid adapter result envelope")
        observed = {}
        for result in payload["results"]:
            if set(result) != {"id", "status", "evidence"}:
                raise ContractError("adapter results permit only id, status, and evidence")
            if result["id"] not in expected or result["id"] in observed or result["status"] not in VALID_STATUSES:
                raise ContractError("adapter returned an unknown/duplicate case or status")
            if not isinstance(result["evidence"], list) or not all(isinstance(x, str) for x in result["evidence"]):
                raise ContractError("adapter evidence must be a list of relative paths")
            if result["status"] == PASS and not result["evidence"]:
                raise ContractError("passing cases require evidence")
            observed[result["id"]] = result
        normalized = []
        for case_id in sorted(expected):
            result = observed.get(case_id, {"id": case_id, "status": UNAVAILABLE, "evidence": []})
            normalized.append(result)
        files = safe_evidence_files(suite_root)
        if not scan_canary(files, canary):
            return [{"id": case_id, "status": FAIL, "evidence": []} for case_id in sorted(expected)]
        allowed_paths = {str(path.relative_to(suite_root)): path for path in files}
        for result in normalized:
            described = []
            for relative in result["evidence"]:
                path = allowed_paths.get(relative)
                if not path or path == results_path or path.stat().st_size == 0:
                    raise ContractError("adapter referenced missing or unsafe evidence")
                described.append({"path": relative, "sha256": sha256_file(path), "bytes": path.stat().st_size})
            result["evidence"] = described
        return normalized
    except (OSError, json.JSONDecodeError, ContractError):
        return [{"id": case_id, "status": FAIL, "evidence": []} for case_id in sorted(expected)]


def overall_status(results, required):
    required_statuses = [r["status"] for r in results if required[r["id"]]]
    if FAIL in required_statuses:
        return FAIL, 1
    if any(status != PASS for status in required_statuses):
        return UNAVAILABLE, 2
    return PASS, 0


def main(argv=None):
    parser = argparse.ArgumentParser()
    actions = parser.add_mutually_exclusive_group(required=True)
    actions.add_argument("--inventory", action="store_true")
    actions.add_argument("--check-readiness", action="store_true")
    actions.add_argument("--run", action="store_true")
    parser.add_argument("--matrix", type=Path, default=Path(__file__).with_name("matrix.json"))
    parser.add_argument("--suite", choices=("gdm", "smb", "winbind"))
    parser.add_argument("--evidence-dir", type=Path)
    args = parser.parse_args(argv)
    try:
        matrix_path = args.matrix.resolve(strict=True)
        matrix = load_matrix(matrix_path)
        suites = selected_suites(matrix, args.suite)
    except (OSError, ContractError) as exc:
        print(json.dumps({"status": FAIL, "error": str(exc)}))
        return 64
    if args.inventory:
        inventory = {"schema_version": 1, "matrix_id": matrix["matrix_id"], "support_claim": matrix.get("support_claim"), "roles": matrix["roles"], "cases": [c for c in matrix["cases"] if c["suite"] in suites]}
        print(json.dumps(inventory, indent=2, sort_keys=True))
        return 0
    problems = readiness(matrix, suites, require_adapters=args.run)
    if args.check_readiness:
        status = PASS if not problems else UNAVAILABLE
        print(json.dumps({"schema_version": 1, "matrix_id": matrix["matrix_id"], "status": status, "problems": problems}, indent=2, sort_keys=True))
        return 0 if not problems else 2
    if os.environ.get("NOSTR_ACCEPTANCE_LAB_OPT_IN") != OPT_IN:
        problems.insert(0, {"kind": "opt_in", "detail": "NOSTR_ACCEPTANCE_LAB_OPT_IN is not the required literal value"})
    if not args.evidence_dir:
        problems.insert(0, {"kind": "evidence", "detail": "--evidence-dir is required for --run"})
    if problems:
        print(json.dumps({"schema_version": 1, "matrix_id": matrix["matrix_id"], "status": UNAVAILABLE, "problems": problems}, indent=2, sort_keys=True))
        return 2
    root = args.evidence_dir.resolve()
    if root.exists():
        print(json.dumps({"status": FAIL, "error": "evidence directory already exists"}))
        return 64
    root.mkdir(mode=0o700, parents=True)
    started_at = utc_now()
    results = []
    for suite in suites:
        results.extend(run_suite(matrix, matrix_path, suite, root))
    required = {case["id"]: case["required"] for case in matrix["cases"]}
    status, exit_code = overall_status(results, required)
    report = {"schema_version": 1, "matrix_id": matrix["matrix_id"], "matrix_sha256": sha256_file(matrix_path), "started_at": started_at, "finished_at": utc_now(), "status": status, "suites": suites, "results": results}
    report_path = root / "acceptance-report.json"
    temporary = root / ".acceptance-report.json.tmp"
    temporary.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.chmod(temporary, 0o600)
    temporary.replace(report_path)
    print(json.dumps({"status": status, "report": str(report_path)}))
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
