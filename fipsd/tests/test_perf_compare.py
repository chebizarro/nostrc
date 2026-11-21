#!/usr/bin/env python3
"""No performance thresholds depend on tests faking a fast daemon."""
import copy
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from perf_compare import aggregate, compare, hosted_compare, hosted_gated, validate, report_only


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

    def test_new_diagnostics_are_not_gate_metrics(self):
        data = self.data()
        original = aggregate(data)
        paced = copy.deepcopy(data['loopback'])
        paced['paced'] = {'loss_threshold': .01}
        for run in paced['runs']:
            for row in run:
                row.update(sustainable_mbps=400, sustainable_loss=0,
                           loaded_one_way={'p50_us': 10, 'p99_us': 100})
                for cpu in row['cpu']:
                    cpu['us_per_delivered_datagram'] = cpu['traffic_s'] * 1e6 / row['received']
        data['paced_loopback'] = paced
        self.assertEqual(aggregate(data), original)
        report = report_only(data)
        self.assertEqual(report['paced/c-c/sustainable_mbps'], 400)
        self.assertEqual(report['paced/c-c/loss_fraction'], 0)
        self.assertEqual(report['paced/c-c/loaded_one_way/p99_us'], 100)
        self.assertEqual(report['loopback/c-c/cpu0_us_per_delivered_datagram'], 10)
        for run in paced['runs']:
            run[0]['sustainable_mbps'] = None
        self.assertIsNone(report_only(data)['paced/c-c/sustainable_mbps'])
        self.assertEqual(compare(aggregate(data), original), [])

    def test_window_cpu_and_fixed_rate_remain_report_only(self):
        data = self.data()
        original = aggregate(data)
        for run in data['loopback']['runs']:
            for row in run:
                for cpu in row['cpu']:
                    cpu.update(traffic_cpu_s=.004, us_per_delivered_datagram=4,
                               us_per_delivered_datagram_run=10)
        paced = copy.deepcopy(data['loopback'])
        paced['paced'] = {'paced_rate': 100}
        for run in paced['runs']:
            for row in run:
                row.update(paced_rate_mbps=100, offered_mbps=100, loss_fraction=.2)
        data['paced_loopback'] = paced
        report = report_only(data)
        self.assertEqual(report['loopback/c-c/cpu0_us_per_delivered_datagram'], 4)
        self.assertEqual(report['loopback/c-c/cpu0_us_per_delivered_datagram_run'], 10)
        self.assertEqual(report['paced/c-c/fixed_rate_mbps'], 100)
        self.assertEqual(report['paced/c-c/loss_fraction'], .2)
        self.assertEqual(report['paced/c-c/cpu0_us_per_delivered_datagram'], 4)
        self.assertEqual(aggregate(data), original)


class HostedPolicyTest(unittest.TestCase):
    """Shared runners gate goodput and noise-qualified, non-cross-thread micros."""
    def setUp(self):
        policy = json.loads(Path(__file__).with_name('perf_baseline.json').read_text())
        self.reference = copy.deepcopy(policy['metrics'])
        for name, metric in self.reference.items():
            if name.startswith('micro/'):
                metric['spread_ns'] = 0

    def scaled(self, predicate, factor):
        actual = copy.deepcopy(self.reference)
        for name, metric in actual.items():
            if predicate(name): metric['value'] *= factor
        return actual

    def test_gates_goodput_and_non_cross_thread_micro_medians(self):
        gated = sorted(n for n in self.reference if hosted_gated(n))
        self.assertEqual(len(gated), 39 - 6 + 3)
        self.assertFalse(any('_cross_thread' in n for n in gated))
        self.assertTrue(all(n.startswith('micro/') or n.endswith('/throughput_mbps') for n in gated))
        self.assertFalse(any(hosted_gated(n) for n in self.reference if '_us' in n or '/cpu' in n))

    def test_latency_and_cpu_regressions_are_reported_not_failed(self):
        actual = self.scaled(lambda n: n.endswith('_us') or '/cpu' in n, 1.4)
        failures, rows, warnings = hosted_compare(actual, self.reference)
        self.assertEqual(failures, [])
        self.assertEqual(len(rows), 66)
        self.assertEqual(warnings, [])  # +40% stays below the 50% p50 warning

    def test_eleven_percent_goodput_or_micro_regression_fails(self):
        for predicate, factor in ((lambda n: n.endswith('/throughput_mbps'), .89),
                                  (lambda n: n.startswith('micro/'), 1.11)):
            failures, _, _ = hosted_compare(self.scaled(predicate, factor), self.reference)
            self.assertTrue(failures)

    def with_goodput(self, base, **values):
        out = copy.deepcopy(base)
        for pairing, value in values.items():
            out[f'loopback/{pairing.replace("_", "-")}/throughput_mbps']['value'] = value
        return out

    def test_c_rust_bounded_by_rust_receiver_warns_not_fails(self):
        # Hosted run 44700102649: the faster C sender drove the pinned Rust
        # receiver into the same overload collapse Rust<->Rust shows.
        base = self.with_goodput(self.reference, c_c=434, c_rust=421, rust_rust=55)
        actual = self.with_goodput(self.reference, c_c=924, c_rust=58, rust_rust=56)
        failures, _, warnings = hosted_compare(actual, base)
        self.assertEqual(failures, [])
        self.assertEqual(len(warnings), 1)
        self.assertIn('bounded by the Rust receiver', warnings[0])

    def test_c_rust_below_rust_control_still_fails(self):
        base = self.with_goodput(self.reference, c_c=434, c_rust=421, rust_rust=55)
        actual = self.with_goodput(self.reference, c_c=924, c_rust=49, rust_rust=56)
        failures, _, _ = hosted_compare(actual, base)
        self.assertEqual(len(failures), 1)
        self.assertIn('loopback/c-rust/throughput_mbps', failures[0])

    def test_rust_control_does_not_excuse_c_c(self):
        base = self.with_goodput(self.reference, c_c=434, c_rust=421, rust_rust=55)
        actual = self.with_goodput(self.reference, c_c=380, c_rust=421, rust_rust=500)
        failures, _, _ = hosted_compare(actual, base)
        self.assertEqual(len(failures), 1)
        self.assertIn('loopback/c-c/throughput_mbps', failures[0])

    def test_p50_beyond_fifty_percent_warns(self):
        actual = self.scaled(lambda n: n.endswith('/p50_us'), 1.6)
        failures, _, warnings = hosted_compare(actual, self.reference)
        self.assertEqual(failures, [])
        self.assertEqual(len(warnings), 9)  # 3 pairings x one_way/reverse_one_way/rtt

    def sampled_metrics(self, name, reference_values, candidate_values):
        reference = ComparisonTest.data(ComparisonTest())
        candidate = copy.deepcopy(reference)
        for data, values in ((reference, reference_values), (candidate, candidate_values)):
            for run, value in zip(data['micro_runs'], values):
                run[name]['median_ns'] = run[name]['p95_ns'] = value
        return aggregate(candidate), aggregate(reference)

    def test_historical_hosted_false_positives(self):
        # Raw medians from the three fips-bench-results artifacts, not rounded log rows.
        cases = [
            ('44082939871', 'transport_addr_clone/shared/6',
             [7.939, 8.958, 7.938], [7.939, 8.958, 8.958]),
            ('44301687127', 'transport_addr_clone/shared/6',
             [9.697, 9.722, 9.120], [10.939, 10.938, 10.936]),
            ('44301687127', 'transport_addr_clone/shared/18',
             [8.863, 10.295, 8.857], [10.939, 10.933, 10.940]),
            ('44301687127', 'transport_addr_clone/shared/58',
             [9.704, 11.681, 8.862], [10.938, 9.994, 10.940]),
            ('44301687127', 'transport_addr_clone/former_vec/18',
             [12.653, 13.572, 14.436], [15.040, 14.429, 15.057]),
            ('44301687127', 'transport_addr_clone/former_vec/58',
             [12.899, 13.449, 12.833], [15.651, 13.061, 14.462]),
            ('44301687127', 'transport_addr_socket_and_clone/shared/scoped_ipv6',
             [74.351, 72.444, 87.196], [82.398, 72.576, 84.556]),
            ('44410974924', 'transport_addr_clone/shared/6',
             [7.940, 7.941, 8.959], [7.940, 8.959, 9.195]),
            ('44410974924', 'transport_addr_cross_thread/former_vec/6',
             [345.635, 372.870, 335.872], [401.160, 408.535, 356.302]),
            ('44410974924', 'transport_addr_cross_thread/former_vec/18',
             [343.923, 380.087, 342.034], [407.777, 416.237, 352.566]),
            ('44410974924', 'transport_addr_cross_thread/former_vec/58',
             [375.533, 417.744, 382.783], [437.104, 441.858, 398.370]),
        ]
        for run, name, reference, candidate in cases:
            with self.subTest(run=run, name=name):
                actual, base = self.sampled_metrics(name, reference, candidate)
                self.assertTrue(compare(actual, base))  # Original local 10% gate still fails.
                self.assertEqual(hosted_compare(actual, base)[0], [])

    def test_spread_uses_run_medians_not_p95_or_pooled_builds(self):
        actual, base = self.sampled_metrics('transport_addr_clone/shared/6',
                                           [200, 201, 202], [280, 281, 282])
        name = 'micro/transport_addr_clone/shared/6/median_ns'
        self.assertEqual(actual[name]['spread_ns'], 2)
        self.assertEqual(base[name]['spread_ns'], 2)
        self.assertEqual(len(hosted_compare(actual, base)[0]), 1)

    def test_real_goodput_and_micro_regressions_still_fail(self):
        actual = self.scaled(lambda n: n.endswith('/throughput_mbps'), .85)
        self.assertEqual(len(hosted_compare(actual, self.reference)[0]), 3)
        actual, base = self.sampled_metrics('transport_addr_clone/shared/6',
                                           [199, 200, 201], [279, 280, 281])
        self.assertEqual(len(hosted_compare(actual, base)[0]), 1)

    def test_absolute_and_spread_floor_edges_are_strict(self):
        name = 'micro/transport_addr_clone/shared/6/median_ns'
        for b, spread, boundary in ((8, 0, 10), (100, 20, 140)):
            for noisy_side in ('candidate', 'reference'):
                for a, fails in ((boundary, False), (math.nextafter(boundary, math.inf), True)):
                    with self.subTest(base=b, spread=spread, side=noisy_side, actual=a):
                        base = {name: {'value': b, 'direction': 'lower', 'spread_ns': 0}}
                        actual = {name: {'value': a, 'direction': 'lower', 'spread_ns': 0}}
                        (actual if noisy_side == 'candidate' else base)[name]['spread_ns'] = spread
                        failures, rows, _ = hosted_compare(actual, base)
                        self.assertEqual(bool(failures), fails)
                        self.assertEqual(rows[0][5], boundary - b)

    def test_relative_gate_remains_required_and_goodput_boundary_unchanged(self):
        name = 'micro/transport_addr_clone/shared/6/median_ns'
        base = {name: {'value': 200, 'direction': 'lower', 'spread_ns': 0}}
        actual = copy.deepcopy(base)
        actual[name]['value'] = 220
        self.assertEqual(hosted_compare(actual, base)[0], [])  # >2 ns, exactly 10%.
        actual[name]['value'] = 221
        self.assertTrue(hosted_compare(actual, base)[0])
        self.assertEqual(hosted_compare(self.scaled(lambda n: n.endswith('/throughput_mbps'), .90),
                                        self.reference)[0], [])
        self.assertTrue(hosted_compare(self.scaled(lambda n: n.endswith('/throughput_mbps'), .899),
                                       self.reference)[0])

    def test_cross_thread_is_report_only_even_for_large_shifts(self):
        actual = self.scaled(lambda n: '_cross_thread' in n, 2)
        failures, rows, _ = hosted_compare(actual, self.reference)
        self.assertEqual(failures, [])
        self.assertEqual(sum(not row[4] for row in rows if '_cross_thread' in row[0]), 6)
        self.assertEqual(len(compare(actual, self.reference)), 6)

    def test_missing_or_invalid_spread_fails_closed(self):
        name = 'micro/transport_addr_clone/shared/6/median_ns'
        for side in ('candidate', 'reference'):
            for value in (None, True, -1, float('nan'), float('inf')):
                with self.subTest(side=side, value=value), self.assertRaises((KeyError, ValueError)):
                    actual, base = copy.deepcopy(self.reference), copy.deepcopy(self.reference)
                    metric = (actual if side == 'candidate' else base)[name]
                    if value is None:
                        del metric['spread_ns']
                    else:
                        metric['spread_ns'] = value
                    hosted_compare(actual, base)

    def test_hosted_cli_uses_spreads_and_summary_without_new_flags(self):
        policy_path = Path(__file__).with_name('perf_baseline.json')
        policy = json.loads(policy_path.read_text())
        reference = ComparisonTest.data(ComparisonTest())
        reference.update(source_commit=policy['reference_commit'], machine={'test': True}, build={'test': True})
        candidate = copy.deepcopy(reference)
        candidate['source_commit'] = 'candidate'
        name = 'transport_addr_socket_and_clone/shared/scoped_ipv6'
        for data, values in ((reference, [74.351, 72.444, 87.196]),
                             (candidate, [82.398, 72.576, 84.556])):
            for run, value in zip(data['micro_runs'], values):
                run[name]['median_ns'] = run[name]['p95_ns'] = value
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[2]) as tmp:
            tmp = Path(tmp)
            for filename, data in (('candidate.json', candidate), ('reference.json', reference)):
                (tmp / filename).write_text(json.dumps(data))
            command = [sys.executable, str(Path(__file__).with_name('perf_compare.py')),
                       str(tmp / 'candidate.json'), '--baseline', str(policy_path),
                       '--reference', str(tmp / 'reference.json'), '--summary', str(tmp / 'summary.md')]
            hosted = subprocess.run(command + ['--hosted'], capture_output=True, text=True)
            local = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(hosted.returncode, 0, hosted.stderr)
            self.assertEqual(local.returncode, 1, local.stderr)
            summary = (tmp / 'summary.md').read_text()
            self.assertIn('29.504', summary)
            self.assertIn('cross-thread micros', summary)
            self.assertIn('36 gated', hosted.stdout)

    def test_hosted_requires_a_same_runner_reference(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[2]) as tmp:
            results = Path(tmp) / 'candidate.json'
            results.write_text(json.dumps(ComparisonTest.data(ComparisonTest())))
            run = subprocess.run([sys.executable, str(Path(__file__).with_name('perf_compare.py')), str(results), '--hosted'],
                                 capture_output=True, text=True)
        self.assertEqual(run.returncode, 2)
        self.assertIn('same-runner --reference', run.stderr)


class PinBaselineCheckTest(unittest.TestCase):
    root = Path(__file__).resolve().parents[2]

    def run_check(self, root):
        return subprocess.run(['bash', str(root / 'scripts/fips-upstream-bump.sh'),
                               '--check-perf-baseline'], capture_output=True, text=True)

    def test_checked_in_baseline_matches_accepted_pin(self):
        run = self.run_check(self.root)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn('immutable C reference', run.stdout)

    def test_preflight_fails_before_rehearsal_on_stale_rust_pin(self):
        self.check_fixture('stale_rust', 'Do not relabel old timings')

    def test_preflight_rejects_invalid_c_reference(self):
        self.check_fixture('invalid_reference', 'invalid C reference commit')

    def test_preflight_is_read_only(self):
        self.check_fixture('valid', None)

    def check_fixture(self, case, error):
        with tempfile.TemporaryDirectory(dir=self.root) as tmp:
            root = Path(tmp)
            for rel in ('scripts/fips-upstream-bump.sh', 'tools/fips-vectors/Cargo.toml',
                        'tools/fips-vectors/rust-toolchain.toml', 'libfips-core/UPSTREAM.md',
                        'fipsd/tests/perf_baseline.json'):
                p = root / rel
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_bytes((self.root / rel).read_bytes())
            path = root / 'fipsd/tests/perf_baseline.json'
            baseline = json.loads(path.read_text())
            if case == 'stale_rust':
                baseline['rust_commit'] = '0' * 40
            elif case == 'invalid_reference':
                baseline['reference_commit'] = 'moving-branch'
            path.write_text(json.dumps(baseline))
            before = {str(p.relative_to(root)): p.read_bytes() for p in root.rglob('*') if p.is_file()}
            run = self.run_check(root)
            if error:
                self.assertEqual(run.returncode, 2, run.stderr)
                self.assertIn(error, run.stderr)
            else:
                self.assertEqual(run.returncode, 0, run.stderr)
            after = {str(p.relative_to(root)): p.read_bytes() for p in root.rglob('*') if p.is_file()}
            self.assertEqual(after, before)


if __name__ == '__main__':
    unittest.main()
