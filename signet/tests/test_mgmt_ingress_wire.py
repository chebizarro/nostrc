#!/usr/bin/env python3
"""Real management ingress across history/EOSE/live and a socket reconnect."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from test_relay_pool_wire import Relay


def run(binary, isolate):
    with tempfile.TemporaryDirectory(prefix='signet-ingress-') as tmp:
        fixtures = Path(tmp) / 'history.json'
        state = {'requests': 0, 'sid': None, 'old_sid': None,
                 'live': 0, 'connections': 0, 'late_sent': False}
        current_socket = None

        def handle(relay, frame):
            nonlocal current_socket
            if frame[0] == 'REQ':
                state['requests'] += 1
                assert state['requests'] <= 8, state
                if relay.sock is not current_socket:
                    current_socket = relay.sock
                    state['connections'] += 1
                assert state['connections'] <= 2, state
                # Startup and reconnect can re-fire existing REQs as well as
                # install the pool's new required subscription. Each sees the
                # same history for this connection, deduplicated by event id.
                state['sid'] = frame[1]
                if state['old_sid'] is None:
                    state['old_sid'] = frame[1]
                history = json.loads(fixtures.read_text())
                # Include fresh/future-dated history: timestamp skew alone
                # cannot distinguish this from a live command.
                relay.send(['EVENT', frame[1], history[state['connections'] - 1]])
                relay.send(['EOSE', frame[1]])
            elif frame[0] == 'EVENT':
                event = frame[1]
                relay.send(['OK', event['id'], True, ''])
                if event['kind'] == 12345:
                    # Close the WebSocket normally so reconnect observes an
                    # explicit peer close rather than a half-shutdown socket.
                    with relay.lock:
                        relay.sock.sendall(b'\x88\x00')
                else:
                    assert event['kind'] == 1059, event['kind']
                    state['live'] += 1
                    if isolate and not state['late_sent']:
                        state['late_sent'] = True
                        # A distinct future-dated mutation on an older REQ,
                        # after the current required subscription is live.
                        # Dedup cannot mask incorrect relay-only admission.
                        assert state['old_sid'] != state['sid'], state
                        history = json.loads(fixtures.read_text())
                        relay.send(['EVENT', state['old_sid'], history[2]])
                    relay.send(['EVENT', state['sid'], event])

        relay = Relay(handle)
        try:
            env = {key: value for key, value in os.environ.items()
                   if not key.startswith('CLAUDE_CODE_') and key != 'SSH_AUTH_SOCK'}
            try:
                result = subprocess.run([binary, '--wire-isolation' if isolate else '--wire-ingress',
                                         relay.url, str(fixtures)],
                                        env=env, capture_output=True, text=True, timeout=20)
            except subprocess.TimeoutExpired as exc:
                raise AssertionError((state, exc.stdout, exc.stderr)) from exc
            assert result.returncode == 0, (result.returncode, state, result.stdout + result.stderr)
            assert state['connections'] == (1 if isolate else 2), state
            assert state['live'] == (2 if isolate else 4), state
            print(result.stdout, end='')
        finally:
            relay.close()


if __name__ == '__main__':
    run(sys.argv[1], isolate=True)
    run(sys.argv[1], isolate=False)
