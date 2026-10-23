"""Worker apt provisioning covers unconditional signet libmicrohttpd use.

PR23 shadow run 40023343699 (core.linux job 120653339860) failed its selected
prerequisite build at signet/src/bootstrap_server.c: `microhttpd.h: No such
file or directory`. Signet's CMake treats libmicrohttpd as optional only for
the health server; bootstrap_server.c is always compiled into signet_core and
includes <microhttpd.h> unguarded. Every Linux profile that configures the root
tree with Signet enabled therefore needs libmicrohttpd-dev before execution, as
legacy signet-ci installs it. Static wiring regression, not hosted execution.
"""
import copy
import hashlib
import json
import re
import unittest

from selector_support import ROOT, s
from test_worker_signer_prerequisites import EXECUTE, selects, steps
import worker_prerequisite_support as wave

WORKER = '.github/workflows/dependency-worker.yml'
SETUP = 'Install Linux profile prerequisites'
PACKAGE = 'libmicrohttpd-dev'
MHD_INCLUDE = re.compile(r'^\s*#\s*include\s*<microhttpd\.h>', re.M)
CONDITIONAL = re.compile(r'^\s*#\s*(if|ifdef|ifndef)\b', re.M)
ENDIF = re.compile(r'^\s*#\s*endif\b', re.M)


def unguarded_mhd_sources(cmake_text, read):
    """Unconditional signet_core sources including <microhttpd.h> outside any #if."""
    if not re.search(r'^add_library\(signet_core STATIC \$\{SIGNET_CORE_SOURCES\}\)$', cmake_text, re.M):
        raise ValueError('signet_core library not built from SIGNET_CORE_SOURCES')
    listed = re.search(r'^set\(SIGNET_CORE_SOURCES\n(.*?)\n\)$', cmake_text, re.S | re.M)
    if listed is None:
        raise ValueError('signet_core source list not found')
    # Only the SIGNET_ENABLE option guard (modelled by signet_profiles) may enclose it.
    stack = []
    for line in cmake_text[:listed.start()].splitlines():
        opened = re.match(r'\s*if\s*\((.*)\)\s*$', line)
        if opened:
            stack.append(opened.group(1).strip())
        elif re.match(r'\s*endif\s*\(', line):
            stack.pop()
    if stack != ['SIGNET_ENABLE']:
        raise ValueError('signet_core source list is conditional: ' + repr(stack))
    found = []
    for rel in re.findall(r'\$\{CMAKE_CURRENT_SOURCE_DIR\}/(src/[\w./-]+\.c)', listed.group(1)):
        depth = 0
        for line in read(rel).splitlines():
            if CONDITIONAL.match(line):
                depth += 1
            elif ENDIF.match(line):
                depth -= 1
            elif depth == 0 and MHD_INCLUDE.match(line):
                found.append(rel)
                break
    return found


def signet_profiles(inventory):
    """Linux profiles configuring the root tree with the default-ON Signet option."""
    return {name for name, p in inventory['profiles'].items()
            if p.get('platform') == 'linux' and p.get('source_dir') == '.'
            and p.get('configure_args') and '-DSIGNET_ENABLE=OFF' not in p['configure_args']}


def apt_packages(body):
    """Packages named by each backslash-continued `apt-get install` command only."""
    packages = set()
    for command in re.findall(r'apt-get install((?:[^\n]*\\\n)*[^\n]*)', body):
        packages.update(word for word in command.replace('\\', ' ').split() if not word.startswith('-'))
    return packages


def missing(worker_text, inventory, required):
    """Profiles lacking `required` in a pre-execution, unmasked Linux setup step."""
    found = steps(worker_text)
    names = [step['name'] for step in found]
    if EXECUTE not in names:
        return ['execution step missing']
    execute = names.index(EXECUTE)
    errors = []
    for profile in sorted(signet_profiles(inventory)):
        covering = [i for i, step in enumerate(found)
                    if i < execute and selects(step['if'], profile) and required in apt_packages(step['body'])
                    and 'continue-on-error' not in step['body'] and '|| true' not in step['body']]
        if len(covering) != 1:
            errors.append(f'{profile}: needs exactly one pre-execution {required} install, got {len(covering)}')
    return errors


class WorkerMicrohttpdPrerequisiteTests(unittest.TestCase):
    def setUp(self):
        self.worker = (ROOT / WORKER).read_text()
        self.inventory = json.loads((ROOT / 'tests/selection/inventory.json').read_text())
        self.cmake = (ROOT / 'signet/CMakeLists.txt').read_text()

    def read(self, rel):
        return (ROOT / 'signet' / rel).read_text()

    def test_requirement_is_derived_from_actual_signet_source(self):
        self.assertEqual(unguarded_mhd_sources(self.cmake, self.read), ['src/bootstrap_server.c'])
        self.assertEqual(signet_profiles(self.inventory), {'core.linux', 'groundhog.mdk08'})
        # Legacy signet CI provisions the same development package.
        self.assertIn(PACKAGE, apt_packages((ROOT / '.github/workflows/signet-ci.yml').read_text()))
        self.assertEqual(apt_packages('apt-get install -y a \\\n  b\necho libmicrohttpd-dev\n'), {'a', 'b'})

    def test_every_signet_profile_is_provisioned_before_execution(self):
        self.assertEqual(missing(self.worker, self.inventory, PACKAGE), [])
        setup = next(step for step in steps(self.worker) if step['name'] == SETUP)
        self.assertEqual(setup['if'], "runner.os == 'Linux'")

    def test_hosted_failing_worker_bytes_lack_the_package(self):
        raw = (ROOT / WORKER).read_bytes()
        failing = wave.before_source(WORKER, raw).decode()
        self.assertEqual(missing(failing, self.inventory, PACKAGE), [
            f'core.linux: needs exactly one pre-execution {PACKAGE} install, got 0',
            f'groundhog.mdk08: needs exactly one pre-execution {PACKAGE} install, got 0'])
        self.assertEqual(wave.witness()['hosted_failure']['missing_header'], 'microhttpd.h')

    def test_removed_masked_late_or_duplicated_install_is_refused(self):
        removed = self.worker.replace(' ' + PACKAGE, '', 1)
        self.assertEqual(len(missing(removed, self.inventory, PACKAGE)), 2)
        masked = self.worker.replace('pkg-config --modversion libpeas-2',
                                     'pkg-config --modversion libpeas-2 || true', 1)
        self.assertEqual(len(missing(masked, self.inventory, PACKAGE)), 2)
        late = removed + (f"      - name: Late\n        run: sudo apt-get install -y {PACKAGE}\n")
        self.assertEqual(len(missing(late, self.inventory, PACKAGE)), 2)
        execute = '      - name: ' + EXECUTE
        duplicate = self.worker.replace(
            execute, f"      - name: Again\n        run: sudo apt-get install -y {PACKAGE}\n" + execute, 1)
        self.assertEqual(len(missing(duplicate, self.inventory, PACKAGE)), 2)

    def test_guarded_include_or_disabled_signet_carries_no_requirement(self):
        source = self.read('src/bootstrap_server.c')
        guarded = source.replace('#include <microhttpd.h>',
                                 '#ifdef SIGNET_HAVE_MHD\n#include <microhttpd.h>\n#endif', 1)
        reader = lambda rel: guarded if rel == 'src/bootstrap_server.c' else self.read(rel)
        self.assertEqual(unguarded_mhd_sources(self.cmake, reader), [])
        inventory = json.loads(json.dumps(self.inventory))
        inventory['profiles']['core.linux']['configure_args'].append('-DSIGNET_ENABLE=OFF')
        self.assertEqual(signet_profiles(inventory), {'groundhog.mdk08'})
        with self.assertRaises(ValueError):
            unguarded_mhd_sources('add_executable(signetd main.c)', self.read)
        conditional = self.cmake.replace('set(SIGNET_CORE_SOURCES\n', 'if(SIGNET_HTTP)\nset(SIGNET_CORE_SOURCES\n', 1)
        with self.assertRaises(ValueError):
            unguarded_mhd_sources(conditional, self.read)


class WorkerPrerequisiteEnrollmentTests(unittest.TestCase):
    """The finite inverse reverses exactly this repair; it admits nothing."""

    def policy(self):
        return s.load_inventory(ROOT / 'tests/selection/inventory.json')

    def test_exact_policy_inverse_reaches_base_and_is_idempotent(self):
        policy = self.policy()
        data = wave.witness()
        self.assertEqual(s.digest(policy), data['enrolled_policy_hash'])
        before = wave.before_policy(policy)
        self.assertEqual(s.digest(before), data['base_policy_hash'])
        self.assertEqual(wave.before_policy(before), before)
        self.assertFalse(policy['enforcement_ready'])
        for ident, profile in policy['profiles'].items():
            self.assertEqual(profile, before['profiles'][ident])

    def test_exact_source_inverse_and_unreviewed_byte_refusal(self):
        import post1640_integration_support as older
        for path, record in wave.witness()['sources'].items():
            with self.subTest(path=path):
                raw = (ROOT / path).read_bytes()
                self.assertEqual(hashlib.sha256(raw).hexdigest(), record['after_sha256'])
                before = wave.before_source(path, raw)
                self.assertEqual(hashlib.sha256(before).hexdigest(), record['before_sha256'])
                self.assertEqual(wave.before_source(path, before), before)
                # Unreviewed bytes pass through here and the strict older inverse refuses.
                drifted = raw + b'\n# unreviewed source\n'
                self.assertEqual(wave.before_source(path, drifted), drifted)
                if path in older.witness()['sources']:
                    with self.assertRaises(ValueError):
                        older.before_source(path, drifted)

    def test_jobs_inverse_projects_only_the_exact_reviewed_hashes(self):
        from profile_contract_support import source_job_blocks, validate_job_accounting
        policy = self.policy()
        validate_job_accounting(policy, ROOT)
        actual = source_job_blocks(ROOT)
        for ident, record in wave.witness()['maps']['workflow_jobs'].items():
            for field in ('source_sha256', 'workflow_sha256', 'matrix_source'):
                self.assertEqual(actual[ident][field], record['after'][field])
            before = wave.before_jobs(actual)
            self.assertEqual(before[ident], {key: record['before'][key] for key in actual[ident]})
            bad = copy.deepcopy(actual)
            bad[ident]['source_sha256'] = '0' * 64
            self.assertEqual(wave.before_jobs(bad)[ident], bad[ident])

    def test_missing_duplicate_mutated_and_mixed_enrollment_refuse(self):
        data = wave.witness()
        for path in data['runtime_inputs']:
            for duplicate in (False, True):
                policy = self.policy()
                inputs = policy['checks']['selection.contract']['runtime_inputs']
                inputs.append(path) if duplicate else inputs.remove(path)
                with self.subTest(path=path, duplicate=duplicate), self.assertRaises(ValueError):
                    wave.before_policy(policy)
        for field, records in data['maps'].items():
            for ident in records:
                policy = self.policy()
                policy[field][ident]['unreviewed'] = True
                with self.subTest(field=field, ident=ident), self.assertRaises(ValueError):
                    wave.before_policy(policy)
        policy = self.policy()
        policy['checks']['selection.contract']['runtime_inputs'] = [
            path for path in policy['checks']['selection.contract']['runtime_inputs']
            if path not in data['runtime_inputs']]
        with self.assertRaises(ValueError):
            wave.before_policy(policy)

    def test_new_runtime_inputs_have_live_command_ownership(self):
        policy = self.policy()
        inputs = policy['checks']['selection.contract']['runtime_inputs']
        for path in wave.witness()['runtime_inputs']:
            with self.subTest(path=path):
                self.assertEqual(inputs.count(path), 1)
                relative = path.removeprefix('{source}/')
                self.assertTrue((ROOT / relative).is_file())
                classified = s.classify(policy, relative)
                self.assertEqual(classified['effect'], 'global')
                self.assertIn('selection.contract', classified['nodes'])


if __name__ == '__main__':
    unittest.main()
