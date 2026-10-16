#!/usr/bin/env python3
"""Container-only shared-engine runner. Host finalization is still mandatory."""
import importlib.util
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys

import test_selection as selection
import test_selection_realization as backend
from test_selection_custody import PreparedContext, CustodyError, canonical, digest, file_hash, retained_files, retained_claims, write_json


def provision(capabilities, state):
    """Read actual service readiness, never sleep and infer availability."""
    unsupported = set(capabilities) - {'display', 'private_bus', 'external_peer', 'prepared_peer'}
    if unsupported:
        raise RuntimeError('unsupported contained capabilities: ' + repr(sorted(unsupported)))
    if 'private_bus' in capabilities:
        log = open('/work/private-bus.raw', 'wb')
        argv = ['dbus-daemon', '--session', '--nofork', '--print-address=1', '--address=unix:path=/tmp/contained-bus']
        bus = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=log)
        address = bus.stdout.readline().strip().decode()
        if not address or not address.startswith('unix:'):
            raise RuntimeError('private bus did not report readiness')
        os.environ['DBUS_SESSION_BUS_ADDRESS'] = address
        state['private_bus'] = {'argv': argv, 'address': address, 'pid': bus.pid}
    if 'display' in capabilities:
        read, write = os.pipe()
        argv = ['Xvfb', '-displayfd', str(write), '-screen', '0', '1280x1024x24', '-nolisten', 'tcp']
        with open('/work/display.raw', 'wb') as log:
            display = subprocess.Popen(argv, pass_fds=(write,), stdout=log, stderr=subprocess.STDOUT)
        os.close(write)
        with os.fdopen(read, 'rb') as stream:
            number = stream.readline().strip().decode()
        if not number.isdecimal():
            raise RuntimeError('private display did not report readiness')
        os.environ['DISPLAY'] = ':' + number
        state['display'] = {'argv': argv, 'display': os.environ['DISPLAY'], 'pid': display.pid}


def main():
    control = json.loads(Path('/control/run.json').read_text())
    context = PreparedContext(control)
    state = {'status': 'starting', 'run_id': control['owner']['run_id'], 'container_id': control['container_id']}
    def cancelled(signum, frame):
        raise KeyboardInterrupt('contained runner cancelled')
    signal.signal(signal.SIGTERM, cancelled)
    code = 1
    try:
        policy = selection.load_inventory('/control/inventory.json')
        plan = selection.read_json('/control/plan.json')
        selection.validate_plan(plan, policy)
        profile = policy['profiles'][control['binding']['profile_id']]
        if selection.digest(policy) != control['binding']['inventory_hash'] or profile != control['profile']:
            raise RuntimeError('contained policy/profile binding mismatch')
        capabilities = set(profile['capabilities'])
        for check in plan['lanes']:
            if check['profile'] == control['binding']['profile_id']:
                for ident in check['checks']:
                    capabilities.update(policy['checks'][ident].get('requires', []))
        provision(capabilities, state)
        write_json('/work/phase-state.json', state)
        spec = importlib.util.spec_from_file_location('contained_plan_cli', '/source/scripts/test-plan.py')
        cli = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cli)
        args = cli.parser().parse_args(['run', '--repo', '/source', '--inventory', '/control/inventory.json',
            '--plan', '/control/plan.json', '--profile', control['binding']['profile_id'],
            '--build-dir', '/work/build', '--evidence-dir', '/work/execution', '--configure',
            '--parallel', str(profile.get('parallelism', 2)), '--timeout', str(control['timeout'])])
        result, code = cli.realization_command(args, policy, backend, custody=context)
        realized = backend.load_realization('/work/build', plan['source_identity'])
        artifacts = {}
        vectors = Path('/work/build/mdk011-interop-artifacts/vectors.jsonl')
        if vectors.is_file():
            artifacts[str(vectors)] = file_hash(vectors)
        retained = retained_files('/work')
        for path, expected in retained_claims(result).items():
            if retained.get(path) != {'kind': 'file', 'sha256': expected}:
                raise CustodyError('retained evidence already differs from receipt: ' + path)
        state.update(artifacts=artifacts, retained_evidence=retained, retained_evidence_hash=digest(retained),
                     realization_fingerprint=realized['profile_fingerprint'], phase_context=realized['phase_context'],
                     phase_context_hash=realized['phase_context_hash'], configure_argv=realized['commands'][0] if realized['commands'] else None,
                     prepared_cache=context.cache_values, compiler_toolchains=realized.get('toolchains'),
                     effective_configure_args=realized['profile']['configure_args'],
                     cache_sha256=file_hash('/work/build/CMakeCache.txt') if Path('/work/build/CMakeCache.txt').is_file() else None,
                     backend_sha256=hashlib.sha256(canonical(result) + b'\n').hexdigest(), status='succeeded' if code == 0 else 'failed')
        # Publish only after sealing. Escaped writers remain owned until exit;
        # the host must compare these original claims with final filesystem bytes.
        write_json('/work/backend.json', result)
    except KeyboardInterrupt as exc:
        state.update(status='cancelled', error=str(exc))
        code = 130
    except Exception as exc:
        state.update(status='failed', error=type(exc).__name__ + ': ' + str(exc))
        print(state['error'], file=sys.stderr, flush=True)
    finally:
        # Services/escaped descendants stay inside this PID namespace. Returning
        # terminates its init; only the host can attest exit/drain/removal.
        write_json('/work/phase-state.json', state)
    return code


if __name__ == '__main__':
    sys.exit(main())
