#!/usr/bin/env python3
"""Two real backends; pipes carry SDP/trickle ICE, not media (DTLS-SRTP)."""
import os
import selectors
import subprocess
import sys
import time


def main():
    children = []
    selector = selectors.DefaultSelector()
    ready = set()
    try:
        for role in ("offer", "answer"):
            child = subprocess.Popen([sys.argv[1], "--peer-" + role],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=None, bufsize=0)
            children.append(child)
            selector.register(child.stdout, selectors.EVENT_READ, len(children) - 1)
        buffers = [b"", b""]
        deadline = time.monotonic() + 45
        while len(ready) != 2:
            events = selector.select(max(0, deadline - time.monotonic()))
            if not events:
                raise RuntimeError("two-process media deadline exceeded")
            for key, _ in events:
                i = key.data
                chunk = os.read(key.fd, 65536)
                if not chunk:
                    raise RuntimeError(f"peer {i} exited before both decoded media")
                buffers[i] += chunk
                while b"\n" in buffers[i]:
                    line, buffers[i] = buffers[i].split(b"\n", 1)
                    if line.startswith(b"MSG "):
                        children[1-i].stdin.write(line + b"\n")
                    elif line.startswith(b"READY "):
                        print(f"peer {i}: {line.decode()}", flush=True)
                        ready.add(i)
        for child in children:
            child.stdin.write(b"STOP\n")
        for child in children:
            code = child.wait(timeout=10)
            if code:
                raise RuntimeError(f"media peer exited with {code}")
    finally:
        selector.close()
        for child in children:
            if child.poll() is None:
                child.kill()
            child.wait()
            child.stdin.close()
            child.stdout.close()


if __name__ == "__main__":
    main()
