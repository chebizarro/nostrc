#!/usr/bin/env python3
"""Counterfactual checks: quarantine never disables a test or hides a sanitizer."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix='grotto-quarantine-') as tmp:
    tmp = Path(tmp)
    for case, name, message, expected in (
        ('known', 'groundhog-nip29-service', 'known baseline failure', 0),
        ('unknown', 'new-signer-regression', 'new failure', 1),
        ('asan', 'groundhog-nip29-service', 'ERROR: AddressSanitizer: use-after-free', 1),
        ('ubsan', 'groundhog-nip29-service', 'file.c:1: runtime error: overflow', 1),
        ('tsan', 'groundhog-nip29-service', 'WARNING: ThreadSanitizer: data race', 1),
        ('skip', 'optional-prerequisite', 'Prerequisite unavailable', 0),
    ):
        source = tmp / case
        source.mkdir()
        (source / 'CMakeLists.txt').write_text(
            'cmake_minimum_required(VERSION 3.22)\nproject(quarantine NONE)\nenable_testing()\n'
            f'add_test(NAME {name} COMMAND "{sys.executable}" -c '
            f'"print(\'{message}\'); raise SystemExit({77 if case == "skip" else 1})")\n'
            f'set_tests_properties({name} PROPERTIES SKIP_RETURN_CODE 77)\n')
        build = source / 'build'
        subprocess.run(['cmake', '-S', str(source), '-B', str(build)], check=True, capture_output=True)
        env = dict(os.environ, BUILD_DIR=str(build), STATE_DIR=str(source / 'state'),
                   TEST_REGEX=f'^{name}$', JOBS='1', DISPLAY_WRAP='0', CTEST_TIMEOUT='',
                   RERUN_MAX='0', NOSTRC_CTEST_QUARANTINE_PLATFORM='linux',
                   NOSTRC_CTEST_QUARANTINE_FILE=str(root / 'scripts/grotto-ci-quarantine.tsv'))
        result = subprocess.run(['bash', str(root / 'scripts/linux-gate-smoke.sh')],
                                env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        assert (result.returncode == 0) == (expected == 0), result.stdout
        first = (source / 'state/ctest.log').read_text()
        if case == 'skip':
            assert '***Skipped' in first, 'The optional process must return its skip code'
        else:
            assert message in first, 'The original failing test must actually execute'
        ledger = source / 'state/gate-history/quarantined.tsv'
        assert ledger.exists() == (case == 'known'), result.stdout
        if case == 'skip':
            assert '1 selected, 0 passed, 1 skipped' in result.stdout, result.stdout
        if ledger.exists():
            assert 'nostrc-q1974' in ledger.read_text()
        print(f'{case}: expected result, original test output preserved')
print('Grotto named quarantine checks passed')
