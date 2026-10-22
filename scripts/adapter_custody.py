#!/usr/bin/env python3
"""Linux-only, launch-time ptrace custody for the diagnostic outer adapter.

PTRACE_SEIZE + TRACE{FORK,VFORK,CLONE} stops new tasks before they run;
EXITKILL kills tracees if this supervisor dies. A stopped task reserves its
identity while /proc supplies its thread-group ID and pidfd_open binds that
process. All asynchronous signals use pidfds, never cached PIDs or PGIDs.
See https://man7.org/linux/man-pages/man2/ptrace.2.html and pidfd_open(2).

Explicit opt-in only: tracing is not untraced profile parity; instrumented
lanes refuse in the adapter. This is not a sandbox: programs cannot gain set-ID
privileges, use a second debugger, or attest remote work. clone3 is unavailable.
macOS has no implementation here. Run only in a dedicated supervisor process.
"""
import ctypes
import errno
import os
from pathlib import Path
import signal
import platform
import sys
import time

SEIZE, INTERRUPT, CONT, GETEVENTMSG = 0x4206, 0x4207, 7, 0x4201
OPTIONS = 2 | 4 | 8 | 16 | 0x100000  # FORK, VFORK, CLONE, EXEC, EXITKILL
WALL = 0x40000000
GRACE_SECONDS = 15


class CustodyError(RuntimeError):
    pass


def restrict_untraced_clone(libc):
    """Inherited, unprivileged filter closes CLONE_UNTRACED's tracing escape.

    clone3's flags are behind a pointer, so refuse it with ENOSYS (libc can
    fall back to clone). Unknown/compat ABIs refuse, never bypass the filter.
    seccomp(2): filters survive fork/exec and cannot be removed by descendants.
    This is custody policy only, not a general syscall/security sandbox.
    """
    arch, clone_nr = {"x86_64": (0xC000003E, 56), "aarch64": (0xC00000B7, 220)}[platform.machine()]

    class Filter(ctypes.Structure):
        _fields_ = [("code", ctypes.c_ushort), ("jt", ctypes.c_ubyte),
                    ("jf", ctypes.c_ubyte), ("k", ctypes.c_uint)]

    class Program(ctypes.Structure):
        _fields_ = [("length", ctypes.c_ushort), ("filters", ctypes.POINTER(Filter))]

    instructions = [
        (0x20, 0, 0, 4),             # LD seccomp_data.arch
        (0x15, 1, 0, arch),          # JEQ native audit architecture
        (0x06, 0, 0, 0x80000000),    # RET KILL_PROCESS: wrong ABI
        (0x20, 0, 0, 0),             # LD seccomp_data.nr
        (0x45, 0, 1, 0x40000000),    # JSET x32 syscall bit
        (0x06, 0, 0, 0x50000 | errno.ENOSYS),
        (0x15, 0, 1, 435),           # JEQ clone3 (both admitted ABIs)
        (0x06, 0, 0, 0x50000 | errno.ENOSYS),
        (0x15, 0, 3, clone_nr),      # JEQ clone
        (0x20, 0, 0, 16),            # LD low word of args[0] (flags)
        (0x45, 0, 1, 0x800000),      # JSET CLONE_UNTRACED
        (0x06, 0, 0, 0x50000 | errno.EPERM),
        (0x06, 0, 0, 0x7FFF0000),    # RET ALLOW
    ]
    filters = (Filter * len(instructions))(*(Filter(*row) for row in instructions))
    program = Program(len(instructions), filters)
    libc.prctl.argtypes = (ctypes.c_int, ctypes.c_ulong, ctypes.c_ulong,
                           ctypes.c_ulong, ctypes.c_ulong)
    for option, value in ((38, 1), (22, ctypes.addressof(program))):
        # PR_SET_NO_NEW_PRIVS; PR_SET_SECCOMP(SECCOMP_MODE_FILTER, program)
        result = libc.prctl(option, value if option == 38 else 2,
                            0 if option == 38 else value, 0, 0)
        if result != 0:
            error = ctypes.get_errno()
            raise OSError(error, "custody seccomp unavailable: " + os.strerror(error))


def supervise(argv):
    if sys.platform != "linux" or not all((hasattr(os, "pidfd_open"),
                                            hasattr(signal, "pidfd_send_signal"))):
        raise CustodyError("Linux ptrace/pidfd custody unavailable; target not launched")
    if platform.machine() not in ("x86_64", "aarch64") or sys.byteorder != "little":
        raise CustodyError("unsupported Linux custody syscall ABI; target not launched")
    # Tgid binding requires the supervisor's real, accessible proc view. Refuse
    # an unavailable/mismatched view before forking or executing any target.
    try:
        own_status = Path('/proc/self/status').read_text()
        own_tgid = int(next(row.split()[1] for row in own_status.splitlines() if row.startswith('Tgid:')))
        if own_tgid != os.getpid():
            raise CustodyError("proc namespace disagrees with supervisor; target not launched")
    except OSError as exc:
        raise CustodyError("proc status unavailable; target not launched: " + str(exc)) from exc
    libc = ctypes.CDLL(None, use_errno=True)
    libc.ptrace.restype = ctypes.c_long
    libc.ptrace.argtypes = (ctypes.c_uint, ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p)

    def trace(request, tid, data=0):
        result = libc.ptrace(request, tid, None, ctypes.c_void_p(data))
        if result == -1:
            error = ctypes.get_errno()
            raise OSError(error, os.strerror(error))
        return result

    signals = {signal.SIGCHLD, signal.SIGTERM, signal.SIGINT}
    old_mask = signal.pthread_sigmask(signal.SIG_BLOCK, signals)
    signal.signal(signal.SIGCHLD, signal.SIG_DFL)
    if signal.sigpending() & {signal.SIGTERM, signal.SIGINT}:
        # Cancellation queued during interpreter/parent-reservation startup.
        return 130
    read_fd, write_fd = os.pipe()
    root = os.fork()
    if root == 0:
        os.close(write_fd)
        try:
            # EOF on supervisor failure exits without executing the target.
            if os.read(read_fd, 1) != b"1":
                os._exit(125)
            os.close(read_fd)
            os.setsid()
            restrict_untraced_clone(libc)
            signal.signal(signal.SIGPIPE, signal.SIG_DFL)
            # The adapter deliberately blocks cancellation across supervisor
            # startup. Only the supervisor, not the target, inherits that mask.
            signal.pthread_sigmask(signal.SIG_SETMASK, old_mask - {signal.SIGTERM, signal.SIGINT})
            os.execvpe(argv[0], argv, os.environ)
        except BaseException as exc:
            os.write(2, ("adapter custody exec: " + str(exc) + "\n").encode())
            os._exit(125)
    os.close(read_fd)
    handles = {}
    groups = {}
    root_handle = None
    attached = False
    cancelled = False
    root_code = None
    deadline = None
    pending = {root}
    stopped = set()
    root_resumed = False

    def send(fd, sig):
        try:
            signal.pidfd_send_signal(fd, sig)
        except ProcessLookupError:
            pass

    def kill_remaining():
        for fd in handles.values():
            send(fd, signal.SIGKILL)

    def bind_group(tid):
        # A stopped task reserves this relationship until it resumes/exits.
        status_text = Path(f"/proc/{tid}/status").read_text()
        tgid = int(next(row.split()[1] for row in status_text.splitlines() if row.startswith("Tgid:")))
        handles[tid] = os.pidfd_open(tgid)
        groups[tid] = tgid

    def cancel():
        nonlocal cancelled, deadline
        if not cancelled:
            cancelled = True
            deadline = time.monotonic() + GRACE_SECONDS
            send(root_handle, signal.SIGTERM if root_resumed else signal.SIGKILL)
            for tid, fd in handles.items():
                if groups[tid] != root:
                    send(fd, signal.SIGKILL)

    def kill_and_reap():
        """Fatal-path barrier; all waits in this isolated tracer are owned."""
        nonlocal root_handle
        send(root_handle, signal.SIGKILL)
        kill_remaining()
        # Signals are already delivered through bound handles. Release their
        # descriptors so even an allocation failure can drain remaining tasks;
        # this is failed-attempt cleanup, never a retry authorizing execution.
        for fd in handles.values():
            os.close(fd)
        handles.clear()
        groups.clear()
        os.close(root_handle)
        root_handle = None
        def kill_stopped(tid):
            if tid not in handles:
                bind_group(tid)
            send(handles[tid], signal.SIGKILL)
            os.close(handles.pop(tid))
            groups.pop(tid, None)
            try:
                trace(CONT, tid)
            except ProcessLookupError:
                pass
            stopped.discard(tid)

        # The event whose binding failed was already consumed by waitpid. It
        # will not be reported again until resumed/killed: handle it explicitly.
        for tid in list(stopped):
            kill_stopped(tid)
        while True:
            try:
                tid, status = os.waitpid(-1, WALL)
            except ChildProcessError:
                pending.clear()
                return
            if os.WIFEXITED(status) or os.WIFSIGNALED(status):
                pending.discard(tid)
                fd = handles.pop(tid, None)
                groups.pop(tid, None)
                if fd is not None:
                    os.close(fd)
            elif os.WIFSTOPPED(status):
                kill_stopped(tid)

    try:
        # Direct unreaped child: PID cannot be reused before this acquisition.
        root_handle = os.pidfd_open(root)
        signal.pidfd_send_signal(root_handle, 0)
        # Unsupported kernel options / ptrace policy refuse while target is gated.
        trace(SEIZE, root, OPTIONS)
        attached = True
        trace(INTERRUPT, root)
        os.write(write_fd, b"1")
        os.close(write_fd)
        write_fd = -1
        while pending:
            # Bounded event batch prevents a busy fork tree starving TERM/INT.
            for _ in range(256):
                try:
                    tid, status = os.waitpid(-1, os.WNOHANG | WALL)
                except ChildProcessError:
                    if pending:
                        raise CustodyError("lost traced task ownership")
                    break
                if not tid:
                    break
                if os.WIFEXITED(status) or os.WIFSIGNALED(status):
                    pending.discard(tid)
                    stopped.discard(tid)
                    groups.pop(tid, None)
                    fd = handles.pop(tid, None)
                    if fd is not None:
                        os.close(fd)
                    if tid == root:
                        root_code = os.waitstatus_to_exitcode(status)
                        # Normal exit must not leave escaped background children.
                        kill_remaining()
                    continue
                if not os.WIFSTOPPED(status):
                    raise CustodyError("unexpected traced task state")
                pending.add(tid)
                stopped.add(tid)
                event = status >> 16
                if event in (1, 2, 3, 4):
                    message = ctypes.c_ulong()
                    trace(GETEVENTMSG, tid, ctypes.addressof(message))
                    if event == 4:  # Nonleader exec changes TID to its leader.
                        former = message.value
                        if former != tid:
                            pending.discard(former)
                            stopped.discard(former)
                            groups.pop(former, None)
                            fd = handles.pop(former, None)
                            if fd is not None:
                                os.close(fd)
                    else:
                        pending.add(message.value)
                if tid not in handles:
                    bind_group(tid)
                # Consume cancellation before resuming any stopped task,
                # including the first root resume after launch reservation.
                if signal.sigtimedwait({signal.SIGTERM, signal.SIGINT}, 0):
                    cancel()
                if (cancelled and (groups[tid] != root or not root_resumed)) or root_code is not None:
                    send(handles[tid], signal.SIGKILL)
                # SEIZE event stops / automatic child stops aren't signals to
                # reinject. Failed proc binding above is always fatal; only
                # CONT's ESRCH can be normal concurrently observed task death.
                sig = 0 if event else os.WSTOPSIG(status)
                try:
                    trace(CONT, tid, sig)
                    stopped.discard(tid)
                    if tid == root:
                        root_resumed = True
                except ProcessLookupError:
                    pass
            if not pending:
                break
            timeout = max(0, deadline - time.monotonic()) if deadline else None
            if tid:
                timeout = 0  # More wait events may already be queued.
            notification = signal.sigtimedwait(signals, timeout) if timeout is not None else signal.sigwaitinfo(signals)
            if notification and notification.si_signo in (signal.SIGINT, signal.SIGTERM):
                cancel()
            if deadline and time.monotonic() >= deadline:
                send(root_handle, signal.SIGKILL)
                kill_remaining()
                deadline = None
        return 130 if cancelled else (root_code if root_code is not None else 125)
    finally:
        if write_fd != -1:
            os.close(write_fd)
        if pending:
            try:
                if attached:
                    kill_and_reap()
                else:
                    # Closing the never-authorized gate is sufficient even if
                    # pidfd allocation/signaling itself was the failed feature.
                    os.waitpid(root, 0)
                    pending.clear()
            except (OSError, ValueError, StopIteration) as exc:
                # EXITKILL still applies on tracer exit, but no exit barrier was
                # established. Preserve the original failure and report this
                # separately; never turn this path into a successful receipt.
                print(f"adapter custody cleanup_unverified: {exc}", file=sys.stderr, flush=True)
        for fd in handles.values():
            os.close(fd)
        if root_handle is not None:
            os.close(root_handle)
        # This dedicated supervisor is exiting. Pending/repeated cancellation
        # must not replace its final exit status after all tracees were reaped.
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        signal.pthread_sigmask(signal.SIG_SETMASK, old_mask)


if __name__ == "__main__":
    try:
        if sys.argv[1:2] == ["--parent-ready-fd"]:
            gate = int(sys.argv[2])
            del sys.argv[1:3]
            try:
                if sys.argv[1:2] == ["--parent-pid"]:
                    parent = int(sys.argv[2])
                    del sys.argv[1:3]
                    libc = ctypes.CDLL(None, use_errno=True)
                    # Arm before checking getppid: covers death both before and
                    # after prctl. If the adapter dies, EXITKILL (or gate EOF
                    # before SEIZE) prevents an orphaned executing target tree.
                    if libc.prctl(1, signal.SIGKILL, 0, 0, 0) != 0:
                        raise CustodyError("parent-death custody unavailable; target not launched")
                    if os.getppid() != parent:
                        raise CustodyError("adapter parent exited; target not launched")
                if os.read(gate, 1) != b"1":
                    raise CustodyError("parent pidfd reservation missing; target not launched")
            finally:
                os.close(gate)
        code = supervise(sys.argv[1:])
    except (CustodyError, OSError) as exc:
        print(f"adapter custody: {exc}", file=sys.stderr, flush=True)
        sys.exit(125)
    sys.exit(128 - code if code < 0 else code)
