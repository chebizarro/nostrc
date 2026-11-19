#!/usr/bin/env python3
"""Fail closed on missing/invalid measurements or a >10% median regression.

By default every metric is gated (local runs, against a same-machine reference
or the absolute baseline). --hosted is the shared-runner policy: it requires a
same-runner --reference, gates loopback goodput and noise-qualified micro
medians, and reports cross-thread micros, loopback latency and CPU without failing.
"""
import argparse
import json
import math
from pathlib import Path
import statistics
import sys


# Empirical shared-runner bounds; derivation and hosted run IDs: docs/perf.md.
HOSTED_MICRO_ABS_FLOOR_NS = 2.0
HOSTED_MICRO_SPREAD_MULTIPLIER = 2.0


def positive(value):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value <= 0:
        raise ValueError(f'expected positive finite measurement, got {value!r}')
    return value


def aggregate(data):
    if data['schema'] != 1 or len(data['micro_runs']) < 3 or len(data['loopback']['runs']) < 3:
        raise ValueError('schema 1 and at least three complete runs are required')
    config = data['config']
    if (type(config['runs']) is not int or config['runs'] < 3
            or len(data['micro_runs']) != config['runs']
            or len(data['loopback']['runs']) != config['runs']):
        raise ValueError('actual run counts differ from declared workload')
    seconds = positive(config['seconds'])
    if (type(config['latency_samples']) is not int or config['latency_samples'] < 100
            or config['payload_bytes'] != 1200 or config['window'] != 0
            or data['loopback']['seconds'] != seconds):
        raise ValueError('invalid or inconsistent loopback workload')
    samples = {}
    expected = None
    for run in data['micro_runs']:
        keys = set(run)
        if expected is not None and keys != expected:
            raise ValueError('micro benchmark names differ across runs')
        expected = keys
        for name, row in run.items():
            for field in ('median_ns', 'p95_ns', 'ops_per_s'):
                positive(row[field])
            if row['p95_ns'] < row['median_ns']:
                raise ValueError(f'{name}: p95 < median')
            samples.setdefault(f'micro/{name}/median_ns', []).append(row['median_ns'])
    if len(expected or ()) != 39:
        raise ValueError('expected all 39 upstream micro workloads')
    for run in data['loopback']['runs']:
        if len(run) != 3 or {row['pairing'] for row in run} != {'c-c', 'rust-rust', 'c-rust'}:
            raise ValueError('all three loopback pairings required in every run')
        for row in run:
            name = f'loopback/{row["pairing"]}'
            if (row['payload_bytes'] != 1200 or row['window'] != 0 or row['loss'] < 0
                    or row['sent'] - row['received'] != row['loss']
                    or row['latency_samples'] != config['latency_samples']
                    or not math.isclose(positive(row['elapsed_s']), seconds, rel_tol=0, abs_tol=1e-9)):
                raise ValueError(f'{name}: workload/delivery validation failed')
            positive(row['received']); positive(row['elapsed_s'])
            throughput = positive(row['throughput_mbps'])
            calculated = row['received'] * 1200 * 8 / row['elapsed_s'] / 1e6
            if not math.isclose(calculated, throughput, rel_tol=1e-5):
                raise ValueError(f'{name}: throughput does not match delivered bytes/time')
            samples.setdefault(f'{name}/throughput_mbps', []).append(throughput)
            for direction in ('one_way', 'reverse_one_way', 'rtt'):
                for percentile in ('p50_us', 'p99_us'):
                    value = positive(row[direction][percentile])
                    samples.setdefault(f'{name}/{direction}/{percentile}', []).append(value)
            if len(row['cpu']) != 2:
                raise ValueError('both daemon CPU measurements required')
            for i, cpu in enumerate(row['cpu']):
                # More CPU during a fixed interval can mean more work, not a
                # regression. Normalize by the delivered throughput payloads.
                value = positive(cpu['traffic_s']) * 1e9 / row['received']
                samples.setdefault(f'{name}/cpu{i}_ns_per_datagram', []).append(value)
    return {name: {'value': statistics.median(values),
                   'direction': 'higher' if name.endswith('/throughput_mbps') else 'lower',
                   **({'spread_ns': max(values) - min(values)} if name.startswith('micro/') else {})}
            for name, values in sorted(samples.items())}


def compare(actual, baseline, tolerance=.10):
    if not 0 <= tolerance <= .10:
        raise ValueError('tolerance must be between 0 and 10%')
    if actual.keys() != baseline.keys():
        raise ValueError(f'metric set changed: missing={baseline.keys() - actual.keys()}, extra={actual.keys() - baseline.keys()}')
    regressions = []
    for name, base in baseline.items():
        current = actual[name]
        b, a = positive(base['value']), positive(current['value'])
        direction = base['direction']
        if current['direction'] != direction or direction not in ('higher', 'lower'):
            raise ValueError(f'{name}: invalid comparison direction')
        bad = a < b * (1 - tolerance) if direction == 'higher' else a > b * (1 + tolerance)
        if bad:
            regressions.append(f'{name}: {b:.3f} -> {a:.3f} ({(a / b - 1) * 100:+.1f}%)')
    return regressions


def hosted_gated(name):
    """Metrics stable enough on shared runners to fail a hosted job."""
    return name.endswith('/throughput_mbps') or (name.startswith('micro/')
            and name.endswith('/median_ns') and '_cross_thread' not in name)


def hosted_compare(actual, reference, tolerance=.10, warn_p50=.50):
    """(regressions in gated metrics, report rows for every metric, warnings)."""
    if actual.keys() != reference.keys():
        raise ValueError('candidate/reference metric sets differ')
    if not 0 <= tolerance <= .10:
        raise ValueError('tolerance must be between 0 and 10%')
    failures, rows, warnings = [], [], []
    for name in sorted(actual):
        b, a = positive(reference[name]['value']), positive(actual[name]['value'])
        delta = a / b - 1
        gated = hosted_gated(name)
        floor = None
        if gated:
            regression = compare({name: actual[name]}, {name: reference[name]}, tolerance)
            if name.startswith('micro/'):
                spreads = [actual[name]['spread_ns'], reference[name]['spread_ns']]
                if any(isinstance(s, bool) or not isinstance(s, (int, float))
                       or not math.isfinite(s) or s < 0 for s in spreads):
                    raise ValueError(f'{name}: expected nonnegative finite run spread')
                # Use within-build ranges, not a pooled candidate/reference range:
                # pooling would count a real between-build shift as noise.
                floor = max(HOSTED_MICRO_ABS_FLOOR_NS,
                            HOSTED_MICRO_SPREAD_MULTIPLIER * max(spreads))
                if a - b <= floor:
                    regression = []
            failures.extend(regression)
        rows.append((name, b, a, delta, gated, floor))
        if name.endswith('/p50_us') and delta > warn_p50:
            warnings.append(f'{name}: {b:.3f} -> {a:.3f} ({delta * 100:+.1f}%)')
    return failures, rows, warnings


def summary_markdown(rows, reference_commit, tolerance):
    lines = [f'### FIPS performance: candidate vs same-runner reference `{reference_commit[:12]}`', '',
             f'Gated: loopback goodput loss >{tolerance:.0%}; non-cross-thread micro median '
             f'increase >{tolerance:.0%} AND >max({HOSTED_MICRO_ABS_FLOOR_NS:g} ns, '
             f'{HOSTED_MICRO_SPREAD_MULTIPLIER:g} × spread). '
             'Spread is the larger candidate/reference max−min range across runs. '
             'Reported only: cross-thread micros, loopback latency percentiles and CPU per datagram '
             '(shared-runner noise).', '',
             '| Metric | Reference | Candidate | Change | Gate | Noise floor (ns) |',
             '| --- | ---: | ---: | ---: | --- | ---: |']
    for name, b, a, delta, gated, floor in rows:
        floor_text = f'{floor:.3f}' if floor is not None else '—'
        lines.append(f'| `{name}` | {b:.3f} | {a:.3f} | {delta * 100:+.1f}% | '
                     f'{"gated" if gated else "report"} | {floor_text} |')
    return '\n'.join(lines) + '\n'


def validate(data, policy):
    if data['config'] != policy['config']:
        raise ValueError('workload configuration differs from baseline')
    if data['rust_commit'] != policy['rust_commit']:
        raise ValueError('Rust source is not the immutable baseline commit')
    metrics = aggregate(data)
    if set(metrics) != set(policy['metrics']):
        raise ValueError('measured metric set differs from baseline manifest')
    return metrics


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('results', type=Path)
    p.add_argument('--baseline', type=Path, default=Path(__file__).with_name('perf_baseline.json'))
    p.add_argument('--reference', type=Path, help='same-runner measurements of baseline.reference_commit')
    p.add_argument('--hosted', action='store_true',
                   help='shared-runner policy: needs --reference; gates goodput and noise-qualified non-cross-thread micros')
    p.add_argument('--summary', type=Path, help='append a markdown report (e.g. $GITHUB_STEP_SUMMARY)')
    args = p.parse_args()
    if args.hosted and not args.reference:
        raise ValueError('--hosted compares against a same-runner --reference, never the absolute baseline')
    policy = json.loads(args.baseline.read_text())
    actual = json.loads(args.results.read_text())
    metrics = validate(actual, policy)
    if args.reference:
        reference = json.loads(args.reference.read_text())
        if reference['source_commit'] != policy['reference_commit']:
            raise ValueError('reference source is not the immutable baseline commit')
        if (reference['machine'] != actual['machine'] or reference['config'] != actual['config']
                or reference['build'] != actual['build']):
            raise ValueError('candidate/reference must share machine and workload')
        base = validate(reference, policy)
    else:
        if actual['machine'] != policy['machine']:
            raise ValueError('absolute baseline is for another machine; supply a same-runner --reference')
        base = policy['metrics']
    if args.hosted:
        failures, rows, warnings = hosted_compare(metrics, base, policy['tolerance'])
        for name, b, a, delta, gated, _floor in rows:
            if not gated:
                print(f'reported: {name}: {b:.3f} -> {a:.3f} ({delta * 100:+.1f}%)')
        for line in warnings:
            print(f'::warning title=FIPS loopback latency::{line} (report-only on hosted runners)')
        if args.summary:
            with args.summary.open('a') as out:
                out.write(summary_markdown(rows, policy['reference_commit'], policy['tolerance']))
        gated = sum(1 for row in rows if row[4])
    else:
        failures = compare(metrics, base, policy['tolerance'])
        gated = len(metrics)
    for line in failures:
        print(f'REGRESSION: {line}', file=sys.stderr)
    print(f'{len(metrics)} metrics, {gated} gated; medians of >=3 runs; {len(failures)} regression(s); '
          f'tolerance={policy["tolerance"]:.0%}')
    return bool(failures)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (KeyError, OSError, ValueError, TypeError) as error:
        print(f'INVALID RESULTS: {error}', file=sys.stderr)
        sys.exit(2)
