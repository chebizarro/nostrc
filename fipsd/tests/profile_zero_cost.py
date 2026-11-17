#!/usr/bin/env python3
"""Feature-off machine-code identity (even at -O0) and real archive symbols."""
import argparse
from pathlib import Path
import subprocess
import tempfile

p = argparse.ArgumentParser()
p.add_argument('--cc', required=True)
p.add_argument('--include', required=True)
p.add_argument('--archive', required=True)
a = p.parse_args()
source = '''#include "instr/recorder.h"
#include "instr/writer.h"
#include "node/perf_profile.h"
struct probe_fields { int n; FIPSD_PERF_QUEUE_FIELD FIPSD_PERF_JOB_FIELDS };
_Static_assert(sizeof(struct probe_fields) == sizeof(int), "profiling fields in default build");
int probe(int n) {
#ifdef PLAIN
  ++n;
#else
  FIPSD_INSTR_GATE(on);
  FIPSD_INSTR_ENTRY(on, nonexistent(), nonexistent());
  FIPSD_INSTR_STEP(on, WHOLE_TICK, ++n);
  FIPSD_INSTR_PEER_COUNT(on, nonexistent());
  FIPSD_PERF_BEGIN(t);
  FIPSD_PERF_END(t, FMP_ENCRYPT);
  FIPSD_PERF_START(); FIPSD_PERF_SHUTDOWN(); FIPSD_INSTR_SHUTDOWN();
#endif
  return n;
}
'''
# CTest's working directory is inside the assigned out-of-tree build.
with tempfile.TemporaryDirectory(prefix='zero-cost-', dir=Path.cwd()) as tmp:
    root = Path(tmp)
    for plain in (False, True):
        flags = ['-DPLAIN'] if plain else []
        subprocess.run([a.cc, '-x', 'c', '-', '-c', '-O0', '-std=c11', '-I', a.include,
                        *flags, '-o', str(root / f'{plain}.o')], input=source, text=True,
                       check=True, timeout=20)
    left, right = ((root / f'{v}.o').read_bytes() for v in (False, True))
    assert left == right, 'feature-off object differs from uninstrumented object'
    print(f'feature-off objects byte-identical at -O0: {len(left)} bytes')
symbols = subprocess.check_output(['nm', a.archive], text=True, timeout=20)
assert 'fipsd_instr_' not in symbols and 'fipsd_perf_' not in symbols
print('default fipsd_lib: no profiler symbols')
