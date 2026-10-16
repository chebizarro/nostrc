"""Real CMake/Ninja/CTest escape adversary; only run inside an owned container."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys

HERE = str(Path(__file__).resolve())


def actor(fd):
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    os.kill(os.getpid(), signal.SIGTERM)
    Path('/work/escape.json').write_text(json.dumps({'pid': os.getpid(), 'pgid': os.getpgrp(), 'sid': os.getsid(0)}))
    os.write(fd, b'ready')
    os.close(fd)
    while True:
        signal.pause()


def consumer(mode):
    read, write = os.pipe()
    subprocess.Popen([sys.executable, HERE, 'actor', str(write)], pass_fds=(write,),
                     start_new_session=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    os.close(write)
    assert os.read(read, 5) == b'ready'
    os.close(read)
    os.write(1, b'owned ready \xff\n')
    Path('/work/actor.raw').write_bytes(b'owned ready \xff\n')
    if mode == 'hold':
        import socket
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        listener = socket.socket(socket.AF_UNIX)
        listener.bind('/work/pending.sock')
        listener.listen(1)
        os.rename('/work/pending.sock', '/work/ready.sock')
        connection, _ = listener.accept()
        connection.sendall(b'ready-after-raw')
        connection.close()
        while True:
            signal.pause()
    sys.exit(19 if mode == 'fail' else 0)


def observe():
    import ctypes
    import select
    import socket
    libc = ctypes.CDLL(None, use_errno=True)
    fd = libc.inotify_init1(0)
    assert fd >= 0 and libc.inotify_add_watch(fd, b'/work', 0x100 | 0x80) >= 0
    while not Path('/work/ready.sock').exists():
        assert select.select([fd], [], [], 30)[0], 'actual phase readiness timed out'
        os.read(fd, 65536)
    os.close(fd)
    with socket.socket(socket.AF_UNIX) as connection:
        connection.connect('/work/ready.sock')
        assert connection.recv(64) == b'ready-after-raw'


def workload(mode):
    source = Path('/work/mini')
    source.mkdir()
    consumer_command = sys.executable + ' ' + HERE + ' consumer '
    cmake = 'cmake_minimum_required(VERSION 3.22)\nproject(Custody NONE)\nenable_testing()\n'
    if mode == 'configure-hold':
        cmake += 'execute_process(COMMAND ' + consumer_command + 'hold)\n'
    build_command = consumer_command + 'hold' if mode == 'build-hold' else '${CMAKE_COMMAND} -E touch /work/built'
    cmake += 'add_custom_target(real-build ALL COMMAND ' + build_command + ')\n'
    cmake += 'add_test(NAME escaped COMMAND ' + consumer_command + (mode if mode in {'hold', 'fail'} else 'success') + ')\n'
    if mode == 'fixture-hold':
        cmake += ('add_test(NAME fixture COMMAND ' + consumer_command + 'hold)\n'
                  'set_tests_properties(fixture PROPERTIES FIXTURES_SETUP escaped-fixture)\n'
                  'set_tests_properties(escaped PROPERTIES FIXTURES_REQUIRED escaped-fixture)\n')
    (source / 'CMakeLists.txt').write_text(cmake)
    Path('/work/phase').write_text('configure')
    subprocess.run(['cmake', '-S', str(source), '-B', '/work/build', '-G', 'Ninja'], check=True)
    Path('/work/phase').write_text('build')
    subprocess.run(['cmake', '--build', '/work/build'], check=True)
    Path('/work/phase').write_text('fixture' if mode == 'fixture-hold' else 'consumer')
    sys.exit(subprocess.call(['ctest', '--test-dir', '/work/build', '-V', '--output-junit', '/work/results.xml']))


if __name__ == '__main__':
    mode = sys.argv[1]
    if mode == 'actor':
        actor(int(sys.argv[2]))
    elif mode == 'consumer':
        consumer(sys.argv[2])
    elif mode == 'observe':
        observe()
    elif mode == 'command':
        consumer('success')
    elif mode == 'wrapped':
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        sys.exit(subprocess.call(['dbus-run-session', '--', 'xvfb-run', '-a', sys.executable, HERE, sys.argv[2]]))
    else:
        workload(mode)
