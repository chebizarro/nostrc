#!/usr/bin/env python3
"""
fake_relay_fixture.py — Minimal NIP-01 WebSocket relay for integration tests.

Serves pre-seeded events from a JSON file.  Supports REQ, CLOSE, EVENT.

Environment:
  FAKE_RELAY_PORT    — TCP port (default: 7777)
  FAKE_RELAY_EVENTS  — path to JSON array of event objects

Usage:
  FAKE_RELAY_PORT=7777 FAKE_RELAY_EVENTS=events.json python3 fake_relay_fixture.py
"""
import asyncio
import json
import os
import sys

try:
    # websockets >= 13 new-style import
    from websockets.asyncio.server import serve as ws_serve
except (ImportError, AttributeError):
    try:
        # websockets < 13 legacy import
        import websockets

        ws_serve = websockets.serve  # type: ignore[attr-defined]
    except ImportError:
        print("python3 websockets not installed; skipping", file=sys.stderr)
        sys.exit(77)

PORT = int(os.environ.get("FAKE_RELAY_PORT", "7777"))
EVENTS_FILE = os.environ.get("FAKE_RELAY_EVENTS", "")

stored_events: list = []


def load_events():
    global stored_events
    if EVENTS_FILE and os.path.exists(EVENTS_FILE):
        with open(EVENTS_FILE) as f:
            stored_events = json.load(f)


def matches_filter(event: dict, filt: dict) -> bool:
    """Check if *event* matches a NIP-01 filter object."""
    if "kinds" in filt:
        if event.get("kind") not in filt["kinds"]:
            return False
    if "authors" in filt:
        if event.get("pubkey") not in filt["authors"]:
            return False
    if "ids" in filt:
        if event.get("id") not in filt["ids"]:
            return False
    # Generic tag filters: #d, #p, #e, …
    for key, values in filt.items():
        if key.startswith("#") and len(key) == 2:
            tag_name = key[1]
            ev_vals = [
                t[1]
                for t in event.get("tags", [])
                if len(t) >= 2 and t[0] == tag_name
            ]
            if not any(v in values for v in ev_vals):
                return False
    return True


async def handler(ws):
    async for raw in ws:
        try:
            msg = json.loads(raw)
        except json.JSONDecodeError:
            continue

        if not isinstance(msg, list) or len(msg) < 2:
            continue

        verb = msg[0]

        if verb == "REQ":
            sub_id = msg[1]
            filters = msg[2:]
            matched = []
            for filt in filters:
                for ev in stored_events:
                    if ev not in matched and matches_filter(ev, filt):
                        matched.append(ev)
            # Honour limit from first filter that has one
            for filt in filters:
                if "limit" in filt:
                    matched = matched[: filt["limit"]]
                    break
            for ev in matched:
                await ws.send(json.dumps(["EVENT", sub_id, ev]))
            await ws.send(json.dumps(["EOSE", sub_id]))

        elif verb == "CLOSE":
            sub_id = msg[1] if len(msg) > 1 else ""
            await ws.send(json.dumps(["CLOSED", sub_id, ""]))

        elif verb == "EVENT":
            ev = msg[1] if len(msg) > 1 else {}
            eid = ev.get("id", "0" * 64)
            stored_events.append(ev)
            await ws.send(json.dumps(["OK", eid, True, ""]))


async def main():
    load_events()
    n = len(stored_events)
    print(
        f"fake_relay: ws://127.0.0.1:{PORT} ({n} events seeded)",
        file=sys.stderr,
        flush=True,
    )
    async with ws_serve(handler, "127.0.0.1", PORT):
        # Signal readiness on stdout so the caller can wait for it
        print("READY", flush=True)
        await asyncio.Future()  # run forever


if __name__ == "__main__":
    asyncio.run(main())
