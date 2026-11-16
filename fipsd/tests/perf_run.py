#!/usr/bin/env python3
"""Collect sequential Release micro and loopback runs into one JSON artifact."""
import argparse
import csv
import io
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys

BENCHES = ('noise_aead', 'routing_next_hop', 'coord_cache', 'transport_addr')


def machine():
    if sys.platform == 'darwin':
        cpu = subprocess.check_output(['sysctl', '-n', 'machdep.cpu.brand_string'], text=True).strip()
    elif sys.platform.startswith('linux'):
        cpu = next((line.split(':', 1)[1].strip() for line in Path('/proc/cpuinfo').read_text().splitlines()
                    if line.startswith('model name')), platform.processor())
    else:
        cpu = platform.processor()
    return {'system': platform.system(), 'release': platform.release(), 'arch': platform.machine(),
            'cpu': cpu, 'logical_cpus': os.cpu_count()}


def build_info(build):
    cache = {}
    for line in (build / 'CMakeCache.txt').read_text().splitlines():
        if '=' in line and ':' in line and not line.startswith(('//', '#')):
            key, value = line.split('=', 1)
            cache[key.split(':', 1)[0]] = value
    if cache.get('CMAKE_BUILD_TYPE') != 'Release':
        raise ValueError('performance measurements require a Release build')
    if any(cache.get(f'GO_ENABLE_{kind}') == 'ON' for kind in ('ASAN', 'UBSAN', 'TSAN')):
        raise ValueError('sanitizer timings are not a performance baseline')
    return {key: cache.get(key, '') for key in ('CMAKE_BUILD_TYPE', 'CMAKE_C_COMPILER',
            'CMAKE_C_FLAGS', 'CMAKE_C_FLAGS_RELEASE', 'GO_WARNINGS_AS_ERRORS')}


def micro_run(build, iterations):
    result = {}
    for bench in BENCHES:
        output = subprocess.check_output([str(build / 'libfips-core/bench' / f'fips_bench_{bench}')],
                                          env=dict(os.environ, FIPS_BENCH_ITERS=str(iterations)), text=True, timeout=600)
        for row in csv.reader(io.StringIO(output)):
            if len(row) != 4 or row[0] in result:
                raise ValueError(f'invalid or duplicate CSV row: {row}')
            result[row[0]] = dict(zip(('median_ns', 'p95_ns', 'ops_per_s'), map(float, row[1:])))
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', type=Path, required=True)
    p.add_argument('--rust', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--work-dir', type=Path, required=True)
    p.add_argument('--runs', type=int, default=3)
    p.add_argument('--iterations', type=int, default=1000)
    p.add_argument('--seconds', type=float, default=5)
    p.add_argument('--latency-samples', type=int, default=1000)
    p.add_argument('--source-commit', required=True)
    p.add_argument('--rust-commit', required=True, help='verified source revision used to build --rust')
    args = p.parse_args()
    if args.runs < 3 or args.iterations < 1:
        p.error('at least 3 runs and positive iterations required')
    if not re.fullmatch(r'[0-9a-f]{40}', args.rust_commit):
        p.error('--rust-commit must be the full verified source commit')
    args.build = args.build.resolve(strict=True)
    args.output = args.output.resolve()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    config = {'runs': args.runs, 'iterations': args.iterations, 'seconds': args.seconds,
              'latency_samples': args.latency_samples, 'payload_bytes': 1200, 'window': 0,
              'workers': 2}
    result = {'schema': 1, 'machine': machine(), 'source_commit': args.source_commit, 'config': config,
              'rust_commit': args.rust_commit, 'build': build_info(args.build), 'micro_runs': []}
    for i in range(args.runs):
        print(f'micro run {i + 1}', file=sys.stderr, flush=True)
        result['micro_runs'].append(micro_run(args.build, args.iterations))
    loopback = args.output.with_suffix('.loopback.json')
    subprocess.run([sys.executable, str(Path(__file__).with_name('perf_loopback.py')),
                    '--fipsd', str(args.build / 'fipsd/fipsd'), '--rust', str(args.rust.resolve(strict=True)),
                    '--native-tool', str(args.build / 'fipsd/tests/fipsd_perf_native'),
                    '--output', str(loopback), '--work-dir', str(args.work_dir.resolve()),
                    '--runs', str(args.runs), '--seconds', str(args.seconds),
                    '--latency-samples', str(args.latency_samples)], check=True)
    result['loopback'] = json.loads(loopback.read_text())
    from perf_compare import aggregate
    aggregate(result)  # Refuse partial/invalid results before publishing.
    args.output.write_text(json.dumps(result, indent=2, allow_nan=False) + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
