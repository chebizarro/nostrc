#!/usr/bin/env python3
"""Install a configured build, then compile/run consumers with installed metadata."""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

PACKAGES = ['nip11', 'nip17', 'nip46', 'nip51', 'nip98']


def run(args, **kwargs):
    print('+', shlex.join(map(str, args)), flush=True)
    return subprocess.run(args, check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', required=True, type=Path)
    parser.add_argument('--shared', action='store_true',
                        help='also test shared NIP-11/98 without --static')
    parser.add_argument('--sanitize', action='store_true',
                        help='link the consumer with CI ASan/UBSan runtimes')
    args = parser.parse_args()
    build = args.build.resolve()
    cache = {}
    for line in (build / 'CMakeCache.txt').read_text().splitlines():
        if '=' in line and not line.startswith(('#', '//')):
            key, value = line.split('=', 1)
            cache[key.split(':', 1)[0]] = value
    prefix = Path(cache['CMAKE_INSTALL_PREFIX'])
    libdir = Path(cache['CMAKE_INSTALL_LIBDIR'])
    if not libdir.is_absolute():
        libdir = prefix / libdir
    run(['cmake', '--install', build])
    env = os.environ.copy()
    # No source/build-tree search overrides can rescue incomplete installation.
    for key in ['CPATH', 'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH', 'LIBRARY_PATH',
                'LD_LIBRARY_PATH', 'DYLD_LIBRARY_PATH', 'DYLD_FALLBACK_LIBRARY_PATH']:
        env.pop(key, None)
    env['PKG_CONFIG_PATH'] = str(libdir / 'pkgconfig') + os.pathsep + env.get('PKG_CONFIG_PATH', '')

    def pc(*flags):
        return subprocess.check_output(['pkg-config', *flags], env=env, text=True).strip()

    for package in PACKAGES + ['nip04', 'nip44', 'nostr', 'libnostr-json', 'libnostrgo']:
        version = pc('--modversion', package)
        assert version, f'{package} has no version'
        assert Path(pc('--variable=prefix', package)) == prefix, package
        print(f'{package}: {version}', flush=True)
    # Keep transient consumer files in this task's build tree, not a sibling.
    with tempfile.TemporaryDirectory(prefix='installed-consumer-', dir=build) as tmp:
        tmp = Path(tmp)
        source = tmp / 'consumer.c'
        shutil.copyfile(Path(__file__).with_name('nip-consumer.c'), source)
        modes = [('static', ['--static'], PACKAGES, [])]
        if args.shared:
            modes.append(('shared', [], ['nip11', 'nip98'], ['-DSHARED_NIPS_ONLY']))
        if (libdir / 'pkgconfig' / 'nip46-glib.pc').is_file():
            assert Path(pc('--variable=prefix', 'nip46-glib')) == prefix
            modes.append(('glib', ['--static'], ['nip46-glib', *PACKAGES], ['-DNIP46_GLIB']))
        for mode, static_flags, packages, defines in modes:
            flags = shlex.split(pc('--cflags', '--libs', *static_flags, *packages))
            # Only installed flags, never -I/-L into the build or source tree.
            for flag in flags:
                assert str(build) not in flag, flag
                root = Path(__file__).resolve().parents[2]
                for subtree in ['nips', 'libnostr', 'libgo', 'libjson']:
                    assert str(root / subtree) not in flag, flag
            exe = tmp / mode
            command = shlex.split(env.get('CC', 'cc')) + [str(source), '-o', str(exe), *defines, *flags]
            if args.sanitize:
                command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
            if os.name != 'nt':
                # Staged DSOs are outside the system loader paths. Derive RPATH
                # from the installed metadata, not from the source/build tree.
                command += ['-Wl,-rpath,' + pc('--variable=libdir', 'libnostr-json')]
            run(command, env=env)
            run([exe], env=env)
            print(f'PASS installed {mode} consumer', flush=True)
        # Match the repo's pkg-config-backed CMake component convention too.
        (tmp / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.22)
project(installed_nips C)
find_package(nostrc REQUIRED COMPONENTS nip11 nip17 nip46 nip51 nip98)
add_executable(consumer consumer.c)
target_link_libraries(consumer PRIVATE nostrc::nip11 nostrc::nip17 nostrc::nip46 nostrc::nip51 nostrc::nip98)
''')
        cmake_flags = ['-DCMAKE_PREFIX_PATH=' + str(prefix)]
        if args.sanitize:
            cmake_flags += ['-DCMAKE_C_FLAGS=-fsanitize=address,undefined',
                            '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined']
        run(['cmake', '-S', tmp, '-B', tmp / 'cmake', *cmake_flags], env=env)
        run(['cmake', '--build', tmp / 'cmake', '-j4'], env=env)
        run([tmp / 'cmake' / 'consumer'], env=env)
        print('PASS installed CMake components', flush=True)


if __name__ == '__main__':
    main()
