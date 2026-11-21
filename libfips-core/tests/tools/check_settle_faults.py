#!/usr/bin/env python3
"""Kill each implemented C settlement mutation with a real invariant assertion."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
try:
    import resource
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
except ImportError:
    pass

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--runner', required=True)
p.add_argument('--unit', required=True)
a = p.parse_args()
core = Path(__file__).resolve().parents[2]

def run(args):
    return subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          encoding='utf-8', errors='replace', timeout=15)

def positive(args):
    result = run(args)
    if result.returncode:
        sys.exit(f'baseline failed: {args}\n{result.stdout}\n{result.stderr}')
    return result.stdout

positive([sys.executable, '-I', str(Path(__file__).with_name('generate_settle_faults.py')), '--check'])
known = set(json.loads((core / 'tests/settle_fault_origins.json').read_text())['faults'])
names = positive([a.runner, '--list-faults']).splitlines()
assert names and len(set(names)) == len(names) and set(names) <= known
assert positive([a.unit, '--list-faults']).splitlines() == names
positive([a.runner])
positive([a.unit])
failed = []
for name in names:
    killed = False
    details = []
    for label, runner in [('diff', a.runner), ('unit', a.unit)]:
        result = run([runner, '--fault', name])
        diagnostic = result.stderr
        assertion = (re.search(r'(?m)^(settle|gates|table|join|model|parity|faults):\d+:', diagnostic)
                     or ('actual: ' in diagnostic and '\nexpected: ' in diagnostic)
                     or 'unchecked settle deadline' in diagnostic
                     or ('Assertion' in diagnostic and '/libfips-core/tests/' in diagnostic))
        unsafe = ('ERROR: AddressSanitizer' in diagnostic
                  or 'runtime error:' in diagnostic
                  or 'WARNING: ThreadSanitizer' in diagnostic)
        if result.returncode and assertion and not unsafe:
            print(f'killed {name}: {label}', flush=True)
            killed = True
            break
        details.append(f'{label}: exit={result.returncode}\n{diagnostic[-3000:]}')
    if not killed:
        failed.append(name)
        print(f'SURVIVED/INVALID {name}\n' + '\n'.join(details), flush=True)
print(f'{len(names)-len(failed)}/{len(names)} mutations killed; both unmutated baselines passed; no skipped mutations')
if failed:
    sys.exit(1)
