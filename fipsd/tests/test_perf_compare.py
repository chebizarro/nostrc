#!/usr/bin/env python3
"""No performance thresholds depend on tests faking a fast daemon."""
import copy
import json
from pathlib import Path
import unittest
from perf_compare import aggregate, compare, validate


class ComparisonTest(unittest.TestCase):
    def setUp(self):
        self.base = {'latency': {'value': 100, 'direction': 'lower'},
                     'throughput': {'value': 100, 'direction': 'higher'}}

    def test_exact_boundary_passes(self):
        candidate = copy.deepcopy(self.base)
        candidate['latency']['value'] = 110
        candidate['throughput']['value'] = 90
        self.assertEqual(compare(candidate, self.base), [])

    def test_eleven_percent_regression_fails_both_directions(self):
        candidate = copy.deepcopy(self.base)
        candidate['latency']['value'] = 111
        candidate['throughput']['value'] = 89
        self.assertEqual(len(compare(candidate, self.base)), 2)

    def test_missing_or_extra_metrics_fail(self):
        with self.assertRaises(ValueError):
            compare({}, self.base)
        with self.assertRaises(ValueError):
            compare(self.base, {})

    def test_non_finite_zero_negative_and_boolean_fail(self):
        for value in (float('nan'), float('inf'), 0, -1, True):
            with self.subTest(value=value), self.assertRaises(ValueError):
                candidate = copy.deepcopy(self.base)
                candidate['latency']['value'] = value
                compare(candidate, self.base)

    def test_cannot_widen_tolerance(self):
        with self.assertRaises(ValueError):
            compare(self.base, self.base, .11)

    def data(self):
        policy = json.loads(Path(__file__).with_name('perf_baseline.json').read_text())
        micro = {key[len('micro/'):-len('/median_ns')]: {'median_ns': 100, 'p95_ns': 110, 'ops_per_s': 1e7}
                 for key in policy['metrics'] if key.startswith('micro/')}
        rows = [{'pairing': pair, 'payload_bytes': 1200, 'window': 0, 'loss': 1,
                 'sent': 1001, 'received': 1000, 'elapsed_s': 5, 'throughput_mbps': 1.92,
                 'latency_samples': 1000, 'one_way': {'p50_us': 1, 'p99_us': 2},
                 'reverse_one_way': {'p50_us': 1, 'p99_us': 2}, 'rtt': {'p50_us': 2, 'p99_us': 4},
                 'cpu': [{'traffic_s': .01}, {'traffic_s': .02}]} for pair in ('c-c', 'rust-rust', 'c-rust')]
        return {'schema': 1, 'config': copy.deepcopy(policy['config']), 'rust_commit': policy['rust_commit'],
                'micro_runs': [copy.deepcopy(micro) for _ in range(3)],
                'loopback': {'seconds': 5, 'runs': [copy.deepcopy(rows) for _ in range(3)]}}

    def test_median_not_best_of_three(self):
        data = self.data()
        key = next(iter(data['micro_runs'][0]))
        for run, value in zip(data['micro_runs'], (100, 200, 10000)):
            run[key]['median_ns'] = run[key]['p95_ns'] = value
        self.assertEqual(aggregate(data)[f'micro/{key}/median_ns']['value'], 200)

    def test_missing_run_pair_or_bench_fails(self):
        for kind in ('run', 'pair', 'bench'):
            data = self.data()
            if kind == 'run': data['micro_runs'].pop()
            if kind == 'pair': data['loopback']['runs'][0].pop()
            if kind == 'bench': data['micro_runs'][0].pop(next(iter(data['micro_runs'][0])))
            with self.subTest(kind=kind), self.assertRaises(ValueError):
                aggregate(data)

    def test_declared_run_count_must_match_both_collectors(self):
        for kind in ('micro', 'loopback', 'declared'):
            data = self.data()
            if kind == 'micro': data['micro_runs'].append(copy.deepcopy(data['micro_runs'][0]))
            if kind == 'loopback': data['loopback']['runs'].append(copy.deepcopy(data['loopback']['runs'][0]))
            if kind == 'declared': data['config']['runs'] = 4
            with self.subTest(kind=kind), self.assertRaises(ValueError):
                aggregate(data)

    def test_actual_duration_and_samples_must_match_declared(self):
        for kind in ('row_duration', 'loopback_duration', 'samples'):
            data = self.data()
            row = data['loopback']['runs'][0][0]
            if kind == 'row_duration':
                row['elapsed_s'] = 1
                row['throughput_mbps'] = 9.6  # Internally consistent but wrong workload.
            if kind == 'loopback_duration': data['loopback']['seconds'] = 1
            if kind == 'samples': row['latency_samples'] = 100
            with self.subTest(kind=kind), self.assertRaises(ValueError):
                aggregate(data)

    def test_rust_pin_missing_or_mismatched_fails(self):
        policy = json.loads(Path(__file__).with_name('perf_baseline.json').read_text())
        self.assertEqual(len(validate(self.data(), policy)), 66)
        for revision in (None, '0' * 40):
            data = self.data()
            if revision is None: del data['rust_commit']
            else: data['rust_commit'] = revision
            with self.subTest(revision=revision), self.assertRaises((KeyError, ValueError)):
                validate(data, policy)

    def test_declared_workload_must_match_policy(self):
        policy = json.loads(Path(__file__).with_name('perf_baseline.json').read_text())
        data = self.data()
        data['config']['iterations'] += 1
        with self.assertRaises(ValueError):
            validate(data, policy)

    def test_inconsistent_delivery_fails(self):
        data = self.data()
        data['loopback']['runs'][0][0]['received'] = 1002
        with self.assertRaises(ValueError):
            aggregate(data)


if __name__ == '__main__':
    unittest.main()
