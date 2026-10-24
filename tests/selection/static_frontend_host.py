#!/usr/bin/env python3
"""Owned host self-test group for the stable static-checks job (scheduling only).

The nine original host self-tests keep their exact argv, host environment,
checkout cwd and order, but run inside ONE supervisor started beside the image
build (nostrc-0cfsg.36.26). Ownership is never inferred from a bare number:

* start: INT/TERM blocked; the supervisor is launched in its own session and
  inherits that mask; it becomes the child subreaper and proves it can list its
  children, then reports ready. Only then is owner.json (random token, pid,
  kernel start time, boot id, uid) atomically published and "go" sent. A start
  that dies earlier closes the handshake; the supervisor then exits having run
  nothing, so no unpublished group can execute.
* the supervisor stays alive until released, so its pid cannot be reused. It
  runs each command in that command's own process group and signals only its
  own unreaped children/groups; orphans the commands leave reparent to it and
  are ended from its own child list (no host scan). It seals final.json (exits,
  log hashes, orphans ended, cancellation) after everything it owns has ended.
* join / ensure-ended signal only a supervisor whose token, start time, boot
  id, uid and session leadership are re-verified; a forged or stale record is
  refused without any signal. INT/TERM or a cancelled job take the bounded
  cancel path (TERM, then KILL inside the runner's grace).
* replay NAME reports exactly one sealed command result; missing, duplicate
  or unexecuted results fail. Nothing here is a gate, a deadline on the
  commands, or a synthetic success.
"""
import hashlib
import json
import os
from pathlib import Path
import secrets
import select
import signal
import subprocess
import sys
import time

# BEGIN COMMANDS (the original static-checks host steps, in order)
COMMANDS = (
    ('unsequenced', ('python3', 'scripts/check-unsequenced-args.py', '--self-test')),
    ('gui-frame', ('python3', 'scripts/check-gui-test-gdk-frame.py', '--self-test')),
    ('linux-packages', ('python3', 'scripts/check-linux-ci-packages.py', '--self-test')),
    ('pre-push', ('bash', 'scripts/test-pre-push.sh')),
    ('full-gate', ('bash', 'scripts/test-full-gate.sh')),
    ('linux-smoke', ('bash', 'scripts/test-linux-gate-smoke.sh')),
    ('sanitizer', ('bash', 'scripts/test-sanitizer-gate.sh')),
    ('signer-names', ('bash', 'scripts/check-grotto-names.sh')),
    ('keychain-guard', ('bash', 'scripts/test-check-macos-keychain.sh')),
)
# END COMMANDS
KIND = 'static_host_frontend_group_v1'
GRACE = 2.0          # command group TERM -> KILL
CHILD_GRACE = 1.0    # leftover children TERM -> KILL
OWNER_WAIT = 5.0     # join/ensure-ended: supervisor end after TERM, else KILL (runner grace)
PR_SET_CHILD_SUBREAPER = 36
CONTROL = {signal.SIGINT, signal.SIGTERM, signal.SIGUSR1}


class Refused(Exception):
    pass


def root():
    return Path(os.environ['RUNNER_TEMP']) / 'namespace-policy' / 'host-frontend'


def write_json(path, value):
    partial = path.with_name('.' + path.name + '.partial')
    with open(partial, 'x') as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write('\n')
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(partial, path)


def boot_id():
    return Path('/proc/sys/kernel/random/boot_id').read_text().strip()


def stat(pid):
    """(state, session, starttime) of a live or zombie pid, or None."""
    try:
        raw = Path('/proc/%d/stat' % pid).read_text()
    except (FileNotFoundError, ProcessLookupError):
        return None
    fields = raw[raw.rindex(')') + 2:].split()
    return fields[0], int(fields[3]), int(fields[19])


def children():
    found = []
    for task in Path('/proc/self/task').iterdir():
        found += [int(p) for p in (task / 'children').read_text().split()]
    return sorted(set(found))


def cmdline(pid):
    try:
        return Path('/proc/%d/cmdline' % pid).read_bytes().replace(b'\0', b' ').decode(errors='replace').strip()
    except OSError:
        return ''


# ---------------------------------------------------------------- ownership
def owner_status(record):
    """'alive' (verified owned supervisor), 'ended', or raise Refused."""
    if not isinstance(record, dict) or record.get('kind') != KIND:
        raise Refused('ownership record malformed')
    if record.get('boot_id') != boot_id() or record.get('uid') != os.getuid():
        raise Refused('ownership record is from another boot or user')
    pid, start = record.get('pid'), record.get('starttime')
    if type(pid) is not int or type(start) is not int or pid <= 1:
        raise Refused('ownership record malformed')
    observed = stat(pid)
    if observed is None or observed[2] != start:
        return 'ended'  # that incarnation is gone; a reused number is never ours
    state, session, _ = observed
    if state == 'Z':
        return 'ended'  # exited; only its reaper may remove it
    try:
        info = os.stat('/proc/%d' % pid)
        argv = Path('/proc/%d/cmdline' % pid).read_bytes().split(b'\0')
    except OSError:
        return 'ended'
    if session != pid or info.st_uid != os.getuid() or record.get('token', '').encode() not in argv:
        raise Refused('pid %d with the recorded start time is not the owned supervisor' % pid)
    return 'alive'


def signal_owned(record, signum):
    if owner_status(record) == 'alive':  # re-verified immediately before every signal
        os.kill(record['pid'], signum)
        return True
    return False


def read_owner(directory):
    try:
        return json.loads((directory / 'owner.json').read_text())
    except FileNotFoundError:
        return None
    except ValueError as exc:
        raise Refused('ownership record malformed') from exc


# ---------------------------------------------------------------- start
def start():
    directory = root()
    sha = os.environ['REQUESTED_SHA']
    head = subprocess.run(['git', 'rev-parse', 'HEAD'], capture_output=True, text=True, check=True).stdout.strip()
    if head != sha:
        raise Refused('checkout is not the requested commit')
    try:
        directory.mkdir(parents=True, exist_ok=False)
    except FileExistsError as exc:
        raise Refused('a host self-test group already exists for this job') from exc
    (directory / 'logs').mkdir()
    token = secrets.token_hex(32)
    go_read, go_write = os.pipe()
    ready_read, ready_write = os.pipe()
    previous = signal.pthread_sigmask(signal.SIG_BLOCK, CONTROL)  # launch and publication are not interruptible
    process = None
    try:
        with open(directory / 'supervisor.log', 'xb') as log:
            process = subprocess.Popen(
                [sys.executable, '-I', os.path.abspath(__file__), 'supervise', str(directory), token,
                 str(go_read), str(ready_write)],
                stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, start_new_session=True,
                pass_fds=(go_read, ready_write))
        os.close(go_read)
        os.close(ready_write)
        ready, _, _ = select.select([ready_read], [], [], 30)
        if not ready or os.read(ready_read, 16) != b'ready\n':
            raise Refused('supervisor did not become ready')
        observed = stat(process.pid)
        if observed is None or observed[1] != process.pid:
            raise Refused('supervisor is not its own session leader')
        record = dict(kind=KIND, token=token, pid=process.pid, starttime=observed[2], boot_id=boot_id(),
                      uid=os.getuid(), scope='scheduling_only_not_acceptance')
        write_json(directory / 'owner.json', record)
        os.write(go_write, b'go\n')
        print('host self-test group %d started beside the image build' % process.pid)
    except BaseException:
        if process is not None and process.poll() is None:
            process.kill()  # our own unreaped child: its pid cannot have been reused
            process.wait()
        raise
    finally:
        for fd in (go_write, ready_read):
            try:
                os.close(fd)
            except OSError:
                pass
        signal.pthread_sigmask(signal.SIG_SETMASK, previous)


# ---------------------------------------------------------------- supervisor
def supervise(directory, token, go_fd, ready_fd):
    directory = Path(directory)
    flags = {'cancel': False, 'release': False}
    signal.signal(signal.SIGTERM, lambda *_: flags.__setitem__('cancel', True))
    signal.signal(signal.SIGINT, lambda *_: flags.__setitem__('cancel', True))
    signal.signal(signal.SIGUSR1, lambda *_: flags.__setitem__('release', True))
    import ctypes
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0:
        return 4
    children()  # proves the child list is readable before anything runs
    os.write(ready_fd, b'ready\n')
    os.close(ready_fd)
    with os.fdopen(go_fd, 'rb') as go:
        if go.readline() != b'go\n':
            return 3  # publication never completed: run nothing
    signal.pthread_sigmask(signal.SIG_UNBLOCK, CONTROL)
    results, orphans = [], []
    for name, argv in COMMANDS:
        entry = dict(name=name, argv=list(argv), status='not_executed', exit=None)
        results.append(entry)
        if flags['cancel']:
            continue
        begun = time.time()
        with open(directory / 'logs' / (name + '.log'), 'xb') as log:
            process = subprocess.Popen(list(argv), stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                       process_group=0)
        entry.update(pid=process.pid, started=begun)
        while process.poll() is None:
            if flags['cancel']:
                os.killpg(process.pid, signal.SIGTERM)  # unreaped child: its group id is ours
                deadline = time.monotonic() + GRACE
                while process.poll() is None and time.monotonic() < deadline:
                    time.sleep(0.05)
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
                entry['status'] = 'cancelled'
                break
            time.sleep(0.05)
        if entry['status'] != 'cancelled':
            entry['status'] = 'exited'
        entry.update(exit=process.returncode, ended=time.time())
    orphans, stuck = end_children()
    write_json(directory / 'done.json', dict(kind=KIND, commands=[(r['name'], r['status'], r['exit']) for r in results]))
    while not (flags['release'] or flags['cancel']):
        time.sleep(0.1)
    late, still = end_children()
    survivors = stuck + still
    logs = {}
    for path in sorted((directory / 'logs').iterdir()):
        logs[path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
    cancelled = any(r['status'] != 'exited' for r in results)
    write_json(directory / 'final.json', dict(
        kind=KIND, scope='scheduling_only_not_acceptance', token_sha256=hashlib.sha256(token.encode()).hexdigest(),
        commands=results, cancelled=cancelled, completed=not cancelled and not survivors, orphans_ended=orphans + late,
        survivors=survivors, logs_sha256=logs, released_by='cancel' if flags['cancel'] else 'join',
        complete=False, complete_gate_eligible=False))
    return 0


def end_children():
    """End every process now parented to this subreaper; TERM, then KILL.

    Returns (ended, survivors); a process that cannot be killed (e.g. in
    uninterruptible sleep) is recorded as a survivor after a bounded wait."""
    ended, begun = [], time.monotonic()
    while True:
        while True:
            try:
                pid, _ = os.waitpid(-1, os.WNOHANG)
            except ChildProcessError:
                pid = 0
            if pid == 0:
                break
        alive = [pid for pid in children() if (stat(pid) or ('Z',))[0] != 'Z']
        if not alive:
            return ended, []
        if time.monotonic() - begun > CHILD_GRACE + 4:
            return ended, ['%d %s' % (pid, cmdline(pid)) for pid in alive]
        for pid in alive:
            line = '%d %s' % (pid, cmdline(pid))
            if line not in ended:
                ended.append(line)
            # Our own unreaped child: its pid cannot be reused while it exists.
            os.kill(pid, signal.SIGTERM if time.monotonic() - begun < CHILD_GRACE else signal.SIGKILL)
        time.sleep(0.05)


# ---------------------------------------------------------------- join / ensure-ended / replay
def finish(cancel_on_entry):
    directory = root()
    flags = {'cancel': cancel_on_entry}
    for signum in (signal.SIGINT, signal.SIGTERM):
        signal.signal(signum, lambda *_: flags.__setitem__('cancel', True))
    if not directory.exists():
        print('no host self-test group was started by this job; nothing owned')
        return 0
    record = read_owner(directory)
    if record is None:
        # start failed before publication; its supervisor ran nothing and exits on
        # the closed handshake. No host process can be proven ours: signal nothing.
        print('host self-test group was never published; nothing can be proven owned', file=sys.stderr)
        return 1
    sent, custody_lost = None, False
    while owner_status(record) == 'alive':
        if flags['cancel'] and sent != 'cancel':
            sent = 'cancel'
            signal_owned(record, signal.SIGTERM)
            deadline = time.monotonic() + OWNER_WAIT
            while owner_status(record) == 'alive' and time.monotonic() < deadline:
                time.sleep(0.05)
            if signal_owned(record, signal.SIGKILL):
                custody_lost = True  # its remaining children are no longer supervised
            break
        if sent is None and (directory / 'done.json').exists():
            sent = 'release'
            signal_owned(record, signal.SIGUSR1)
        time.sleep(0.05)
    final_path = directory / 'final.json'
    if custody_lost or not final_path.exists():
        print('host self-test group ended without a sealed result (custody lost=%s)' % custody_lost, file=sys.stderr)
        return 1
    final = json.loads(final_path.read_text())
    print(json.dumps({k: final[k] for k in ('completed', 'cancelled', 'orphans_ended', 'released_by')}, sort_keys=True))
    for entry in final['commands']:
        print('%-15s %-13s %s' % (entry['name'], entry['status'], entry['exit']))
    return 0 if final['completed'] else 1


def replay(name):
    directory = root()
    try:
        final = json.loads((directory / 'final.json').read_text())  # sealed after the group ended
    except (OSError, ValueError):
        print('no sealed host self-test result exists (not joined)', file=sys.stderr)
        return 1
    entries = [entry for entry in final['commands'] if entry.get('name') == name]
    expected = dict(COMMANDS).get(name)
    if expected is None or len(entries) != 1 or entries[0].get('argv') != list(expected):
        print('no single sealed result for %s' % name, file=sys.stderr)
        return 1
    entry = entries[0]
    print('$ %s  (executed in the owned host group; retained log follows)' % ' '.join(expected))
    log = directory / 'logs' / (name + '.log')
    if entry['status'] != 'exited' or type(entry['exit']) is not int:
        print('%s was %s, not executed to completion' % (name, entry['status']), file=sys.stderr)
        return 1
    data = log.read_bytes()
    if hashlib.sha256(data).hexdigest() != final['logs_sha256'].get(log.name):
        print('retained log differs from its sealed hash: ' + name, file=sys.stderr)
        return 1
    sys.stdout.write(data.decode(errors='replace'))
    return entry['exit'] if 0 <= entry['exit'] < 256 else 1


def main(argv):
    try:
        if argv[:1] == ['start']:
            start()
            return 0
        if argv[:1] == ['supervise'] and len(argv) == 5:
            return supervise(argv[1], argv[2], int(argv[3]), int(argv[4]))
        if argv[:1] == ['join']:
            return finish(os.environ.get('JOB_STATUS') == 'cancelled')
        if argv[:1] == ['ensure-ended']:
            return finish(True)
        if argv[:1] == ['replay'] and len(argv) == 2:
            return replay(argv[1])
    except Refused as exc:
        print('refused: %s' % exc, file=sys.stderr)
        return 1
    print('usage: static_frontend_host.py start|join|ensure-ended|replay NAME', file=sys.stderr)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
