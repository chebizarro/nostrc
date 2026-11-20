#!/usr/bin/env python3
"""Send one control-socket request to fipsd and print the reply.

    control-query.py [--socket PATH] [--retry SECONDS] COMMAND

One JSON request line in, one response line out (fipsd/README.md, "Unix
control socket"). Exit 0 when the reply has "status": "ok", 1 when the daemon
answered an error, 2 when no reply could be had. --retry keeps trying for
that many seconds while the socket is absent or the node answers "node not
yet initialized", which it does for a moment after start-up; the install
smokes use it right after `systemctl start`.

Where fipsctl is installed the smokes prefer it; this is the fallback and
the shape of the query that fipsctl makes.
"""
import argparse
import json
import socket
import sys
import time


def query(path, command):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as conn:
        conn.settimeout(10)
        conn.connect(path)
        conn.sendall(json.dumps({"command": command}).encode() + b"\n")
        data = b""
        while not data.endswith(b"\n"):
            chunk = conn.recv(65536)
            if not chunk:
                break
            data += chunk
    return json.loads(data.decode())


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--socket", default="/run/fips/control.sock")
    parser.add_argument("--retry", type=float, default=0.0, metavar="SECONDS")
    parser.add_argument("command")
    args = parser.parse_args()

    deadline = time.monotonic() + args.retry
    last = None
    while True:
        try:
            reply = query(args.socket, args.command)
        except (OSError, ValueError) as exc:
            last = f"{args.socket}: {exc}"
            reply = None
        if reply is not None:
            if reply.get("status") == "ok":
                print(json.dumps(reply, indent=2, sort_keys=True))
                return 0
            last = reply.get("message", json.dumps(reply))
            if "not yet initialized" not in str(last):
                print(json.dumps(reply, indent=2, sort_keys=True), file=sys.stderr)
                return 1
        if time.monotonic() >= deadline:
            print(f"control-query: no ok reply for {args.command}: {last}", file=sys.stderr)
            return 1 if reply is not None else 2
        time.sleep(0.5)


if __name__ == "__main__":
    sys.exit(main())
