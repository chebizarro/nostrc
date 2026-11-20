#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check the actual Linux daemon's security properties, not just build flags."""
import re
import subprocess
import sys


def check(binary):
    elf = subprocess.check_output(
        ["readelf", "--wide", "--file-header", "--program-headers", "--dynamic",
         "--dyn-syms", binary], text=True)
    stack = next((line.split() for line in elf.splitlines() if "GNU_STACK" in line), [])
    checks = {
        "PIE": re.search(r"Type:\s+DYN\b", elf),
        "RELRO": "GNU_RELRO" in elf,
        "immediate binding": re.search(r"\(BIND_NOW\)|\(FLAGS\).*BIND_NOW|\(FLAGS_1\).*NOW", elf),
        "non-executable stack": len(stack) >= 2 and stack[-2] == "RW",
        "stack protector": "__stack_chk_fail" in elf,
    }
    missing = [name for name, present in checks.items() if not present]
    if missing:
        sys.exit(f"{binary}: missing Linux hardening: {', '.join(missing)}")
    print(f"{binary}: Linux hardening PASS ({', '.join(checks)})")


if __name__ == "__main__":
    check(sys.argv[1])
