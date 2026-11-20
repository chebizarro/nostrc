#!/usr/bin/env python3
"""Search algorithm tests; real daemon/driver smoke runs remain separate."""
import contextlib
import io
from types import SimpleNamespace
import unittest
from perf_loopback import sustainable_search


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


if __name__ == '__main__':
    unittest.main()
