#!/usr/bin/env python3
"""Search algorithm tests; real daemon/driver smoke runs remain separate."""
import contextlib
import io
import subprocess
import sys
from unittest.mock import patch
from types import SimpleNamespace
import unittest
from perf_loopback import ControlError, Failure, counter_delta, metric_counters, run_driver, sustainable_search


class SearchTest(unittest.TestCase):
    def search(self, capacity, offered_factor=1, ceiling=3200, threshold=.01):
        args = SimpleNamespace(paced_max_mbps=ceiling, paced_resolution=.02, loss_threshold=threshold)
        def probe(rate):
            loss = 0 if rate <= capacity else .01
            return dict(offered_mbps=rate * offered_factor, throughput_mbps=rate * (1 - loss),
                        loss_fraction=loss)
        with contextlib.redirect_stderr(io.StringIO()):
            return sustainable_search(probe, args)

    def test_bracket_and_resolution(self):
        for capacity in (7, 100, 650):
            result = self.search(capacity)
            self.assertLessEqual(result['sustainable_mbps'], capacity)
            self.assertGreaterEqual(result['sustainable_mbps'], capacity / 1.02)
            self.assertTrue(any(not p['sustainable'] for p in result['probes']))

    def test_strict_loss_threshold(self):
        self.assertLessEqual(self.search(100)['sustainable_mbps'], 100)
        self.assertEqual(self.search(100, threshold=.011)['sustainable_mbps'], 3200)

    def test_unachieved_offer_is_not_sustainable(self):
        self.assertIsNone(self.search(10000, offered_factor=.9)['sustainable_mbps'])

    def test_no_passing_rate_and_ceiling(self):
        self.assertIsNone(self.search(0)['sustainable_mbps'])
        result = self.search(10000, ceiling=200)
        self.assertEqual(result['sustainable_mbps'], 200)
        self.assertTrue(result['search_ceiling_reached'])


class CpuWindowTest(unittest.TestCase):
    def test_markers_are_sampled_and_acknowledged_in_order(self):
        script = """
import sys
for marker in ('START', 'END'):
    print('FIPSD_PERF_TRAFFIC_' + marker, file=sys.stderr, flush=True)
    assert sys.stdin.readline() == '\\n'
print('{}')
"""
        with patch('perf_loopback.cpu_seconds', side_effect=[1, 2, 4, 6]) as cpu:
            stdout, begin, end = run_driver([sys.executable, '-c', script], [11, 22], 5)
        self.assertEqual(stdout.strip(), '{}')
        self.assertEqual(begin, [1, 2])
        self.assertEqual(end, [4, 6])
        self.assertEqual([call.args[0] for call in cpu.call_args_list], [11, 22, 11, 22])

    def test_missing_or_out_of_order_markers_fail(self):
        for script in ("print('{}')", "import sys; print('FIPSD_PERF_TRAFFIC_END', file=sys.stderr)"):
            with self.subTest(script=script), self.assertRaises(Failure):
                run_driver([sys.executable, '-c', script], [], 5)

    def test_cpu_sampling_failure_and_child_timeout_are_propagated(self):
        script = "import sys; print('FIPSD_PERF_TRAFFIC_START', file=sys.stderr, flush=True); sys.stdin.read()"
        with patch('perf_loopback.cpu_seconds', side_effect=OSError('CPU unavailable')):
            with self.assertRaisesRegex(OSError, 'CPU unavailable'):
                run_driver([sys.executable, '-c', script], [11], 5)
        with self.assertRaises(subprocess.TimeoutExpired):
            run_driver([sys.executable, '-c', 'import sys; sys.stdin.read()'], [], .1)


class NativeCountersTest(unittest.TestCase):
    def test_flattened_native_deltas(self):
        node = SimpleNamespace(query=lambda command: {'native': {'drop_flow_queue_full': 7, 'flows_opened': 1},
                                                      'forwarding': {'sent': 3}, 'enabled': True})
        before = metric_counters(node)
        self.assertEqual(before, {'native.drop_flow_queue_full': 7, 'native.flows_opened': 1, 'forwarding.sent': 3})
        after = dict(before, **{'native.drop_flow_queue_full': 9, 'native.received_datagrams': 4, 'forwarding.sent': 8})
        self.assertEqual(counter_delta(before, after),
                         {'native.drop_flow_queue_full': 2, 'native.received_datagrams': 4})

    def test_unanswered_metrics_give_none(self):
        def refuse(command):
            raise ControlError('unknown command')
        self.assertIsNone(metric_counters(SimpleNamespace(query=refuse)))
        self.assertIsNone(counter_delta(None, {'native.flows_opened': 1}))


if __name__ == '__main__':
    unittest.main()
