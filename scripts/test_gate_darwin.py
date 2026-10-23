"""test-gate's explicit native Darwin custody adapter (A-owned wiring of C's helper).

Diagnostic only: never .23/profile acceptance. Imported by scripts/test-gate.sh.
"""
import hashlib
import json
import os
from pathlib import Path
import pwd
import re
import select
import signal
import subprocess
import sys
import tempfile
import uuid

# Native seal trust boundary (read-only admission, fail closed). The finalizer's
# PT_DENY_ATTACH closes ptrace attach only; whether a same-UID process can take
# its task port (and so its private seal pipe) is AMFI/taskgated host policy.
# Admitted only when that policy has no developer/admin authorization path and
# a throwaway debugger-entitled probe is actually denied. Never mutates
# DevToolsSecurity, group membership or any developer executable's signature.
ADMISSION_TOOLS = {'csrutil': '/usr/bin/csrutil', 'devtools': '/usr/sbin/DevToolsSecurity',
                   'dseditgroup': '/usr/sbin/dseditgroup', 'codesign': '/usr/bin/codesign', 'cc': '/usr/bin/cc'}
TASKPORT_PROBE = r'''#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
  mach_port_t task = MACH_PORT_NULL;
  kern_return_t kr = task_for_pid(mach_task_self(), atoi(argv[1]), &task);
  printf("%d\n", kr);
  return kr == KERN_SUCCESS ? 10 : 0;
}
'''
DEBUGGER_ENTITLEMENT = (b'<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" '
                        b'"http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n<plist version="1.0"><dict>'
                        b'<key>com.apple.security.cs.debugger</key><true/></dict></plist>\n')


def _read_only(argv, timeout=20):
    try:
        result = subprocess.run(argv, capture_output=True, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return None, type(exc).__name__
    return result.returncode, (result.stdout + result.stderr).decode(errors='replace').strip()


def seal_admission(work):
    """Fail-closed admission of the native seal trust boundary on this host."""
    record = {'kind': 'darwin_seal_admission_v1', 'admitted': False, 'checks': {}, 'refusals': [],
              'boundary': 'same-UID task-port denial for the finalizer interpreter; ptrace attach closed by PT_DENY_ATTACH',
              'not_claimed': 'hostile-code isolation, root/admin attackers, hardened arbitrary-macOS parity'}
    checks, refusals = record['checks'], record['refusals']
    if sys.platform != 'darwin' or os.geteuid() == 0:
        refusals.append('requires a non-root macOS user')
    missing = [name for name, path in ADMISSION_TOOLS.items() if not os.access(path, os.X_OK)]
    if missing:
        refusals.append('admission tools unavailable: ' + ', '.join(missing))
    if not refusals:
        user = pwd.getpwuid(os.geteuid()).pw_name
        code, out = _read_only([ADMISSION_TOOLS['csrutil'], 'status'])
        checks['sip'] = out
        if code != 0 or out != 'System Integrity Protection status: enabled.':
            refusals.append('SIP not verifiably enabled')
        code, out = _read_only([ADMISSION_TOOLS['devtools'], '-status'])
        checks['devtools_security'] = out
        if code != 0 or out != 'Developer mode is currently disabled.':
            refusals.append('DevToolsSecurity not verifiably disabled')
        for group in ('_developer', 'admin'):
            code, out = _read_only([ADMISSION_TOOLS['dseditgroup'], '-o', 'checkmember', '-m', user, group])
            checks['member_' + group] = out
            if not out or not out.startswith('no '):
                # Members can authorize the taskport right (or the answer is unknown).
                refusals.append(f'user is (or may be) a member of {group}')
        finalizer = Path(os.path.realpath(sys.executable))
        checks['finalizer'] = {'path': str(finalizer), 'sha256': hashlib.sha256(finalizer.read_bytes()).hexdigest()}
        code, out = _read_only([ADMISSION_TOOLS['codesign'], '-dv', '--verbose=2', str(finalizer)])
        checks['finalizer']['codesign'] = out
        code_e, entitlements = _read_only([ADMISSION_TOOLS['codesign'], '-d', '--entitlements', '-', '--xml', str(finalizer)])
        checks['finalizer']['entitlements'] = entitlements
        if code != 0 or code_e != 0 or 'get-task-allow' in (entitlements or ''):
            refusals.append('finalizer executable signature/entitlements unknown or debuggable (get-task-allow)')
    if not refusals:
        # Only now (no developer/admin authorization path, so no interactive
        # prompt) actively prove denial with a throwaway entitled debugger.
        with tempfile.TemporaryDirectory(prefix='nostrc-taskport-probe-') as tmp:
            source, binary, plist = Path(tmp) / 'probe.c', Path(tmp) / 'probe', Path(tmp) / 'debugger.plist'
            source.write_text(TASKPORT_PROBE)
            plist.write_bytes(DEBUGGER_ENTITLEMENT)
            built = _read_only([ADMISSION_TOOLS['cc'], '-O0', str(source), '-o', str(binary)], 120)
            signed = _read_only([ADMISSION_TOOLS['codesign'], '-s', '-', '-f', '--entitlements', str(plist), str(binary)])
            if built[0] != 0 or signed[0] != 0:
                refusals.append('task-port denial probe could not be built/signed')
            else:
                target = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])
                try:
                    code, out = _read_only([str(binary), str(target.pid)], 15)
                finally:
                    target.kill()  # direct, unreaped child
                    target.wait()
                checks['taskport_probe'] = {'exit': code, 'kern_return': out}
                if code != 0 or not out or out == '0':
                    refusals.append('same-UID entitled task_for_pid was not deterministically denied')
    record['admitted'] = not refusals
    with open(Path(work) / ('darwin-seal-admission-' + uuid.uuid4().hex + '.json'), 'x') as stream:
        json.dump(record, stream, sort_keys=True, indent=2)
    return record


DARWIN_NAME = re.compile(r'org\.nostrc\.custody\.([0-9a-f]{32})\.(a|b)')
DARWIN_CONTROL_LIMIT = 4096  # protocol is < 1 KiB; anything larger is not this helper


def darwin_control(read_fd):
    """Read the helper's private control pipe to EOF after it exited.

    Only these bytes are authoritative (evidence/tag files are same-UID
    writable). Returns (tag, final, protocol_error)."""
    data = b''
    try:
        while len(data) <= DARWIN_CONTROL_LIMIT:
            ready, _, _ = select.select([read_fd], [], [], 10)
            if not ready:
                return None, None, 'control pipe did not reach EOF after helper exit'
            block = os.read(read_fd, DARWIN_CONTROL_LIMIT + 1 - len(data))
            if not block:
                break
            data += block
    finally:
        os.close(read_fd)
    if len(data) > DARWIN_CONTROL_LIMIT or (data and not data.endswith(b'\n')):
        return None, None, 'control pipe protocol violation (size/termination)'
    tag = final = None
    for raw in data.splitlines():
        try:
            line = json.loads(raw)
        except ValueError:
            return tag, None, 'control pipe protocol violation (not JSON)'
        event = line.get('event') if isinstance(line, dict) else None
        if event == 'tag' and tag is None and final is None:
            denied, allowed = DARWIN_NAME.fullmatch(str(line.get('denied'))), DARWIN_NAME.fullmatch(str(line.get('allowed')))
            if (not denied or not allowed or denied.group(2) != 'a' or allowed.group(2) != 'b'
                    or denied.group(1) == allowed.group(1)):
                return None, None, 'control pipe tag line invalid'
            tag = line
        elif event == 'final' and final is None:
            final = line
        else:
            return tag, None, 'control pipe protocol violation (unexpected/duplicate event)'
    return tag, final, None


def darwin_clean(final, authority, code=None):
    """Control-pipe final with complete cleanup (and, for the supervisor, its
    exit code equal to the helper's own return code); never files."""
    return bool(isinstance(final, dict) and final.get('cleanup_complete') is True and final.get('error') is None
                and 'evidence_file_error' not in final and final.get('tag_authority') == authority
                and (authority != 'supervisor' or (final.get('launched') is True and final.get('exit_code') == code)))


def darwin_spawn(log, label, env=None):
    control_read, control_write = os.pipe()
    evidence = Path(f'{log}.darwin-{label}-{uuid.uuid4().hex}.json')  # fresh: no collision with prior files
    child_env = {k: v for k, v in (os.environ if env is None else env).items() if k != 'NOSTRC_DARWIN_CUSTODY_TAG_FILE'}
    child_env['NOSTRC_DARWIN_CUSTODY_EVIDENCE'] = str(evidence)
    return control_read, control_write, evidence, child_env


def darwin_recover(helper, tag, log):
    """Exactly one parent-bound recovery with the adapter-held original names."""
    control_read, control_write, evidence, child_env = darwin_spawn(log, 'recovery')
    with open(f'{log}.darwin-recovery.log', 'xb') as stream:
        try:
            recovery = subprocess.Popen([sys.executable, str(helper), '--control-fd', str(control_write),
                                         '--reap-tag', tag['denied'], tag['allowed']], env=child_env,
                                        pass_fds=(control_write,), stdout=stream, stderr=subprocess.STDOUT)
        finally:
            os.close(control_write)
        try:
            code = recovery.wait(timeout=120)
        except subprocess.TimeoutExpired:
            recovery.kill()  # direct, unreaped child: identity reserved
            code = recovery.wait()
    _, final, error = darwin_control(control_read)
    return {'recovery_exit': code, 'recovery_final': final, 'recovery_protocol_error': error,
            'recovery_informational_evidence': str(evidence),
            'recovered': error is None and code == 0 and darwin_clean(final, 'parent-bound')}


def darwin_command(argv, *, helper, records, errors, cwd=None, env=None, log=None, native_lane=None):
    """Explicit macOS seatbelt-tag custody (C helper 131695d0b); diagnostic, never parity.

    The helper is this adapter's direct, unreaped child, so os.kill(pid, 0)
    before gate release and Popen signals on cancel are identity-safe until it
    is waited. Its private control pipe (close-on-exec, never held by the
    target) carries the tag line before the target is released and the final
    line last: only those bytes decide success or authorize recovery.
    """
    active = None
    if log is None:
        raise RuntimeError('darwin custody requires a retained log/evidence path; target not launched')
    record = {'kind': 'darwin_custody_adapter_v1', 'log': str(log), 'status': 'refused_before_launch',
              'complete': False, 'complete_gate_eligible': False, 'untraced_profile_parity': False}
    records.append(record)
    stream = open(log, 'xb')
    gate_read, gate_write = os.pipe()
    seal_read, seal_write = os.pipe() if native_lane else (-1, -1)
    control_read, control_write, evidence, child_env = darwin_spawn(log, 'custody', env=env)
    record['informational_evidence'] = str(evidence)
    if native_lane:
        admission = seal_admission(native_lane)
        record['seal_admission'] = admission
        if not admission['admitted']:
            stream.close()
            for fd in (gate_read, gate_write, seal_read, seal_write, control_read, control_write):
                os.close(fd)
            with open(f'{log}.darwin-adapter.json', 'x') as output:
                json.dump(record, output, sort_keys=True, indent=2)
            raise RuntimeError('darwin native seal trust boundary not established on this host ('
                               + '; '.join(admission['refusals']) + '); target not launched')
    waited, code, failure, clean = False, None, None, False
    try:
        try:
            mask = signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGTERM, signal.SIGINT})
            try:
                if native_lane:
                    child_env['NOSTRC_NATIVE_SEAL_FD'] = str(seal_write)
                active = subprocess.Popen([sys.executable, str(helper), '--control-fd', str(control_write),
                                           '--parent-ready-fd', str(gate_read), '--parent-pid', str(os.getpid()),
                                           *map(str, argv)], cwd=cwd, env=child_env,
                                          pass_fds=tuple(fd for fd in (control_write, gate_read, seal_write) if fd != -1),
                                          stdout=stream, stderr=subprocess.STDOUT)
                for fd in (control_write, seal_write):
                    if fd != -1:
                        os.close(fd)
                control_write = seal_write = -1
                os.kill(active.pid, 0)  # direct, unreaped child: PID still reserved
                if signal.sigpending() & {signal.SIGTERM, signal.SIGINT}:
                    raise KeyboardInterrupt()
                os.write(gate_write, b'1')
                record['status'] = 'gate_released'
            finally:
                for fd in (gate_read, gate_write):
                    os.close(fd)
                gate_read = gate_write = -1
                signal.pthread_sigmask(signal.SIG_SETMASK, mask)
            code = active.wait()
            waited = True
        except BaseException as exc:
            failure = exc
            cancellation_mask = signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGTERM, signal.SIGINT})
            handlers = {sig: signal.signal(sig, signal.SIG_IGN) for sig in (signal.SIGTERM, signal.SIGINT)}
            signal.pthread_sigmask(signal.SIG_SETMASK, cancellation_mask)
            try:
                if active is not None:
                    active.send_signal(signal.SIGTERM)  # Popen refuses once reaped: identity-safe
                    try:
                        code = active.wait(timeout=120)
                    except subprocess.TimeoutExpired:
                        active.kill()  # still unreaped
                        code = active.wait()
                    waited = True
            finally:
                for sig, handler in handlers.items():
                    signal.signal(sig, handler)
        tag, final, protocol_error = darwin_control(control_read) if waited else (None, None, 'helper not waited')
        control_read = -1
        record.update(helper_exit=code, control_tag=tag, control_final=final, control_protocol_error=protocol_error)
        clean = protocol_error is None and tag is not None and darwin_clean(final, 'supervisor', code)
        if clean:
            record['status'] = 'clean'
        elif tag is not None:
            # A target may have run: one parent-bound recovery; lane stays failed.
            record.update(darwin_recover(helper, tag, log))
            record['status'] = 'recovered_lane_failed' if record['recovered'] else 'cleanup_unverified'
        elif protocol_error is None and isinstance(final, dict) and final.get('launched') is False:
            record['status'] = 'refused_before_launch'  # no tag line: no target was released
        else:
            record['status'] = 'cleanup_unverified'  # unparseable/missing authority: never inferred clean
        record['cleanup_complete'] = clean
        if failure is not None:
            raise failure
        if not clean:
            raise RuntimeError(f'darwin custody {record["status"]}: helper exit {code}; log={log}')
        if code:
            raise RuntimeError(f"command exited {code}: {Path(str(argv[0])).name}; log={log}")
    finally:
        for fd in (gate_read, gate_write, control_write, control_read, seal_write):
            if fd != -1:
                os.close(fd)
        stream.close()
        try:
            with open(f'{log}.darwin-adapter.json', 'x') as output:
                json.dump(record, output, sort_keys=True, indent=2)
        except OSError:
            pass
        if seal_read != -1:
            original = sys.exc_info()[1]
            try:
                # The seal anchor is read only after a valid clean control final;
                # seal EOF itself is not the barrier.
                if not waited or not clean:
                    raise RuntimeError('darwin custody cleanup barrier was not observed')
                from test_selection_services import attest_native_lane
                anchor = os.read(seal_read, 65).decode('ascii')
                final_record = attest_native_lane(native_lane, anchor)
                with (native_lane / 'native-final.json').open('x') as output:
                    json.dump(final_record, output, sort_keys=True, indent=2)
                    output.write('\n')
            except Exception as exc:
                try:
                    with (native_lane / 'native-final.json').open('x') as output:
                        json.dump({'status': 'failed', 'error': str(exc), 'complete': False,
                                   'complete_gate_eligible': False}, output, sort_keys=True)
                        output.write('\n')
                except FileExistsError:
                    pass
                if original is None:
                    raise RuntimeError('native final evidence refused: ' + str(exc)) from exc
                errors.append('native final evidence refused: ' + str(exc))
            finally:
                os.close(seal_read)
