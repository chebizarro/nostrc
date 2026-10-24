#!/usr/bin/env python3
"""Offline proof that a hosted signer lane delivered credentials only to its consumer.

The real provisioned signer runs only in the supported hosted route
(dependency-tests.yml -> dependency-worker.yml: pidns custody, per-job AppArmor
admission, post-exit worker-qualify). This tool never executes a lane. ``collect``
names (and with --execute runs) the exact artifact downloads into fresh empty
directories; ``verify`` reads the original downloaded files as untrusted data
(JSON/XML/bytes only) and re-derives every claim from the lane's own receipt,
JUnit and logs. Exit 0 only when every condition holds; nothing is inferred from
a missing file, a skip or a summary field alone.
"""
import argparse
import json
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_selection_services import (SIGNER_CONSUMER, SIGNER_DELIVERY, SIGNER_RESOURCE_TYPE,  # noqa: E402
                                     SIGNER_AMBIENT)

PROFILES = ('core.linux', 'core.linux.asan', 'grotto.linux', 'grotto.linux.asan_ubsan',
            'groundhog.linux', 'groundhog.mdk08', 'groundhog.mdk011')
# Registrations whose private grotto-daemon inherited the old overlay (PR23) or
# spawn one without their own scrub: they must run in the same CTest process.
WITNESSES = ('signer/clients', 'signer/activity', 'signer/room-flow', 'signer/approval-integration')
APPROVED = '(uint32 1, uint32 1, uint32 0)'
LEAKS = (re.compile(rb'NOSTR_SIGNER_TOKEN='), re.compile(rb'AUTH [0-9a-f]{64}'),
         re.compile(rb'(?<![0-9A-Za-z])p[1-9][0-9]{0,4}_[0-9a-f]{64}(?![0-9a-f])'))


class ProofError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise ProofError(message)


def _json(path):
    try:
        return json.loads(Path(path).read_bytes())
    except (OSError, ValueError) as exc:
        raise ProofError(f'unreadable JSON {path}: {exc}') from exc


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


def lane(evidence, profile, sha):
    """The exact single first-attempt backend receipt of a signer lane."""
    found = [path for path in Path(evidence).rglob('attempt-1/receipt.json')
             if (raw := _json(path)).get('profile_id') == profile
             and raw.get('source_identity') == {'kind': 'committed', 'sha': sha}]
    require(len(found) == 1, f'expected exactly one {profile} attempt-1 backend receipt at {sha}, found {len(found)}')
    require(not any(Path(evidence).rglob('attempt-2/receipt.json')), 'retried lane cannot prove first-attempt delivery')
    return found[0].parent, _json(found[0])


def verify_lane(directory, raw):
    require('signer_endpoint' in raw.get('required_capabilities', []), 'lane did not provision the signer')
    require(raw.get('outcome') == 'passed' and raw.get('ctest_exit_code') == 0 and raw.get('attempt') == 1,
            'backend lane did not pass on its first attempt')
    require(raw.get('observed_executions', {}).get(SIGNER_CONSUMER) == 'passed', 'consumer execution not passed')
    state = raw.get('signer_runtime') or {}
    delivery = state.get('delivery') or {}
    endpoint = (state.get('readiness') or {}).get('endpoint')
    require(state.get('status') == 'passed' and state.get('approval_snapshot') == APPROVED,
            'real fixture did not observe exactly one explicit approved request')
    require(isinstance(endpoint, str) and re.fullmatch(r'tcp:127\.0\.0\.1:[1-9][0-9]{0,4}', endpoint),
            'fixture readiness endpoint missing')
    require(delivery.get('mechanism') == SIGNER_DELIVERY and delivery.get('resource_type') == SIGNER_RESOURCE_TYPE
            and delivery.get('slots') == 1 and delivery.get('registrations') == [SIGNER_CONSUMER]
            and delivery.get('endpoint') == endpoint and delivery.get('removed') is True
            and delivery.get('ctest_environment_credentials') is False,
            'credentials were not delivered as the removed registration-scoped resource')
    spec = str(delivery.get('resource_spec_file'))
    require(not spec.startswith(str(raw.get('evidence_dir', '\0'))), 'resource spec placed inside lane evidence')
    ctest = [argv for argv in raw.get('commands', []) if argv and argv[0] == 'ctest' and '--output-junit' in argv]
    require(len(ctest) == 1, 'not exactly one executing CTest process')
    argv = ctest[0]
    require(argv.count('--resource-spec-file') == 1 and argv[argv.index('--resource-spec-file') + 1] == spec,
            'CTest argv lacks the exact resource spec')
    env = raw.get('ctest_environment') or {}
    require(not set(env.get('overlay_names', [])) & set(SIGNER_AMBIENT), 'signer credentials overlay the CTest environment')
    if 'docker' not in raw.get('required_capabilities', []):
        require(env.get('overlay_names') == [] and env.get('effective_hash') == env.get('base_hash'),
                'CTest environment differs from the pre-provisioning test environment')
    junit = (directory / 'results.xml').read_bytes()
    cases = _cases(junit)
    require(_ran(cases.get(SIGNER_CONSUMER)), 'consumer JUnit case is not a real passing run (skip/not-run/failure)')
    missing = [name for name in WITNESSES if not _ran(cases.get(name))]
    require(not missing, 'isolation witnesses did not run and pass in the same CTest process: ' + repr(missing))
    port = endpoint.rsplit(':', 1)[1].encode()
    for name in (directory / 'ctest.log', directory / 'results.xml'):
        require(b'127.0.0.1:' + port not in name.read_bytes(), 'fixture endpoint appears in CTest output: ' + name.name)
    return dict(endpoint=endpoint, consumer='passed', witnesses=list(WITNESSES), ctest_argv_has_spec=True,
                approval=APPROVED, overlay_names=env.get('overlay_names'))


def verify_receipt(receipts, profile, sha):
    from test_selection_execution import KIND
    found = [path for path in Path(receipts).rglob('receipt.json') if _json(path).get('profile') == profile]
    require(len(found) == 1, f'expected exactly one qualified {profile} receipt, found {len(found)}')
    result = _json(found[0])
    require(result.get('receipt_kind') == KIND and result.get('job_conclusion') == 'success'
            and result.get('source_identity') == {'kind': 'committed', 'sha': sha}
            and SIGNER_CONSUMER in result.get('executed_registrations', [])
            and (found[0].parent / 'original-native-receipt.json').is_file(),
            'no post-namespace-exit qualified receipt (worker-qualify) for the lane')
    return str(found[0])


def scan(*roots):
    """No credential form in any retained byte, including the uploaded build tree."""
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


def verify(receipts, evidence, profile, sha):
    require(profile in PROFILES, 'not a signer-provisioning profile: ' + profile)
    require(re.fullmatch(r'[0-9a-f]{40}', sha), 'exact 40-hex tested SHA required')
    directory, raw = lane(evidence, profile, sha)
    proof = verify_lane(directory, raw)
    proof['qualified_receipt'] = verify_receipt(receipts, profile, sha)
    proof['scanned_files'] = scan(receipts, evidence)
    return dict(proved=True, profile=profile, sha=sha, **proof,
                scope='hosted pidns delivery/isolation for one lane; not profile, complete-gate or W11 acceptance')


def collect(run, profile, out, repo, execute):
    out = Path(out)
    require(profile in PROFILES, 'not a signer-provisioning profile: ' + profile)
    commands = [['gh', 'run', 'download', str(run), '-R', repo, '-p', f'dependency-{kind}-*-{profile}-*',
                 '-D', str(out / kind)] for kind in ('receipt', 'evidence')]
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
    v.add_argument('--receipts', required=True)
    v.add_argument('--evidence', required=True)
    v.add_argument('--profile', required=True)
    v.add_argument('--sha', required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == 'collect':
            result = {'commands': collect(args.run, args.profile, args.out, args.repo, args.execute)}
        else:
            result = verify(args.receipts, args.evidence, args.profile, args.sha)
    except ProofError as exc:
        print(json.dumps({'proved': False, 'refusal': str(exc)}, indent=2))
        return 1
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == '__main__':
    sys.exit(main())
