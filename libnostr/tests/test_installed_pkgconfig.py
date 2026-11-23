#!/usr/bin/env python3
"""Build/run an installed static consumer using only pkg-config flags.

Use a configured root build with BUILD_SHARED_LIBS=OFF. The default install
prefix differs from configure-time; --configured-prefix isolates link closure
from relocation when diagnosing old metadata. All scratch data stays in build.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def run(command, **kwargs):
    print("+", shlex.join(map(str, command)), flush=True)
    return subprocess.run(list(map(str, command)), check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--configured-prefix", action="store_true")
    parser.add_argument("--nip44", action="store_true", help="consume only the nostr-nip44 module")
    args = parser.parse_args()
    build = args.build_dir.resolve()
    cache = {}
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        if "=" in line and not line.startswith(("//", "#")):
            key, value = line.split("=", 1)
            cache[key.split(":", 1)[0]] = value
    if cache.get("BUILD_SHARED_LIBS") != "OFF":
        raise SystemExit("requires explicit BUILD_SHARED_LIBS=OFF")
    source = Path(cache["CMAKE_HOME_DIRECTORY"])
    targets = ["nostr", "nostr_nip44_core"] if args.nip44 else ["nostr"]
    run(["cmake", "--build", build, "--target", *targets, "--parallel", "4"])
    with tempfile.TemporaryDirectory(prefix="installed-pc-", dir=build) as scratch:
        scratch = Path(scratch)
        prefix = Path(cache["CMAKE_INSTALL_PREFIX"]) if args.configured_prefix else scratch / "relocated-prefix"
        if prefix.exists():
            raise SystemExit(f"refusing to overwrite install prefix {prefix}")
        try:
            components = ["libgo", "libnostr"]
            if args.nip44:
                components.append("nips/nip44")
            for component in components:
                run(["cmake", "--install", build / component, "--prefix", prefix])
            pcdir = prefix / cache["CMAKE_INSTALL_LIBDIR"] / "pkgconfig"
            env = dict(os.environ, PKG_CONFIG_PATH=str(pcdir) + os.pathsep + os.environ.get("PKG_CONFIG_PATH", ""))
            module = "nostr-nip44" if args.nip44 else "nostr"
            flags = subprocess.check_output(["pkg-config", "--cflags", "--libs", "--static", module], env=env, text=True)
            print(f"pkg-config {module}:", flags.strip(), flush=True)
            resolved = subprocess.check_output(["pkg-config", "--variable=libdir", module], env=env, text=True).strip()
            if Path(resolved).resolve() != pcdir.parent.resolve():
                raise SystemExit(f"{module}.pc resolved outside staged installation: {resolved}")
            assert (pcdir.parent / "libnostr.a").is_file()
            assert (pcdir.parent / "libnostrgo.a").is_file()
            if args.nip44:
                assert (pcdir.parent / "libnostr_nip44_core.a").is_file()
            executable = scratch / "consumer"
            consumer = ("nips/nip44/tests/installed_pkgconfig_consumer.c" if args.nip44
                        else "libnostr/tests/installed_pkgconfig_consumer.c")
            run([cache["CMAKE_C_COMPILER"], source / consumer,
                 "-o", executable, *shlex.split(flags)], env=env)
            store = scratch / "store"
            store.mkdir()
            run([executable, store, cache["LIBNOSTR_WITH_NOSTRDB"]], env=env)
            print(f"installed static {module} consumer: PASS", flush=True)
        finally:
            if args.configured_prefix:
                import shutil
                shutil.rmtree(prefix, ignore_errors=True)


if __name__ == "__main__":
    main()
