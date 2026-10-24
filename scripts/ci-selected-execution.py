#!/usr/bin/env python3
"""Execute the shared plan's ordinary CI checks; no profile/release attestation."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

import test_selection as selection

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / '.github/workflows'))
import ci_selection

TSAN_TESTS = {'signer/bunker', 'signer/bunker-e2e', 'nip55l_approval_service',
              'nip55l_cache_concurrency'}


def selected_checks(plan, policy, profile):
    """Membership comes from the shared lane, not family or prerequisite membership."""
    return {ident: policy['checks'][ident] for lane in plan['lanes']
            if lane['profile'] == profile for ident in lane['checks']}


def test_names(checks, profile):
    return sorted({name for check in checks.values() if check['kind'] == 'ctest'
                   for name in check['ctest_tests'][profile]})


def load_handoff(path, root=ROOT):
    saved = json.loads(path.read_text())
    policy = selection.load_inventory(root / 'tests/selection/inventory.json')
    plan = selection.validate_plan(saved['shared_plan'], policy)
    selection.require(plan['context'] == 'ci' and not plan['partial_profiles'],
                      'CI handoff must be an unrestricted CI plan')
    selection.require(plan['source_identity']['kind'] == 'committed' and
                      selection.commit_sha(root, 'HEAD') == plan['tested_sha'],
                      'selected execution checkout differs from shared plan')
    selection.require(saved['dispatch'] == ci_selection.project(plan),
                      'dispatch differs from shared plan')
    return plan, policy, saved['dispatch']


def extra_steps(plan, profile, names=()):
    nodes = set(plan['witnesses'])
    # Legacy commands absent from inventory retain their existing bounded runner
    # ownership. A jitter run is a single all-six-sites contract, not a subset test.
    return {
        'lifetime_stress': profile == 'groundhog.linux' and 'execution.groundhog' in nodes,
        'session_keyring': profile == 'groundhog.linux' and
            'groundhog-store-key-keyring' in names,
        'gtk_perf': profile == 'groundhog.linux' and
            bool(nodes & {'nostr.gtk', 'nostr.gtk.portable', 'execution.groundhog'}),
        'grotto_package': profile == 'grotto.linux' and
            bool(nodes & {'packaging.grotto', 'packaging.shared', 'execution.grotto'}),
        'grotto_frontend': profile == 'grotto.linux' and
            bool(nodes & {'grotto.gui', 'execution.grotto'}),
        'shell_extension': profile == 'grotto.linux' and
            bool(nodes & {'gnome.nostr_shell_extension', 'execution.grotto'}),
    }


def exact_regex(names):
    selection.require(bool(names), 'empty CTest selection')
    return '^(' + '|'.join(re.escape(name) for name in names) + ')$'


def execute_tests(build, names, *, prefix=(), parallel=1, stem='ci-selected'):
    """CTest supplies fixture setup/cleanup closure; never select prerequisite suites."""
    if not names:
        print('No CTest checks selected in this runner partition.', flush=True)
        return
    regex = exact_regex(names)
    base = ['ctest', '--test-dir', str(build)]
    data = json.loads(subprocess.check_output(base + ['--show-only=json-v1', '-R', regex], text=True))
    registered = {test['name'] for test in data['tests']}
    selection.require(set(names) <= registered,
                      'Missing selected CTest registrations: ' + ', '.join(sorted(set(names) - registered)))
    (build / (stem + '-registration.json')).write_text(json.dumps(data, indent=2) + '\n')
    junit = (build / (stem + '.xml')).resolve()
    command = list(prefix) + base + ['--parallel', str(parallel), '-R', regex,
                                   '--no-tests=error', '--output-on-failure',
                                   '--output-junit', str(junit)]
    print('+ ' + ' '.join(command), flush=True)
    with (build / (stem + '.log')).open('w') as log:
        with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True) as proc:
            with proc.stdout:
                for line in proc.stdout:
                    print(line, end='', flush=True)
                    log.write(line)
            code = proc.wait()
    selection.require(code == 0, f'selected CTest failed with exit {code}; see {build / (stem + ".log")}')
    cases = ET.parse(junit).getroot().findall('.//testcase')
    ran = {case.attrib['name'] for case in cases}
    selection.require(set(names) <= ran, 'selected CTest execution is incomplete')
    selection.require(not any(case.find('skipped') is not None or case.find('failure') is not None
                              or case.find('error') is not None or case.get('status') == 'notrun'
                              for case in cases), 'selected CTest or fixture skipped, failed or did not run')


def execute_commands(checks, root, build):
    values = {'source': str(root), 'build': str(build.resolve())}
    targets = sorted({target for check in checks.values() for target in check.get('build_targets', [])})
    if targets:
        subprocess.run(['cmake', '--build', str(build), '--parallel', '2', '--target', *targets], check=True)
    for ident, check in sorted(checks.items()):
        if check['kind'] == 'ctest':
            continue
        command = [arg.format(**values) for arg in check['command']]
        environment = os.environ.copy()
        environment.update({key: value.format(**values)
                            for key, value in check.get('environment', {}).items()})
        cwd = check.get('working_directory', '{source}').format(**values)
        print(f'+ {ident}: ' + ' '.join(command), flush=True)
        subprocess.run(command, cwd=cwd, env=environment, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['prepare', 'run'])
    parser.add_argument('--plan', type=Path, required=True)
    parser.add_argument('--profile', required=True,
                        choices=['grotto.linux', 'grotto.linux.asan_ubsan',
                                 'groundhog.linux', 'groundhog.linux.sanitizers'])
    parser.add_argument('--build', type=Path, default=Path('build'))
    parser.add_argument('--tsan', action='store_true', help='existing advisory TSan partition only')
    args = parser.parse_args()
    plan, policy, dispatch = load_handoff(args.plan)
    checks = selected_checks(plan, policy, args.profile)
    names = test_names(checks, args.profile)
    if args.tsan:
        names = sorted(set(names) & TSAN_TESTS)
    if args.command == 'prepare':
        outputs = {'affected': not dispatch['broad'], 'has_checks': bool(checks),
                   'needs_nak': any('nak' in c.get('requires', []) for c in checks.values())
                       or any('nak' in name for name in names),
                   **extra_steps(plan, args.profile, names)}
        with open(os.environ['GITHUB_OUTPUT'], 'a') as output:
            for key, value in outputs.items():
                output.write(f'{key}={json.dumps(value)}\n')
        print(json.dumps({'profile': args.profile, 'checks': sorted(checks),
                          'ctest_names': names, **outputs}, indent=2))
        return
    selection.require(not dispatch['broad'], 'broad runs must use the ordinary broad runner')
    # Keep sanitizer policy from the ordinary workflow; add only authored test env.
    for key, value in policy['profiles'][args.profile].get('environment', {}).items():
        os.environ.setdefault(key, value)
    if not args.tsan:
        execute_commands(checks, ROOT, args.build)
    execute_tests(args.build, names, parallel=1)


if __name__ == '__main__':
    try:
        main()
    except (selection.ContractError, OSError, ValueError, KeyError, subprocess.CalledProcessError,
            ET.ParseError) as exc:
        print(f'Selected CI execution failed: {exc}', file=sys.stderr)
        sys.exit(1)
