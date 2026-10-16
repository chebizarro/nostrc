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
    if mode == 'hold':
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        while True:
            signal.pause()
    sys.exit(19 if mode == 'fail' else 0)


def workload(mode):
    source = Path('/work/mini')
    source.mkdir()
    (source / 'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.22)\nproject(Custody NONE)\nenable_testing()\n'
        'add_custom_target(real-build ALL COMMAND ${CMAKE_COMMAND} -E touch /work/built)\n'
        'add_test(NAME escaped COMMAND ' + sys.executable + ' ' + HERE + ' consumer ' + mode + ')\n')
    subprocess.run(['cmake', '-S', str(source), '-B', '/work/build', '-G', 'Ninja'], check=True)
    subprocess.run(['cmake', '--build', '/work/build'], check=True)
    sys.exit(subprocess.call(['ctest', '--test-dir', '/work/build', '-V', '--output-junit', '/work/results.xml']))


if __name__ == '__main__':
    mode = sys.argv[1]
    if mode == 'actor':
        actor(int(sys.argv[2]))
    elif mode == 'consumer':
        consumer(sys.argv[2])
    elif mode == 'command':
        consumer('success')
    elif mode == 'wrapped':
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        sys.exit(subprocess.call(['dbus-run-session', '--', 'xvfb-run', '-a', sys.executable, HERE, sys.argv[2]]))
    else:
        workload(mode)
