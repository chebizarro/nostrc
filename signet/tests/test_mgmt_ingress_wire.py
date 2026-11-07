#!/usr/bin/env python3
"""Real management ingress across history/EOSE/live, a socket reconnect, a
peer relay that never reaches EOSE and a relay-set reload.

The fixture relay behaves like a real one: every REQ receives the
connection's history and its own EOSE, and a live event is fanned out to
every open REQ it matches, oldest first. signetd installs two REQs with the
same filter on a fast relay at startup, so the relay answers fresh requests on
the older one and the pool forwards only that first copy (nostrc-5xqun)."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from test_relay_pool_wire import Relay


def record_req(subscriptions, sid):
    # Reconnect can re-fire old REQs after the newly installed required REQ.
    # Only a previously unseen ID advances the fixture's required destination.
    if sid not in subscriptions:
        subscriptions.append(sid)
    return subscriptions[-1]


def test_req_replay_order():
    for replay in [('old-a', 'old-b', 'current'),
                   ('current', 'old-a', 'old-b'),
                   ('old-b', 'current', 'old-a')]:
        subscriptions = []
        assert record_req(subscriptions, 'old-a') == 'old-a'
        assert record_req(subscriptions, 'old-b') == 'old-b'
        for sid in replay:
            current = record_req(subscriptions, sid)
        assert current == 'current', (replay, subscriptions)
        assert subscriptions == ['old-a', 'old-b', 'current']


def fanout(open_sids, draining):
    """Live delivery order on one connection: every REQ past its EOSE,
    oldest first. A REQ still draining history is not a live destination."""
    return [sid for sid in open_sids if sid != draining]


def test_fanout_order():
    assert fanout(['startup', 'required'], None) == ['startup', 'required']
    assert fanout(['refired-a', 'refired-b', 'new'], None) == ['refired-a', 'refired-b', 'new']
    assert fanout(['draining', 'required'], 'draining') == ['required']


def dead_relay(state):
    """A peer that accepts REQs but never reaches EOSE (relay.nostr.band in
    the 2026-11-07 drill). It must not gate admission on the live relay."""
    def handle(relay, frame):
        if frame[0] == 'REQ':
            state['dead_requests'] += 1
        elif frame[0] == 'EVENT':
            relay.send(['OK', frame[1]['id'], True, ''])
    return Relay(handle)


def run(binary, mode):
    isolate = mode in ('isolation', 'combined')
    phases = 2 if mode in ('ingress', 'combined', 'reload') else 1
    peer = mode in ('multi', 'reload')
    with tempfile.TemporaryDirectory(prefix='signet-ingress-') as tmp:
        fixtures = Path(tmp) / 'history.json'
        state = {'requests': 0, 'sid': None, 'draining': None, 'live': 0,
                 'connections': 0, 'startup_requests': 0, 'warmed': False,
                 'late_sent': False, 'dead_requests': 0}
        current_socket = None
        subscriptions = []
        open_sids = []

        def handle(relay, frame):
            nonlocal current_socket, open_sids
            if frame[0] == 'REQ':
                state['requests'] += 1
                assert state['requests'] <= 12, state
                if relay.sock is not current_socket:
                    current_socket = relay.sock
                    state['connections'] += 1
                    open_sids = []
                    state['warmed'] = False
                    state['draining'] = None
                assert state['connections'] <= 2, state
                sid = frame[1]
                assert sid not in open_sids, (sid, open_sids)
                open_sids.append(sid)
                if state['connections'] == 1:
                    state['startup_requests'] += 1
                state['sid'] = record_req(subscriptions, sid)
                history = json.loads(fixtures.read_text())
                # Every REQ sees this connection's history, deduplicated by
                # event id. Fresh/future-dated history: timestamp skew alone
                # cannot distinguish it from a live command.
                relay.send(['EVENT', sid, history[state['connections'] - 1]])
                if isolate and state['connections'] == 1 and len(open_sids) == 1:
                    # The startup REQ keeps draining while the one installed
                    # on CONNECTED reaches EOSE. A ready relay must not turn
                    # the draining REQ into a live source.
                    state['draining'] = sid
                    return
                relay.send(['EOSE', sid])
            elif frame[0] == 'EVENT':
                event = frame[1]
                relay.send(['OK', event['id'], True, ''])
                if event['kind'] == 12345:
                    # Close the WebSocket normally so reconnect observes an
                    # explicit peer close rather than a half-shutdown socket.
                    with relay.lock:
                        relay.sock.sendall(b'\x88\x00')
                    return
                assert event['kind'] == 1059, event['kind']
                state['live'] += 1
                if not state['warmed']:
                    # The warm-up is answered on the required REQ alone, so
                    # the pool has processed every older REQ's EOSE before
                    # the fan-out below reaches it.
                    state['warmed'] = True
                    relay.send(['EVENT', state['sid'], event])
                    return
                # signetd's startup leaves two REQs with this filter on the
                # relay; the regression needs the older one to carry the
                # fresh request.
                assert state['startup_requests'] >= 2, state
                draining = state['draining']
                if draining and not state['late_sent']:
                    state['late_sent'] = True
                    # A distinct future-dated mutation on the draining REQ,
                    # after the required REQ is live. Dedup cannot mask
                    # incorrect relay-only admission.
                    history = json.loads(fixtures.read_text())
                    relay.send(['EVENT', draining, history[2]])
                for sid in fanout(open_sids, draining):
                    relay.send(['EVENT', sid, event])

        relay = Relay(handle)
        dead = dead_relay(state) if peer else None
        try:
            env = {key: value for key, value in os.environ.items()
                   if not key.startswith('CLAUDE_CODE_') and key != 'SSH_AUTH_SOCK'}
            argv = [binary, '--wire-' + mode, relay.url, str(fixtures)]
            if dead:
                argv.append(dead.url)
            try:
                result = subprocess.run(argv, env=env, capture_output=True,
                                        text=True, timeout=20)
            except subprocess.TimeoutExpired as exc:
                raise AssertionError((state, exc.stdout, exc.stderr)) from exc
            assert result.returncode == 0, (result.returncode, state, result.stdout + result.stderr)
            assert state['connections'] == phases, state
            assert state['startup_requests'] >= 2, state
            # warm-up, stale and live publications per phase
            assert state['live'] == phases * 3, state
            if dead:
                assert state['dead_requests'] >= 1, state
            print(result.stdout, end='')
        finally:
            relay.close()
            if dead:
                dead.close()


if __name__ == '__main__':
    test_req_replay_order()
    test_fanout_order()
    for mode in ('ingress', 'isolation', 'combined', 'multi', 'reload'):
        run(sys.argv[1], mode)
