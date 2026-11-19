#!/usr/bin/env python3
"""Regression tests for UDP diagnostic lifetime and bounded retry policy."""
from copy import deepcopy
from types import SimpleNamespace
import json
import unittest
from unittest.mock import MagicMock, patch

from udp_interop import Failure, MultiScenario, NotYet, Scenario


def report(reason=None, probe_id=1):
    return {
        'probe_id': probe_id,
        'overall': 'partial' if reason else 'ok',
        'bloom': {'verdict': 'skipped'},
        'discovery': {'verdict': 'skipped'},
        'path': {'verdict': 'ok'},
        'session': {'verdict': 'skipped', 'established': True, 'preexisting': True},
        'rtt': {'verdict': 'failed' if reason else 'ok', 'reason': reason,
                'reports_seen': 1, 'samples': 0 if reason else 1,
                'zero_samples': int(reason == 'sub_millisecond'), 'arith_failures': 0},
        'cleanup': {'session_created_and_torn_down': False},
    }


class ProbeNode:
    def __init__(self, reports):
        self.name, self.npub = 'source', 'source-npub'
        self.reports = deepcopy(reports)
        self.starts = 0
        self.polls = []
        self.running = set()

    def query(self, command, params):
        if command == 'probe_start':
            self.starts += 1
            return {'probe_id': self.starts}
        if command != 'probe_poll':
            raise AssertionError(command)
        probe_id = params['probe_id']
        self.polls.append(probe_id)
        if probe_id not in self.running:
            self.running.add(probe_id)
            return {'state': 'running', 'report': None}
        return {'state': 'done', 'report': self.reports[probe_id - 1]}


class ProbeRetryTest(unittest.TestCase):
    def scenario(self, reports):
        # Exercise the production collectors and validators; only daemon IO
        # and the wait clock are replaced, so the test never sleeps.
        scenario = MultiScenario.__new__(MultiScenario)
        source = ProbeNode(reports)
        target = SimpleNamespace(name='target', npub='target-npub')
        scenario.args = SimpleNamespace(probe_timeout=60)
        scenario.lines = []
        scenario.note = scenario.lines.append
        scenario.deadlines = []

        def wait(what, deadline, predicate):
            scenario.deadlines.append(deadline)
            for _ in range(2):
                try:
                    return predicate()
                except NotYet:
                    pass
            self.fail('probe did not complete')

        scenario.wait = wait
        scenario.other = {source.name: target}
        scenario.sessions = lambda node: [{'mmp': {'srtt_ms': 1}}]
        return scenario, source, target

    def test_every_bidirectional_pair_is_adopted_before_either_probe(self):
        for names in (('c0', 'c2'), ('rust1', 'rust3'), ('c0', 'rust1'), ('rust1', 'c0')):
            with self.subTest(names=names):
                scenario = MultiScenario.__new__(MultiScenario)
                calls = []
                source = SimpleNamespace(name=names[0], npub='source')
                target = SimpleNamespace(name=names[1], npub='target')
                def start(command, params):
                    self.assertTrue(calls and calls[0][0] == 'adopt')
                    calls.append((command, params))
                    return {'probe_id': 1}
                source.query = target.query = start
                scenario.datagram_pair = lambda a, b: calls.append(('adopt', a.name, b.name))
                def checked(a, b, started):
                    calls.append(('check', a.name, b.name))
                    return report()
                scenario.probe_pair = checked
                scenario.probe_bidirectional(source, target)
                self.assertEqual([call[0] for call in calls],
                                 ['adopt', 'probe_start', 'probe_start', 'check', 'check'])
                self.assertEqual(calls[0][1:],
                                 names[::-1] if names[0].startswith('c') and names[1].startswith('rust') else names)

    def test_native_data_adopts_both_live_probes_before_completion(self):
        scenario = MultiScenario.__new__(MultiScenario)
        scenario.args = SimpleNamespace(session_timeout=30, remote_port=4242)
        scenario.api_paths = {'c0': 'source-api'}
        scenario.reply_payload = b'reply'
        source = SimpleNamespace(name='c0', npub='source-key', node_addr='source-address')
        target = SimpleNamespace(name='c2', npub='target-key', node_addr='target-address')
        calls, data = [], {'received_datagrams': 0, 'received_bytes': 0}
        def query(node, other, command, params=None):
            calls.append((node.name, command))
            if command == 'probe_start':
                return {'probe_id': node.name}
            if command == 'probe_poll':
                return {'state': 'running', 'report': {'session': {'verdict': 'ok'}}}
            if command == 'show_sessions':
                return {'sessions': [{'remote_addr': other.node_addr, 'state': 'established', 'identity': 'proven'}]}
            if command == 'show_metrics':
                return {'native': dict(data)}
            raise AssertionError(command)
        source.query = lambda command, params=None: query(source, target, command, params)
        target.query = lambda command, params=None: query(target, source, command, params)
        scenario.wait = lambda what, deadline, predicate: predicate()
        def checked(a, b, started):
            self.assertTrue(data['received_datagrams'])
            self.assertIn(('reply', 'received'), calls)
            self.assertEqual(started['probe_id'], a.name)
            calls.append((a.name, 'completed'))
            return report()
        scenario.probe_pair = checked
        class Flow:
            def __init__(inner, *args):
                pass
            def send(inner, payload):
                self.assertIn(('c2', 'probe_start'), calls)
                data.update(received_datagrams=1, received_bytes=len(payload))
            def recv(inner):
                calls.append(('reply', 'received'))
                return scenario.reply_payload
            def close(inner):
                calls.append(('flow', 'closed'))
        with patch('udp_interop.NativeFlow', Flow):
            scenario.datagram_pair(source, target)
        self.assertEqual(calls[-2:], [('c0', 'completed'), ('c2', 'completed')])

    def test_receiver_release_is_observed_before_daemon_shutdown(self):
        scenario = MultiScenario.__new__(MultiScenario)
        scenario.args = SimpleNamespace(shutdown_timeout=20)
        calls = []
        def stop(timeout):
            calls.append('client stopped')
            return 0
        receiver = SimpleNamespace(stop=stop, forced=False, sanitizer_report=lambda: False)
        scenario.receivers = {'receiver': receiver}
        def query(command):
            self.assertEqual(command, 'show_native_flows')
            calls.append('native snapshot')
            return {'flows': [], 'listeners': [1] if calls.count('native snapshot') == 1 else []}
        scenario.nodes = {'node': SimpleNamespace(query=query)}
        def wait(what, deadline, predicate):
            for _ in range(2):
                try:
                    return predicate()
                except NotYet:
                    pass
            self.fail('native receiver did not release its port')
        scenario.wait = wait
        def stop_nodes():
            calls.append('daemon stopped')
            return []
        with patch('udp_interop.Scenario.shutdown', side_effect=stop_nodes):
            self.assertEqual(scenario.shutdown(), [])
        self.assertEqual(calls, ['client stopped', 'native snapshot', 'native snapshot', 'daemon stopped'])

    def test_success_does_not_retry(self):
        scenario, source, target = self.scenario([report()])
        self.assertEqual(scenario.probe_pair(source, target)['overall'], 'ok')
        self.assertEqual(source.starts, 1)

    def test_no_echo_then_sample_retries_new_job_and_records_both(self):
        first, second = report('no_echo'), report(probe_id=2)
        scenario, source, target = self.scenario([first, second])
        started = source.query('probe_start', {'npub': target.npub})
        self.assertEqual(scenario.probe_pair(source, target, started), second)
        self.assertEqual(source.starts, 2)
        self.assertEqual(source.polls, [1, 1, 2, 2])
        self.assertEqual(scenario.deadlines[0], scenario.deadlines[1])
        for attempt, expected in enumerate((first, second), 1):
            self.assertTrue(any(f'attempt {attempt} report: {json.dumps(expected)}' in line
                                for line in scenario.lines))

    def test_two_no_echo_reports_fail_without_third_attempt(self):
        reports = [report('no_echo'), report('no_echo', probe_id=2)]
        scenario, source, target = self.scenario(reports)
        with self.assertRaisesRegex(Failure, 'two consecutive no_echo') as raised:
            scenario.probe_pair(source, target)
        self.assertIn(json.dumps(reports), str(raised.exception))
        self.assertEqual(source.starts, 2)

    def test_other_failures_are_not_retried(self):
        for reason in ('no_report', 'bad_timestamp_echo'):
            with self.subTest(reason=reason):
                scenario, source, target = self.scenario([report(reason)])
                with self.assertRaises(Failure):
                    scenario.probe_pair(source, target)
                self.assertEqual(source.starts, 1)

    def test_no_echo_cannot_hide_another_failed_stage(self):
        for stage in ('bloom', 'discovery', 'path', 'session'):
            with self.subTest(stage=stage):
                first = report('no_echo')
                first[stage]['verdict'] = 'failed'
                scenario, source, target = self.scenario([first])
                self.assertEqual(scenario.probe_reports(source, target), [first])
                self.assertEqual(source.starts, 1)

    def test_retry_failure_is_not_accepted(self):
        for second in (report('no_report', 2), report('bad_timestamp_echo', 2)):
            with self.subTest(second=second):
                scenario, source, target = self.scenario([report('no_echo'), second])
                with self.assertRaises(Failure):
                    scenario.probe_pair(source, target)
                self.assertEqual(source.starts, 2)

    def test_sub_millisecond_multinode_policy_is_unchanged(self):
        scenario, source, target = self.scenario([report('sub_millisecond')])
        self.assertEqual(scenario.probe_pair(source, target)['overall'], 'partial')
        self.assertEqual(source.starts, 1)

    def test_two_node_probe_retries_without_relaxing_sample_requirement(self):
        scenario, source, target = self.scenario([report('no_echo'), report(probe_id=2)])
        self.assertEqual(Scenario.probe(scenario, source, True)['probe_id'], 2)
        scenario, source, target = self.scenario([report('no_echo'), report('sub_millisecond', 2)])
        with self.assertRaises(Failure):
            Scenario.probe(scenario, source, True)

    def test_retry_does_not_replace_original_session_ownership_evidence(self):
        first = report('no_echo')
        first['session'].update(verdict='ok', preexisting=False)
        scenario, source, target = self.scenario([first, report(probe_id=2)])
        self.assertEqual(Scenario.probe(scenario, source, False)['probe_id'], 2)
        first['cleanup']['session_created_and_torn_down'] = True
        scenario, source, target = self.scenario([first, report(probe_id=2)])
        with self.assertRaisesRegex(Failure, 'tore the shared session down'):
            Scenario.probe(scenario, source, False)



class ControlSnapshotTest(unittest.TestCase):
    def test_control_io_retries_share_the_absolute_wait_deadline(self):
        for error in (TimeoutError, ConnectionRefusedError, ConnectionResetError, FileNotFoundError):
            with self.subTest(error=error):
                scenario = Scenario.__new__(Scenario)
                clock = [0.0]
                cond = MagicMock()
                cond.wait.side_effect = lambda duration: clock.__setitem__(0, clock[0] + duration)
                node = SimpleNamespace(daemon=SimpleNamespace(cond=cond), deadline=None)
                scenario.nodes = {'c': node}
                scenario.check_alive = lambda: None
                scenario.note = lambda line: None
                predicate = MagicMock(side_effect=[error('transient control IO'), 'ready'])
                with patch('udp_interop.time.monotonic', side_effect=lambda: clock[0]):
                    self.assertEqual(scenario.wait('ready', 1, predicate), 'ready')
                    self.assertEqual(predicate.call_count, 2)
                    self.assertIsNone(node.deadline)
                    predicate.side_effect = error('still unavailable')
                    with self.assertRaisesRegex(Failure, error.__name__):
                        scenario.wait('ready', 1, predicate)
                self.assertIsNone(node.deadline)

    def test_tree_waits_for_peer_rows_before_indexing(self):
        scenario = Scenario.__new__(Scenario)
        scenario.args = SimpleNamespace(link_timeout=30)
        scenario.note = lambda line: None
        attempts = {}
        def node(name, is_root):
            tree = dict(root='root', is_root=is_root, depth=0 if is_root else 1,
                        peer_tree_count=1, parent=None if is_root else 'root',
                        my_node_addr='root' if is_root else 'leaf', my_coords=[])
            def query(command):
                if command == 'show_tree':
                    return tree
                if command == 'show_bloom':
                    return {'peer_filters': [{'has_filter': True}]}
                self.assertEqual(command, 'show_peers')
                attempts[name] = attempts.get(name, 0) + 1
                return {'peers': [] if attempts[name] == 1 else
                        [{'is_child': is_root, 'is_parent': not is_root}]}
            return SimpleNamespace(name=name, query=query)
        scenario.nodes = {'c': node('c', True), 'rust': node('rust', False)}
        def wait(what, deadline, predicate):
            for _ in range(3):
                try:
                    return predicate()
                except NotYet:
                    pass
            self.fail('convergence did not consume the next peer snapshot')
        scenario.wait = wait
        scenario.tree_and_filters()
        self.assertEqual(attempts, {'c': 2, 'rust': 2})

    def test_rekey_diagnostic_validates_all_session_rows(self):
        scenario = Scenario.__new__(Scenario)
        scenario.args = SimpleNamespace(rekey_timeout=60)
        scenario.session_up_at = 0
        scenario.initial_session_k = {'c': False, 'rust': False}
        scenario.nodes = {name: SimpleNamespace(name=name) for name in ('c', 'rust')}
        scenario.sessions = lambda node: ([{'state': 'established', 'current_k_bit': False}]
                                          if node.name == 'c' else [])
        scenario.wait = lambda what, deadline, predicate: predicate()
        with self.assertRaisesRegex(Failure, 'rust holds 0 sessions'):
            scenario.rekey()

    def test_probe_uses_the_validated_session_snapshot(self):
        scenario, source, target = ProbeRetryTest().scenario([report()])
        queried = []
        def sessions(node):
            queried.append(node.name)
            if queried.count(node.name) > 1:
                self.fail('probe queried a new session list after validating its snapshot')
            return [{'mmp': {'srtt_ms': 1}}]
        scenario.sessions = sessions
        Scenario.probe(scenario, source, expect_preexisting=True)
        self.assertEqual(queried, ['source', 'target'])


if __name__ == '__main__':
    unittest.main()
