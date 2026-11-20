#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Real startup boundary under zero memlock; no privileges or syscall mocks."""
import os
import pathlib
import resource
import subprocess
import sys
import tempfile


def zero_memlock():
    resource.setrlimit(resource.RLIMIT_MEMLOCK, (0, 0))


def main():
    daemon = os.path.abspath(sys.argv[1])
    env = {k: v for k, v in os.environ.items() if not k.startswith("SIGNET_")}
    with tempfile.TemporaryDirectory(prefix="signet-memory-") as tmp:
        # A configuration failure is a deterministic downstream boundary: required
        # and invalid modes must refuse BEFORE config/secret loading, while off
        # and best-effort must reach it. No listener or external service is needed.
        missing = str(pathlib.Path(tmp) / "absent.conf")
        for mode in (None, "best-effort", "off", "required", "bogus"):
            child_env = env.copy()
            if mode is not None:
                child_env["SIGNET_MLOCK_MODE"] = mode
            p = subprocess.run([daemon, "-c", missing], env=child_env,
                               preexec_fn=zero_memlock, capture_output=True,
                               text=True, timeout=15)
            output = p.stdout + p.stderr
            assert p.returncode == 1, (mode, p.returncode, output)
            if mode == "required":
                assert "required memory locking refused" in output, output
                assert "RLIMIT_MEMLOCK is finite" in output, output
                assert "failed to load config" not in output, output
            elif mode == "bogus":
                assert "invalid SIGNET_MLOCK_MODE" in output, output
                assert "failed to load config" not in output, output
            else:
                assert "failed to load config" in output, output
                expected = "disabled (off)" if mode == "off" else "best-effort memory locking degraded"
                assert expected in output, output
                if mode != "off":
                    assert "MCL_FUTURE not enabled" in output, output
            print(f"mode={mode or 'default'} zero-memlock: exit={p.returncode} PASS")


if __name__ == "__main__":
    main()
