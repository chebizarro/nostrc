"""Dependency worker provisions pinned signer-producer prerequisites per profile.

B's normal-source checkpoint (a786f19e9) enables real signer producers that need
pinned Nak for both original MDK profiles and pinned Blueprint for core ASan and
MDK011. This finite regression checks the worker's literal step conditions
against the authored inventory: every profile is provisioned exactly once,
before execution, with the existing pinned installer and version checks. It is
static wiring evidence, not hosted execution.
"""
import json
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
WORKER = ROOT / '.github/workflows/dependency-worker.yml'
INVENTORY = ROOT / 'tests/selection/inventory.json'

NAK_INSTALL = "go install -ldflags='-X main.version=v0.19.7' github.com/fiatjaf/nak@v0.19.7"
NAK_PIN = "grep -Eq '(^|[[:space:]])v?0\\.19\\.7([[:space:]]|$)'"
BLUEPRINT_PIN = ('blueprint-compiler-0.22.2.tar.xz',
                 '231d72efbac931c235ac3e022fe94982095c20d88721d9a8dcf60152f2017e07',
                 'sha256sum -c', 'blueprint-compiler --version | grep -x 0.22.2')
# B checkpoint: core ASan and MDK011 newly need Blueprint, in addition to the
# application profiles that already had it.
BLUEPRINT_PROFILES = {'core.linux', 'core.linux.asan', 'grotto.linux', 'grotto.linux.asan_ubsan',
                      'groundhog.mdk08', 'groundhog.mdk011'}
EXECUTE = 'Resolve, audit, build and execute exact selected checks'
TERM = re.compile(r"inputs\.profile == '([a-z0-9._]+)'|startsWith\(inputs\.profile, '([a-z0-9._]+)'\)"
                  r"|runner\.os == '(Linux|macOS)'|&&|\|\||\(|\)|\s+")


def steps(text):
    """(name, if, uses, body) for each step of the single worker job, in order."""
    out, current = [], None
    for line in text.splitlines():
        if line.startswith('      - '):
            current = {'name': None, 'if': None, 'uses': None, 'body': []}
            out.append(current)
            line = '        ' + line[8:]
        if current is None:
            continue
        current['body'].append(line)
        for key in ('name', 'if', 'uses'):
            if line.startswith(f'        {key}: '):
                current[key] = line[len(f'        {key}: '):]
    return [dict(step, body='\n'.join(step['body'])) for step in out]


def selects(condition, profile, os_name='Linux'):
    """Evaluate the reviewed literal condition grammar; anything else refuses."""
    if condition is None:
        return True
    python, pos = [], 0
    while pos < len(condition):
        match = TERM.match(condition, pos)
        if not match:
            raise ValueError(f'unreviewed condition syntax: {condition!r}')
        token = match.group(0)
        if match.group(1):
            python.append(repr(profile == match.group(1)))
        elif match.group(2):
            python.append(repr(profile.startswith(match.group(2))))
        elif match.group(3):
            python.append(repr(os_name == match.group(3)))
        elif token in ('&&', '||'):
            python.append(' and ' if token == '&&' else ' or ')
        elif not token.isspace():
            python.append(token)
        pos = match.end()
    return bool(eval(''.join(python), {'__builtins__': {}}, {}))  # only True/False/and/or/()


def coverage_errors(text, inventory):
    errors, found = [], steps(text)
    names = [step['name'] for step in found]
    if EXECUTE not in names:
        return ['execution step missing']
    execute = names.index(EXECUTE)
    linux = {name for name, p in inventory['profiles'].items() if p.get('platform') == 'linux'}
    nak_profiles = {name for name in linux if 'nak' in inventory['profiles'][name].get('capabilities', [])}
    go_steps = [i for i, step in enumerate(found) if (step['uses'] or '').startswith('actions/setup-go@')]
    nak_steps = [i for i, step in enumerate(found) if NAK_INSTALL in step['body']]
    blueprint_steps = [i for i, step in enumerate(found) if all(pin in step['body'] for pin in BLUEPRINT_PIN)]
    for i in go_steps + nak_steps + blueprint_steps:
        if 'continue-on-error' in found[i]['body'] or '|| true' in found[i]['body']:
            errors.append(f'{found[i]["name"]}: failure can be masked')
        if i > execute:
            errors.append(f'{found[i]["name"]}: runs after execution')
    for profile in sorted(nak_profiles):
        go = [i for i in go_steps if selects(found[i]['if'], profile)]
        nak = [i for i in nak_steps if selects(found[i]['if'], profile)]
        if len(go) != 1 or len(nak) != 1:
            errors.append(f'{profile}: needs exactly one Go and one pinned nak install, got {len(go)}/{len(nak)}')
        elif go[0] > nak[0]:
            errors.append(f'{profile}: Go toolchain after nak install')
    for i in nak_steps:
        # Pinned either in the step itself or by a later, pre-execution
        # ordinary-tools verifier that covers every profile the step installs.
        served = {p for p in linux if selects(found[i]['if'], p)}
        verified = NAK_PIN in found[i]['body'] or any(
            'ordinary-tools' in found[j]['body'] and all(selects(found[j]['if'], p) for p in served)
            for j in range(i + 1, execute))
        if not verified:
            errors.append(f'{found[i]["name"]}: nak version pin is not verified')
    for profile in sorted(BLUEPRINT_PROFILES):
        if profile not in linux:
            errors.append(f'{profile}: not an authored Linux profile')
        covering = [i for i in blueprint_steps if selects(found[i]['if'], profile)]
        if len(covering) != 1:
            errors.append(f'{profile}: needs exactly one pinned Blueprint install, got {len(covering)}')
    return errors


class WorkerSignerPrerequisiteTests(unittest.TestCase):
    def setUp(self):
        self.text = WORKER.read_text()
        self.inventory = json.loads(INVENTORY.read_text())

    def test_every_signer_producer_profile_is_provisioned_once_before_execution(self):
        self.assertEqual(coverage_errors(self.text, self.inventory), [])

    def test_both_original_mdk_profiles_author_nak(self):
        for profile in ('groundhog.mdk08', 'groundhog.mdk011'):
            self.assertIn('nak', self.inventory['profiles'][profile]['capabilities'])
            self.assertEqual(self.inventory['profiles'][profile]['platform'], 'linux')

    def test_ordinary_only_nak_steps_and_verifier_are_unchanged(self):
        ordinary = "if: runner.os == 'Linux' && (inputs.profile == 'groundhog.linux' || inputs.profile == 'grotto.linux')"
        for name in ('Go toolchain for ordinary runtime peers', 'Install pinned nak for ordinary runtime peers',
                     'Verify ordinary peer and keyring executable setup'):
            step = next(s for s in steps(self.text) if s['name'] == name)
            self.assertIn(ordinary, step['body'])

    def test_regression_detects_missing_duplicate_and_misordered_provisioning(self):
        mutants = {
            'mdk011 Blueprint dropped': (" || inputs.profile == 'groundhog.mdk011')", ')', 'groundhog.mdk011: needs exactly one pinned Blueprint'),
            'core ASan Blueprint dropped': (" || inputs.profile == 'core.linux.asan'", '', 'core.linux.asan: needs exactly one pinned Blueprint'),
            'mdk08 nak dropped': ("(inputs.profile == 'groundhog.mdk08' || inputs.profile == 'groundhog.mdk011')",
                                  "(inputs.profile == 'groundhog.mdk011')", 'groundhog.mdk08: needs exactly one Go'),
            'mdk nak duplicated into ordinary steps': ("(inputs.profile == 'groundhog.linux' || inputs.profile == 'grotto.linux')",
                                                       "(inputs.profile == 'groundhog.linux' || inputs.profile == 'grotto.linux' || startsWith(inputs.profile, 'groundhog.mdk'))",
                                                       'groundhog.mdk08: needs exactly one Go and one pinned nak install, got 2/2'),
            'nak pin check removed': ("printf '%s\\n' \"$version\" | grep -Eq '(^|[[:space:]])v?0\\.19\\.7([[:space:]]|$)'",
                                      'true', 'nak version pin is not verified'),
        }
        for label, (old, new, expected) in mutants.items():
            with self.subTest(mutant=label):
                self.assertIn(old, self.text)
                mutated = self.text.replace(old, new)
                errors = coverage_errors(mutated, self.inventory)
                self.assertTrue(any(expected in error for error in errors), errors)
        moved = steps(self.text)
        nak = next(s for s in moved if s['name'] == 'Install pinned nak for original MDK signer producers')
        late = self.text.replace(nak['body'] + '\n', '', 1) + nak['body'] + '\n'
        self.assertIn('runs after execution', ' '.join(coverage_errors(late, self.inventory)))

    def test_checkpoint_worker_lacked_exactly_these_prerequisites(self):
        """The B checkpoint's worker (a786f19e9) fails this regression for the
        reason B recorded; recorded bytes keep the proof independent of git."""
        current = ("        if: runner.os == 'Linux' && (inputs.profile == 'core.linux' || inputs.profile == 'core.linux.asan'"
                   " || startsWith(inputs.profile, 'grotto.linux') || inputs.profile == 'groundhog.mdk08'"
                   " || inputs.profile == 'groundhog.mdk011')\n")
        checkpoint = ("        if: runner.os == 'Linux' && (inputs.profile == 'core.linux'"
                      " || startsWith(inputs.profile, 'grotto.linux') || inputs.profile == 'groundhog.mdk08')\n")
        # Start from the pre-.36.12/.36.15 worker: only that exact reviewed repair is reversed.
        import worker_prerequisite_support as newer
        from w11_batch2_integration_support import before_source as before_batch2_source
        path = '.github/workflows/dependency-worker.yml'
        text = newer.before_source(path, before_batch2_source(path, WORKER.read_bytes())).decode()
        self.assertEqual(text.count(current), 1)
        original = text.replace(current, checkpoint)
        start = original.index('      # Both original MDK profiles author the nak capability')
        end = original.index('      - name: Install pinned Blueprint compiler for application profiles')
        original = original[:start] + original[end:]
        errors = coverage_errors(original, self.inventory)
        self.assertEqual(sorted(errors), sorted([
            'core.linux.asan: needs exactly one pinned Blueprint install, got 0',
            'groundhog.mdk011: needs exactly one pinned Blueprint install, got 0',
            'groundhog.mdk011: needs exactly one Go and one pinned nak install, got 0/0',
            'groundhog.mdk08: needs exactly one Go and one pinned nak install, got 0/0']))

    def test_condition_grammar_refuses_unreviewed_expressions(self):
        self.assertTrue(selects("runner.os == 'Linux' && (inputs.profile == 'x' || startsWith(inputs.profile, 'y.'))", 'y.z'))
        self.assertFalse(selects("runner.os == 'Linux' && inputs.profile == 'x'", 'x', os_name='macOS'))
        for bad in ("contains(inputs.profile, 'mdk')", "inputs.profile != 'x'", "__import__('os')"):
            with self.subTest(condition=bad), self.assertRaises(ValueError):
                selects(bad, 'x')


if __name__ == '__main__':
    unittest.main()
