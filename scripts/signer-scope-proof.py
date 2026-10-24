#!/usr/bin/env python3
"""Offline proof that a hosted signer lane delivered credentials only to its consumer.

The real provisioned signer runs only in the supported hosted route
(dependency-tests.yml -> dependency-worker.yml: pidns custody, per-job AppArmor
admission, post-exit worker-qualify). This tool never executes a lane.
``collect`` names (and with --execute runs) the exact artifact downloads into a
fresh empty directory. ``verify`` accepts nothing of its own: the lane's
qualified receipt must pass the existing strict aggregate path
(test_selection_execution.transported_receipts + validate_receipt), which
re-runs qualification over the receipt's own sealed transport archive (native
finalizer seal, signer delivery/consent/JUnit, physical CTest argv/output
binding) and checks every sealed source claim against an exact-SHA checkout.
Only then are the signer isolation diagnostics read, from those same verified
sealed bytes. Exit 0 only when every condition holds.
"""
import argparse
import hashlib
import json
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_selection_services import SIGNER_CONSUMER  # noqa: E402

CHECK = 'supported.nips.nip5f.signer_endpoint'
# Registrations whose private grotto-daemon inherited the old overlay (PR23) or
# spawn one without their own scrub; all must run in the consumer's CTest process.
WITNESSES = ('signer/clients', 'signer/activity', 'signer/room-flow', 'signer/approval-integration')
# Only profiles whose inventory maps every witness (re-derived from the tested
# source's inventory at verify time). core.linux, core.linux.asan and
# grotto.linux.asan_ubsan map no signer/room-flow and are not claimed.
PROFILES = ('grotto.linux', 'groundhog.linux', 'groundhog.mdk08', 'groundhog.mdk011')
LEAKS = (re.compile(rb'NOSTR_SIGNER_TOKEN='), re.compile(rb'AUTH [0-9a-f]{64}'),
         re.compile(rb'(?<![0-9A-Za-z])p[1-9][0-9]{0,4}_[0-9a-f]{64}(?![0-9a-f])'))


class ProofError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise ProofError(message)


def witness_map(policy, profile):
    """Witness -> claiming checks for ``profile`` in this inventory."""
    found = {}
    for ident, check in policy['checks'].items():
        if check.get('kind') == 'ctest' and profile in check.get('profiles', []):
            for name in check.get('ctest_tests', {}).get(profile, []):
                if name in WITNESSES:
                    found.setdefault(name, []).append(ident)
    return found


def satisfiable(policy):
    """Signer profiles whose inventory can realize the whole witness set."""
    signer = policy['checks'][CHECK]['profiles']
    return tuple(sorted(p for p in signer if set(witness_map(policy, p)) == set(WITNESSES)))


def _cases(data):
    try:
        cases = {}
        for case in ET.fromstring(data).iter('testcase'):
            require(case.get('name') not in cases, 'duplicate JUnit case: ' + str(case.get('name')))
            cases[case.get('name')] = case
        return cases
    except ET.ParseError as exc:
        raise ProofError('malformed JUnit: ' + str(exc)) from exc


def _ran(case):
    return (case is not None and case.get('status') == 'run'
            and all(case.find(tag) is None for tag in ('failure', 'error', 'skipped')))


def scan(*roots):
    """No credential form in any retained byte (whole roots, symlinks not followed)."""
    count = 0
    for root in roots:
        for path in sorted(Path(root).rglob('*')):
            if path.is_file() and not path.is_symlink():
                data = path.read_bytes()
                count += 1
                for pattern in LEAKS:
                    require(not pattern.search(data), f'credential-shaped bytes in {path.relative_to(root)}')
    require(count, 'no retained evidence to scan')
    return count


def _sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def _receipt_path(receipts, plan, profile):
    pattern = re.compile(re.escape('dependency-receipt-%s-%s-' % (plan['plan_id'], profile)) + r'[1-9][0-9]*')
    found = [path for path in Path(receipts).rglob('receipt.json')
             if path.parent.name == profile and pattern.fullmatch(path.parent.parent.name)]
    require(len(found) == 1, f'expected exactly one {profile} lane receipt artifact for plan {plan["plan_id"]}, '
                             f'found {len(found)}')
    return found[0]


def diagnose(policy, plan, receipt, public, locator, profile):
    """Signer isolation diagnostics over the verified sealed lane bytes only."""
    from test_selection_execution import _file
    raw = public.get('backend_receipt', {})
    require('signer_endpoint' in raw.get('required_capabilities', []), 'lane did not provision the signer')
    executed = set(receipt.get('executed_registrations', []))
    require(SIGNER_CONSUMER in executed, 'consumer is not in the qualified executed closure')
    missing = [name for name in WITNESSES if name not in executed]
    require(not missing, 'isolation witnesses were not co-selected into this qualified lane: ' + repr(missing))
    evidence = Path(raw['evidence_dir'])
    junit = _file(locator, evidence / 'results.xml').read_bytes()
    log = _file(locator, evidence / 'ctest.log').read_bytes()
    cases = _cases(junit)
    require(_ran(cases.get(SIGNER_CONSUMER)), 'consumer JUnit case is not a real passing run')
    failed = [name for name in WITNESSES if not _ran(cases.get(name))]
    require(not failed, 'isolation witnesses did not run and pass in the same CTest process: ' + repr(failed))
    endpoint = raw['signer_runtime']['readiness']['endpoint']
    port = endpoint.rsplit(':', 1)[1].encode()
    for name, data in (('ctest.log', log), ('results.xml', junit)):
        require(b'127.0.0.1:' + port not in data, 'fixture endpoint appears in CTest output: ' + name)
    return dict(witnesses=list(WITNESSES), witness_checks=witness_map(policy, profile),
                junit_sha256=hashlib.sha256(junit).hexdigest(), ctest_log_sha256=hashlib.sha256(log).hexdigest())


def verify(receipts, plan_path, repo, profile, evidence=None):
    import test_selection as selection
    require(profile in PROFILES, 'not a profile whose inventory can realize the isolation witnesses: ' + profile)
    repo = Path(repo).resolve(strict=True)
    policy = selection.load_inventory(repo / 'tests/selection/inventory.json')
    require(profile in satisfiable(policy), 'tested inventory cannot realize every isolation witness for ' + profile)
    return verify_qualified(policy, selection.read_json(plan_path), receipts, repo, profile, evidence)


def verify_qualified(policy, plan, receipts, repo, profile, evidence=None):
    """Strict existing acceptance first; signer diagnostics only over its verified bytes."""
    import test_selection_execution as execution
    sha = plan.get('source_identity', {}).get('sha')
    require(plan.get('source_identity', {}).get('kind') == 'committed' and re.fullmatch(r'[0-9a-f]{40}', str(sha)),
            'plan has no exact committed tested SHA')
    path = _receipt_path(receipts, plan, profile)
    original = path.parent / execution.ORIGINAL
    try:
        with execution.transported_receipts([path], repo=repo, plan=plan) as loaded:
            receipt = loaded[0]
            require(receipt.get('receipt_kind') == execution.KIND and receipt.get('profile') == profile
                    and receipt.get('source_identity') == plan['source_identity'],
                    'lane receipt is not a qualified normal receipt for this profile/source')
            public, locator = execution.verified_transport(policy, plan, receipt)
            # worker-qualify retains the published raw bytes beside the qualified
            # receipt; they must be exactly the sealed public evidence just verified.
            require(original.is_file() and not original.is_symlink()
                    and _sha(original) == receipt['public_evidence']['sha256'],
                    'original-native-receipt.json differs from the verified sealed public evidence')
            proof = diagnose(policy, plan, receipt, public, locator, profile)
            proof['scanned_files'] = scan(*locator.roots.values(), path.parent.parent)
            if evidence is not None:
                # Unbound raw artifact: diagnostic scan only; a leak still refuses.
                proof['raw_evidence_scanned_files'] = scan(evidence)
            binding = dict(qualification_hash=receipt['qualification_hash'], plan_id=plan['plan_id'],
                           lane_id=receipt['lane_id'], profile_hash=receipt['profile_hash'],
                           transport_archive_sha256=receipt['transport']['archive_sha256'],
                           public_evidence_sha256=receipt['public_evidence']['sha256'],
                           native_seal_sha256=public['native_custody_final']['seal_sha256'],
                           execution_scope=receipt.get('execution_scope', 'full'),
                           executed_registrations=len(receipt['executed_registrations']))
    except ProofError:
        raise
    except (ValueError, KeyError, TypeError, AttributeError, OSError) as exc:
        raise ProofError('strict qualified-receipt verification refused: ' + str(exc)) from exc
    return dict(proved=True, profile=profile, sha=sha, binding=binding, **proof,
                scope='hosted pidns delivery/isolation for one qualified lane; not profile, complete-gate or W11 acceptance')


def collect(run, profile, out, repo, execute):
    out = Path(out)
    require(profile in PROFILES, 'not a profile whose inventory can realize the isolation witnesses: ' + profile)
    commands = [['gh', 'run', 'download', str(run), '-R', repo, '-p', 'dependency-plan-*', '-D', str(out / 'plan')],
                ['gh', 'run', 'download', str(run), '-R', repo, '-p', f'dependency-receipt-*-{profile}-*',
                 '-D', str(out / 'receipts')],
                ['gh', 'run', 'download', str(run), '-R', repo, '-p', f'dependency-evidence-*-{profile}-*',
                 '-D', str(out / 'evidence')]]
    if execute:
        require(not out.exists() or not any(out.iterdir()), 'collection directory must be new and empty')
        for argv in commands:
            subprocess.run(argv, check=True)
    return commands


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest='command', required=True)
    c = sub.add_parser('collect')
    c.add_argument('--run', required=True, type=int)
    c.add_argument('--profile', required=True)
    c.add_argument('--out', required=True)
    c.add_argument('--repo', default='chebizarro/nostrc')
    c.add_argument('--execute', action='store_true', help='download; default prints the exact commands')
    v = sub.add_parser('verify')
    v.add_argument('--receipts', required=True, help='downloaded dependency-receipt-* artifact directory')
    v.add_argument('--plan', required=True, help='plan.json from the same run\'s dependency-plan-* artifact')
    v.add_argument('--repo', required=True, help='clean checkout (with submodules) of the plan\'s tested SHA')
    v.add_argument('--profile', required=True)
    v.add_argument('--evidence', help='optional raw dependency-evidence-* artifact (diagnostic scan only)')
    args = parser.parse_args(argv)
    try:
        if args.command == 'collect':
            result = {'commands': collect(args.run, args.profile, args.out, args.repo, args.execute)}
        else:
            result = verify(args.receipts, args.plan, args.repo, args.profile, args.evidence)
    except ProofError as exc:
        print(json.dumps({'proved': False, 'refusal': str(exc)}, indent=2))
        return 1
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == '__main__':
    sys.exit(main())
