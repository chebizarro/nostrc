#!/usr/bin/env python3
"""Explicit native macOS descendant custody without entitlements or ptrace.

DRAFT backend (nostrc-0cfsg.23), not accepted custody. Contract:

Ownership label. The gated root child applies a narrowing seatbelt profile,
``(allow default)`` plus one denied random Mach global name A, before it
executes the target. The label is inherited across fork, exec, setsid and
reparenting; a labelled process cannot replace or drop it (any further
sandbox_init fails with EPERM). Membership is sandbox_check() reporting A
denied while a second random, never-mentioned name B is allowed, so foreign
allow-all or deny-all profiles never match.

What the label is and is not. Every fork descendant of the root is a member.
The label is NOT an unforgeable capability: the names are random but readable
by the same UID (an optional tag file, the supervisor's memory), so a
same-UID process may voluntarily label itself. Such a forger is killed if seen
and is not guaranteed to be seen; it is never lineage evidence. Descendants
cannot leave the set, which is the property cleanup depends on.

Discovery barrier. proc_listallpids() is one proc_list_lock walk of allproc
and zombproc (XNU proc_listpids), but classifying each PID afterwards is not
atomic. A round therefore takes snapshot S0 and classifies it, then takes
snapshot S1 and classifies every identity (pid, uniqueid) not already stably
classified. A process alive at S1 is in S1 (a member still inside fork() is
itself alive in S1). An S1 identity that exits, becomes a zombie or execs
while being classified makes the round uncertain; only zombies already seen
dead during pass 1 are known dead before S1. Classification is valid only when
the live identity (uniqueid, pidversion) is unchanged before and after both
sandbox_check calls. Empty is accepted only from a certain round; uncertain
rounds are rescanned until certainty or the guard, which fails. After a
certain empty round no member can be created except by forgers.

Signals. Members are signalled only with proc_signal_with_audittoken() for
(pid, pidversion); XNU's proc_find_audit_token() refuses a changed version
(exec, PID reuse). The root is also this supervisor's direct, unreaped child.
Exits are awaited with kqueue NOTE_EXIT registered while the same identity is
live; timeouts are failure guards, never synchronization.

Trust boundary for results. Files are same-UID writable by the target, so
the tag file and evidence file are informational only. The adapter passes a
private pipe with ``--control-fd``; the target never holds it. Before the
target is released the helper writes the authoritative tag on it, and at the
end a bounded final outcome. Recovery after supervisor death must use the
tag the adapter read there (``--reap-tag DENIED ALLOWED``, parent-bound);
``--reap-tag-file`` is manual recovery with an untrusted, substitutable tag
and is labelled so. Target environments never see NOSTRC_DARWIN_CUSTODY_*.

Refused or failing, never inferred away: nested custody (sandbox_init EPERM
before exec); private display/bus wrappers; members that become another user
(signal EPERM fails cleanup); work done by launchd/XPC brokers (not a
descendant, not owned); SIGKILL of this supervisor (nothing reaps then; the
durable tag file lets ``--reap-tag-file`` finish cleanup). Not profile
parity: members are sandboxed and cannot apply another seatbelt profile.
No numeric-PID, PGID, ps-ancestry or name-based signal is used for members.
"""
import ctypes
import errno
import json
import os
import re
import secrets
import select
import signal
import sys
import time

from adapter_custody import CustodyError, GRACE_SECONDS

SANDBOX_FILTER_GLOBAL_NAME = 2
PROC_PIDTBSDINFO = 3
PROC_PIDUNIQIDENTIFIERINFO = 17
SZOMB = 5
CANCEL_SIGNALS = (signal.SIGTERM, signal.SIGINT)
REAP_GUARD_SECONDS = 30


class BsdInfo(ctypes.Structure):
    _fields_ = [("pbi_flags", ctypes.c_uint32), ("pbi_status", ctypes.c_uint32),
                ("pbi_xstatus", ctypes.c_uint32), ("pbi_pid", ctypes.c_uint32),
                ("pbi_ppid", ctypes.c_uint32), ("pbi_uid", ctypes.c_uint32),
                ("pbi_gid", ctypes.c_uint32), ("pbi_ruid", ctypes.c_uint32),
                ("pbi_rgid", ctypes.c_uint32), ("pbi_svuid", ctypes.c_uint32),
                ("pbi_svgid", ctypes.c_uint32), ("rfu_1", ctypes.c_uint32),
                ("pbi_comm", ctypes.c_char * 16), ("pbi_name", ctypes.c_char * 32),
                ("pbi_nfiles", ctypes.c_uint32), ("pbi_pgid", ctypes.c_uint32),
                ("pbi_pjobc", ctypes.c_uint32), ("e_tdev", ctypes.c_uint32),
                ("e_tpgid", ctypes.c_uint32), ("pbi_nice", ctypes.c_int32),
                ("pbi_start_tvsec", ctypes.c_uint64), ("pbi_start_tvusec", ctypes.c_uint64)]


class UniqInfo(ctypes.Structure):
    _fields_ = [("p_uuid", ctypes.c_uint8 * 16), ("p_uniqueid", ctypes.c_uint64),
                ("p_puniqueid", ctypes.c_uint64), ("p_idversion", ctypes.c_int32),
                ("p_orig_ppidversion", ctypes.c_int32), ("p_reserve2", ctypes.c_uint64),
                ("p_reserve3", ctypes.c_uint64)]


class AuditToken(ctypes.Structure):
    _fields_ = [("val", ctypes.c_uint32 * 8)]


assert ctypes.sizeof(BsdInfo) == 136 and ctypes.sizeof(UniqInfo) == 56


class Kernel:
    """libSystem entry points. Refuses on any other platform or missing symbol."""

    def __init__(self):
        if sys.platform != 'darwin':
            raise CustodyError('darwin seatbelt custody is macOS-only; target not launched')
        libc = ctypes.CDLL(None, use_errno=True)
        try:
            self.sandbox_init = libc.sandbox_init
            self.sandbox_free_error = libc.sandbox_free_error
            self.sandbox_check = libc.sandbox_check
            self.proc_pidinfo = libc.proc_pidinfo
            self.proc_listallpids = libc.proc_listallpids
            self.signal_token = libc.proc_signal_with_audittoken
        except AttributeError as exc:
            raise CustodyError('required libSystem custody symbol missing: ' + str(exc)) from None
        self.sandbox_init.argtypes = [ctypes.c_char_p, ctypes.c_uint64, ctypes.POINTER(ctypes.c_char_p)]
        self.sandbox_free_error.argtypes = [ctypes.c_char_p]
        # Variadic: only fixed parameters are declared, so ctypes passes the
        # name with the variadic calling convention (stack on Apple arm64).
        self.sandbox_check.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
        self.proc_pidinfo.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_int]
        self.proc_listallpids.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.signal_token.argtypes = [ctypes.POINTER(AuditToken), ctypes.c_int]

    def _uniq(self, pid, zombies):
        info = UniqInfo()
        # A nonzero arg also finds zombies (XNU proc_pidinfo findzomb).
        if self.proc_pidinfo(pid, PROC_PIDUNIQIDENTIFIERINFO, 1 if zombies else 0, ctypes.byref(info),
                             ctypes.sizeof(info)) != ctypes.sizeof(info):
            return None
        return info.p_idversion, info.p_uniqueid

    def version(self, pid):
        """(pidversion, uniqueid) of a live process, else None."""
        return self._uniq(pid, False)

    def zombie(self, pid):
        """(pidversion, uniqueid) of an unreaped dead process, else None.

        Zombie-inclusive lookup first, then a live-only lookup: if the PID is
        not live afterwards, the identity read first was already dead. The
        reverse order could return a new live process that reused the PID."""
        identity = self._uniq(pid, True)
        if identity is None or self._uniq(pid, False) is not None:
            return None
        return identity

    def pids(self):
        count = self.proc_listallpids(None, 0)
        if count <= 0:
            raise CustodyError('proc_listallpids failed: ' + os.strerror(ctypes.get_errno()))
        while True:
            capacity = count + 256
            buffer = (ctypes.c_int * capacity)()
            count = self.proc_listallpids(buffer, ctypes.sizeof(buffer))
            if count <= 0:
                raise CustodyError('proc_listallpids failed: ' + os.strerror(ctypes.get_errno()))
            if count < capacity:
                return [pid for pid in buffer[:count] if pid > 0]

    def check(self, pid, name):
        return self.sandbox_check(pid, b'mach-lookup', SANDBOX_FILTER_GLOBAL_NAME, ctypes.c_char_p(name))

    def signal(self, pid, version, sig):
        """0 when delivered to exactly (pid, version); otherwise an errno."""
        token = AuditToken()
        token.val[5] = pid
        token.val[7] = version & 0xFFFFFFFF
        return self.signal_token(ctypes.byref(token), sig)


TAG_NAME = re.compile(r'org\.nostrc\.custody\.[0-9a-f]{32}\.(a|b)\Z')


class Tag:
    def __init__(self, denied=None, allowed=None):
        self.denied = denied or ('org.nostrc.custody.' + secrets.token_hex(16) + '.a')
        self.allowed = allowed or ('org.nostrc.custody.' + secrets.token_hex(16) + '.b')
        # Equal names would make every scan empty: refuse, never "succeed".
        for name, suffix in ((self.denied, 'a'), (self.allowed, 'b')):
            match = TAG_NAME.fullmatch(name) if isinstance(name, str) else None
            if not match or match.group(1) != suffix:
                raise CustodyError('invalid custody tag name')
        if self.denied[:-2] == self.allowed[:-2]:
            raise CustodyError('custody tag names must be independent')

    def profile(self):
        return f'(version 1)(allow default)(deny mach-lookup (global-name "{self.denied}"))'.encode()

    def to_json(self):
        return json.dumps({'denied': self.denied, 'allowed': self.allowed})

    @classmethod
    def from_json(cls, text):
        data = json.loads(text)
        return cls(data['denied'], data['allowed'])


def apply_tag(kernel, tag):
    """Runs in the forked root child, before the target is executed."""
    error = ctypes.c_char_p()
    if kernel.sandbox_init(tag.profile(), 0, ctypes.byref(error)) != 0:
        message = error.value.decode(errors='replace') if error.value else os.strerror(ctypes.get_errno())
        raise CustodyError('sandbox_init custody tag refused: ' + message)


def classify(kernel, tag, pid, observe=None):
    """(state, identity): member | other (live, stable), zombie, gone,
    vanished (died while classified), unstable (exec/reuse while classified),
    error (live, unclassifiable). identity is (pidversion, uniqueid)."""
    before = kernel.version(pid)
    if before is None:
        dead = kernel.zombie(pid)
        return ('zombie', dead) if dead is not None else ('gone', None)
    if observe:
        observe('classify', pid=pid)
    denied = kernel.check(pid, tag.denied.encode())
    allowed = kernel.check(pid, tag.allowed.encode())
    after = kernel.version(pid)
    if after is None:
        return 'vanished', before
    if after != before:
        return 'unstable', before
    if denied == 1 and allowed == 0:
        return 'member', before
    if denied in (0, 1) and allowed in (0, 1):
        return 'other', before
    return 'error', before


def scan_round(kernel, tag, exclude=(), observe=None):
    """One two-snapshot barrier round: ({pid: pidversion} members, certain)."""
    stable, dead_before = {}, set()
    snapshot = kernel.pids()
    if observe:
        observe('snapshot0', pids=snapshot)
    for pid in snapshot:
        if pid in exclude:
            continue
        state, identity = classify(kernel, tag, pid, observe)
        if observe:
            observe('classified', pid=pid, state=state, pass_no=1)
        if state in ('member', 'other'):
            stable[(pid, identity[1])] = (state, identity[0])
        elif state == 'zombie':
            dead_before.add((pid, identity[1]))
        elif state == 'error':
            raise CustodyError(f'cannot classify live process {pid} for custody')
    snapshot = kernel.pids()
    if observe:
        observe('snapshot1', pids=snapshot)
    certain = True
    for pid in snapshot:
        if pid in exclude:
            continue
        live = kernel.version(pid)
        if live is not None and (pid, live[1]) in stable and stable[(pid, live[1])][1] == live[0]:
            continue
        state, identity = classify(kernel, tag, pid, observe)
        if observe:
            observe('classified', pid=pid, state=state, pass_no=2)
        if state in ('member', 'other'):
            stable[(pid, identity[1])] = (state, identity[0])
        elif state == 'zombie' and (pid, identity[1]) in dead_before:
            continue  # already dead during pass 1, before S1
        elif state == 'error':
            raise CustodyError(f'cannot classify live process {pid} for custody')
        else:
            # gone/zombie/vanished/unstable after S1: it may have been an
            # unclassified live member at S1 that forked and exited since.
            certain = False
    members = {pid: version for (pid, _), (state, version) in stable.items() if state == 'member'}
    return members, certain


def scan(kernel, tag, deadline, exclude=(), observe=None, evidence=None):
    """Members from the first certain round; uncertain rounds are rescanned."""
    while True:
        members, certain = scan_round(kernel, tag, exclude, observe)
        if certain or members:
            return members
        if evidence is not None:
            evidence['uncertain_rounds'] = evidence.get('uncertain_rounds', 0) + 1
        if time.monotonic() >= deadline:
            raise CustodyError('no certain custody scan before the guard; cleanup unverified')


def kill_pass(kernel, tag, deadline, exclude=(), evidence=None, observe=None):
    """SIGKILL every member found once; returns {pid: version} delivered."""
    signalled = {}
    for pid, version in scan(kernel, tag, deadline, exclude, observe, evidence).items():
        result = kernel.signal(pid, version, signal.SIGKILL)
        if evidence is not None:
            evidence.setdefault('killed', []).append({'pid': pid, 'pidversion': version, 'errno': result})
        if result == errno.ESRCH:
            continue  # exited or exec'd since classification: the next round decides
        if result != 0:
            raise CustodyError(f'cannot signal custody member pid {pid}: {os.strerror(result)}')
        signalled[pid] = version
    return signalled


def await_exits(kernel, members, deadline):
    """Event-driven wait for NOTE_EXIT of exactly these (pid, version) members."""
    queue = select.kqueue()
    try:
        waiting = set()
        for pid, version in members.items():
            # Register without receiving events: an event returned here could
            # be an earlier member's one-shot NOTE_EXIT and would be lost.
            try:
                queue.control([select.kevent(pid, filter=select.KQ_FILTER_PROC,
                               flags=select.KQ_EV_ADD | select.KQ_EV_ONESHOT,
                               fflags=select.KQ_NOTE_EXIT)], 0, 0)
            except ProcessLookupError:
                continue  # already exited
            current = kernel.version(pid)
            if current is not None and current[0] == version:
                waiting.add(pid)  # registered while the same identity was live
        while waiting:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise CustodyError(f'custody members did not exit before the guard: {sorted(waiting)}')
            for event in queue.control(None, 64, remaining):
                waiting.discard(event.ident)
    finally:
        queue.close()


def reap(kernel, tag, deadline, evidence, observe=None):
    """Kill rounds until a certain round finds no member. Raises on failure."""
    rounds = 0
    while True:
        members = scan(kernel, tag, deadline, observe=observe, evidence=evidence)
        if not members:
            evidence['reap_rounds'] = rounds
            return
        rounds += 1
        signalled = {}
        for pid, version in members.items():
            result = kernel.signal(pid, version, signal.SIGKILL)
            evidence.setdefault('killed', []).append({'pid': pid, 'pidversion': version, 'errno': result})
            if result == errno.ESRCH:
                continue
            if result != 0:
                raise CustodyError(f'cannot signal custody member pid {pid}: {os.strerror(result)}')
            signalled[pid] = version
        await_exits(kernel, signalled, deadline)
        if time.monotonic() >= deadline:
            raise CustodyError('custody members kept appearing until the guard; cleanup unverified')


CONTROL_FD = None
CONTROL_KEYS = ('backend', 'tag_authority', 'launched', 'cancelled', 'adapter_parent_exit', 'root_pid',
                'exit_code', 'reap_rounds', 'uncertain_rounds', 'cleanup_complete', 'error',
                'evidence_file_error')


def control_send(message):
    """Bounded JSON line on the adapter's private control pipe, if any."""
    if CONTROL_FD is None:
        return
    data = (json.dumps(message, sort_keys=True) + '\n').encode()
    while data:
        data = data[os.write(CONTROL_FD, data):]


def finish(evidence):
    """Write the informational file, then the authoritative final line.

    A pre-existing or unwritable evidence file is a failure (it may be a
    forgery planted by the target), reported on the control pipe."""
    path = os.environ.get('NOSTRC_DARWIN_CUSTODY_EVIDENCE')
    if path:
        try:
            with open(path, 'x') as handle:
                json.dump(evidence, handle, sort_keys=True)
        except OSError as exc:
            evidence['evidence_file_error'] = str(exc)
            evidence['cleanup_complete'] = False
    final = {key: evidence[key] for key in CONTROL_KEYS if key in evidence}
    final['killed_count'] = len(evidence.get('killed', ()))
    control_send(dict(final, event='final'))
    if 'evidence_file_error' in evidence:
        raise CustodyError('custody evidence file refused: ' + evidence['evidence_file_error'])


def run_root(kernel, tag, argv, queue, adapter_parent, old_mask, evidence):
    """Launch the gated, tagged root and supervise it; returns its exit code."""
    go_read, go_write = os.pipe()
    ack_read, ack_write = os.pipe()
    root = os.fork()
    if root == 0:
        try:
            os.close(go_write)
            os.close(ack_read)
            os.setsid()
            apply_tag(kernel, tag)
            os.write(ack_write, b'T')
            os.close(ack_write)
            if os.read(go_read, 1) != b'1':
                os._exit(125)
            os.close(go_read)
            for sig in CANCEL_SIGNALS + (signal.SIGPIPE, signal.SIGCHLD):
                signal.signal(sig, signal.SIG_DFL)
            signal.pthread_sigmask(signal.SIG_SETMASK, old_mask - set(CANCEL_SIGNALS))
            # The control FD is close-on-exec; custody paths are not advertised.
            env = {k: v for k, v in os.environ.items() if not k.startswith('NOSTRC_DARWIN_CUSTODY_')}
            os.execvpe(argv[0], argv, env)
        except BaseException as exc:
            os.write(2, ('darwin custody root: ' + str(exc) + '\n').encode())
        os._exit(125)
    os.close(go_read)
    os.close(ack_write)
    # Only the root keeps the adapter's native seal FD (it inherited it at
    # fork); the supervisor drops its copy. Seal EOF is NOT a cleanup barrier
    # (the root may close it early): the barrier is this supervisor's exit
    # with evidence cleanup_complete=true after a certain empty reap.
    drop_seal()
    reaped = False
    try:
        if os.read(ack_read, 1) != b'T':
            raise CustodyError('custody tag was not applied; target not launched')
        queue.control([select.kevent(root, filter=select.KQ_FILTER_PROC,
                       flags=select.KQ_EV_ADD | select.KQ_EV_ONESHOT, fflags=select.KQ_NOTE_EXIT)], 0)
        guard = time.monotonic() + REAP_GUARD_SECONDS
        # Prove both sides of the classification on this host before exec.
        state, identity = classify(kernel, tag, root)
        if state != 'member':
            raise CustodyError(f'custody tag not observable on gated root ({state}); target not launched')
        if classify(kernel, tag, os.getpid())[0] != 'other':
            raise CustodyError('supervisor classified as a custody member; target not launched')
        evidence.update(root_pid=root, root_pregate_pidversion=identity[0])
        # Authoritative binding, written before the target can run.
        control_send({'event': 'tag', 'denied': tag.denied, 'allowed': tag.allowed,
                      'root_pid': root, 'root_pregate_pidversion': identity[0]})
        if set(signal.sigpending()) & set(CANCEL_SIGNALS):
            evidence['cancelled'] = True
            return 130  # the root is still gated; finally kills and reaps it
        os.write(go_write, b'1')
        evidence['launched'] = True
        cancelled, deadline = False, None
        while True:
            timeout = None if deadline is None else max(0.0, deadline - time.monotonic())
            for event in queue.control(None, 8, timeout):
                if event.filter == select.KQ_FILTER_PROC and event.ident == root:
                    _, status = os.waitpid(root, 0)
                    reaped = True
                    raw = os.waitstatus_to_exitcode(status)
                    evidence['root_status'] = raw
                    return 130 if cancelled else (128 - raw if raw < 0 else raw)
                if cancelled:
                    continue
                cancelled = evidence['cancelled'] = True
                if event.filter == select.KQ_FILTER_PROC and event.ident == adapter_parent:
                    evidence['adapter_parent_exit'] = True
                    deadline = time.monotonic()
                else:
                    # Like the Linux backend: TERM the root so it can finalize
                    # its output, kill every other member now.
                    deadline = time.monotonic() + GRACE_SECONDS
                    os.kill(root, signal.SIGTERM)  # direct, unreaped child: PID reserved
                    guard = time.monotonic() + REAP_GUARD_SECONDS
                    kill_pass(kernel, tag, guard, exclude={root}, evidence=evidence)
            if deadline is not None and time.monotonic() >= deadline:
                os.kill(root, signal.SIGKILL)  # still unreaped: PID reserved
                evidence['root_grace_expired'] = True
                deadline = None
    finally:
        for fd in (go_write, ack_read):
            os.close(fd)
        if not reaped:
            os.kill(root, signal.SIGKILL)  # direct, unreaped child: PID reserved
            os.waitpid(root, 0)


def drop_seal():
    seal = os.environ.pop('NOSTRC_NATIVE_SEAL_FD', None)
    if seal is not None:
        os.close(int(seal))  # popped first: a second call is a no-op


def supervise(argv, adapter_parent=None, gate=None):
    kernel = Kernel()
    if not argv:
        raise CustodyError('missing command')
    old_mask = signal.pthread_sigmask(signal.SIG_BLOCK, set(CANCEL_SIGNALS))
    queue = select.kqueue()
    tag = Tag()
    evidence = {'backend': 'darwin-seatbelt-tag', 'tag_authority': 'supervisor', 'complete': False,
                'untraced_profile_parity': False, 'cleanup_complete': False, 'cancelled': False,
                'adapter_parent_exit': False, 'launched': False}
    try:
        for sig in CANCEL_SIGNALS:
            queue.control([select.kevent(sig, filter=select.KQ_FILTER_SIGNAL, flags=select.KQ_EV_ADD)], 0)
        if adapter_parent is not None:
            queue.control([select.kevent(adapter_parent, filter=select.KQ_FILTER_PROC,
                           flags=select.KQ_EV_ADD | select.KQ_EV_ONESHOT, fflags=select.KQ_NOTE_EXIT)], 0)
            # Registered first, then checked: a live parent cannot have been reused.
            if os.getppid() != adapter_parent:
                raise CustodyError('adapter parent exited; target not launched')
        if gate is not None:
            try:
                if os.read(gate, 1) != b'1':
                    raise CustodyError('adapter reservation missing; target not launched')
            finally:
                os.close(gate)
        if set(signal.sigpending()) & set(CANCEL_SIGNALS):
            evidence.update(cancelled=True, exit_code=130)
            return 130
        tag_file = os.environ.get('NOSTRC_DARWIN_CUSTODY_TAG_FILE')
        if tag_file:
            # Owner-only, exclusive. Not a secret from the same UID (see contract).
            with os.fdopen(os.open(tag_file, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), 'w') as handle:
                handle.write(tag.to_json())
        try:
            code = run_root(kernel, tag, argv, queue, adapter_parent, old_mask, evidence)
        finally:
            # Root is reaped; every remaining member is an escaped descendant.
            reap(kernel, tag, time.monotonic() + REAP_GUARD_SECONDS, evidence)
            evidence['cleanup_complete'] = True
        evidence['exit_code'] = code
        return code
    except BaseException as exc:
        evidence['error'] = str(exc)
        raise
    finally:
        drop_seal()  # also on refusal before any root was forked
        queue.close()
        finish(evidence)


def recover(read_tag, authority):
    """Reap a tag after supervisor death.

    authority 'parent-bound': names the adapter read from the control pipe
    before the target ran. 'untrusted-file': a same-UID writable file the
    target may have replaced; an empty result proves nothing about the
    original tree and must not turn a failed lane into a clean one."""
    evidence = {'backend': 'darwin-seatbelt-tag', 'recovery': True, 'tag_authority': authority,
                'cleanup_complete': False}
    try:
        tag = read_tag()
        reap(Kernel(), tag, time.monotonic() + REAP_GUARD_SECONDS, evidence)
        evidence['cleanup_complete'] = True
    except BaseException as exc:
        evidence['error'] = str(exc)
        raise
    finally:
        finish(evidence)
    return 0


def main():
    global CONTROL_FD
    args = sys.argv[1:]
    gate = parent = None
    while args[:1] in (['--control-fd'], ['--parent-ready-fd']):
        if args[0] == '--control-fd':
            if len(args) < 2:
                raise CustodyError('missing control FD')
            CONTROL_FD = int(args[1])
            os.fstat(CONTROL_FD)  # invalid FD refuses before anything runs
            os.set_inheritable(CONTROL_FD, False)
            args = args[2:]
        else:
            if len(args) < 4 or args[2] != '--parent-pid':
                raise CustodyError('missing adapter parent identity')
            gate, parent = int(args[1]), int(args[3])
            args = args[4:]
    if args[:1] == ['--reap-tag']:
        if len(args) != 3:
            raise CustodyError('usage: [--control-fd N] --reap-tag DENIED ALLOWED')
        return recover(lambda: Tag(args[1], args[2]), 'parent-bound')
    if args[:1] == ['--reap-tag-file']:
        if len(args) != 2:
            raise CustodyError('usage: [--control-fd N] --reap-tag-file PATH')

        def from_file():
            with open(args[1]) as handle:
                return Tag.from_json(handle.read())
        return recover(from_file, 'untrusted-file')
    if args[:1] == ['--private-resource-root']:
        raise CustodyError('darwin custody does not host private display/bus resources; target not launched')
    return supervise(args, parent, gate)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (CustodyError, OSError) as exc:
        print('darwin custody: ' + str(exc), file=sys.stderr, flush=True)
        sys.exit(125)
