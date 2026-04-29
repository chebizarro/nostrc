#!/usr/bin/env python3
"""
fake_relay_fixture.py — Minimal NIP-01 WebSocket relay for integration tests.

Serves pre-seeded events from a JSON file.  Supports REQ, CLOSE, EVENT.

Environment:
  FAKE_RELAY_PORT           — TCP port (default: 7777)
  FAKE_RELAY_EVENTS         — path to JSON array of event objects
  FAKE_RELAY_REQUIRE_AUTH   — if "1", send AUTH challenge and reject unauthenticated REQ/EVENT
  FAKE_RELAY_REJECT_PUBKEYS — comma-separated hex pubkeys to reject with OK false
  FAKE_RELAY_VERIFY_SIGS    — if "1", verify event.id == SHA256(serialized_event)

Usage:
  FAKE_RELAY_PORT=7777 FAKE_RELAY_EVENTS=events.json python3 fake_relay_fixture.py
"""
import asyncio
import hashlib
import json
import os
import sys
import uuid

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
REQUIRE_AUTH = os.environ.get("FAKE_RELAY_REQUIRE_AUTH", "") == "1"
REJECT_PUBKEYS = set(
    pk.strip() for pk in os.environ.get("FAKE_RELAY_REJECT_PUBKEYS", "").split(",") if pk.strip()
)
VERIFY_SIGS = os.environ.get("FAKE_RELAY_VERIFY_SIGS", "") == "1"

stored_events: list = []
# Track authenticated state per websocket connection
authenticated_sockets: set = set()
# Track active AUTH challenges per socket
auth_challenges: dict = {}


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


def verify_event_id(event: dict) -> bool:
    """Verify event.id == SHA256(serialized_event) per NIP-01."""
    try:
        # Build canonical serialization: [0, pubkey, created_at, kind, tags, content]
        serialized = json.dumps(
            [
                0,
                event.get("pubkey", ""),
                event.get("created_at", 0),
                event.get("kind", 0),
                event.get("tags", []),
                event.get("content", ""),
            ],
            separators=(",", ":"),
            ensure_ascii=False,
        )
        computed_id = hashlib.sha256(serialized.encode("utf-8")).hexdigest()
        return computed_id == event.get("id", "")
    except Exception:
        return False


async def handler(ws):
    # Send AUTH challenge if required
    if REQUIRE_AUTH:
        challenge = str(uuid.uuid4())
        auth_challenges[ws] = challenge
        await ws.send(json.dumps(["AUTH", challenge]))

    async for raw in ws:
        try:
            msg = json.loads(raw)
        except json.JSONDecodeError:
            continue

        if not isinstance(msg, list) or len(msg) < 2:
            continue

        verb = msg[0]

        if verb == "REQ":
            # Check authentication if required
            if REQUIRE_AUTH and ws not in authenticated_sockets:
                sub_id = msg[1]
                await ws.send(
                    json.dumps(
                        ["CLOSED", sub_id, "auth-required: please authenticate"]
                    )
                )
                continue

            sub_id = msg[1]
            filters = msg[2:]
            
            # F37 fix: Apply limit per-filter independently, then merge with dedup
            all_matched = []
            seen_ids = set()
            
            for filt in filters:
                filter_matched = []
                for ev in stored_events:
                    if matches_filter(ev, filt):
                        filter_matched.append(ev)
                
                # Apply limit to this filter's results
                limit = filt.get("limit")
                if limit is not None and isinstance(limit, int):
                    filter_matched = filter_matched[:limit]
                
                # Merge into all_matched with dedup by event id
                for ev in filter_matched:
                    ev_id = ev.get("id")
                    if ev_id and ev_id not in seen_ids:
                        seen_ids.add(ev_id)
                        all_matched.append(ev)
            
            for ev in all_matched:
                await ws.send(json.dumps(["EVENT", sub_id, ev]))
            await ws.send(json.dumps(["EOSE", sub_id]))

        elif verb == "CLOSE":
            sub_id = msg[1] if len(msg) > 1 else ""
            await ws.send(json.dumps(["CLOSED", sub_id, ""]))

        elif verb == "EVENT":
            # Handle AUTH events specially
            ev = msg[1] if len(msg) > 1 else {}
            eid = ev.get("id", "0" * 64)
            
            # Check if this is a NIP-42 AUTH event (kind 22242)
            if ev.get("kind") == 22242 and REQUIRE_AUTH:
                # Verify AUTH event contains the correct challenge tag
                challenge = auth_challenges.get(ws)
                if challenge:
                    tags = ev.get("tags", [])
                    challenge_tag = None
                    for tag in tags:
                        if len(tag) >= 2 and tag[0] == "challenge":
                            challenge_tag = tag[1]
                            break
                    
                    if challenge_tag == challenge:
                        # Mark socket as authenticated
                        authenticated_sockets.add(ws)
                        await ws.send(json.dumps(["OK", eid, True, ""]))
                        continue
                    else:
                        await ws.send(
                            json.dumps(
                                ["OK", eid, False, "invalid: challenge mismatch"]
                            )
                        )
                        continue
                else:
                    await ws.send(
                        json.dumps(["OK", eid, False, "invalid: no challenge found"])
                    )
                    continue

            # Check authentication for regular events if required
            if REQUIRE_AUTH and ws not in authenticated_sockets:
                await ws.send(
                    json.dumps(
                        ["OK", eid, False, "auth-required: please authenticate"]
                    )
                )
                continue

            # F37 fix: Optional signature verification
            if VERIFY_SIGS and not verify_event_id(ev):
                await ws.send(
                    json.dumps(["OK", eid, False, "invalid: event id verification failed"])
                )
                continue

            # F14 fix: Check if pubkey is in reject list
            pubkey = ev.get("pubkey", "")
            if pubkey in REJECT_PUBKEYS:
                await ws.send(
                    json.dumps(["OK", eid, False, "blocked: pubkey not admitted"])
                )
                continue

            # Accept and store the event
            stored_events.append(ev)
            await ws.send(json.dumps(["OK", eid, True, ""]))

    # Clean up socket state when connection closes
    authenticated_sockets.discard(ws)
    auth_challenges.pop(ws, None)


async def main():
    load_events()
    n = len(stored_events)
    mode_flags = []
    if REQUIRE_AUTH:
        mode_flags.append("AUTH")
    if REJECT_PUBKEYS:
        mode_flags.append(f"REJECT={len(REJECT_PUBKEYS)} pubkeys")
    if VERIFY_SIGS:
        mode_flags.append("VERIFY_SIGS")
    
    mode_str = f" [{', '.join(mode_flags)}]" if mode_flags else ""
    print(
        f"fake_relay: ws://127.0.0.1:{PORT} ({n} events seeded){mode_str}",
        file=sys.stderr,
        flush=True,
    )
    async with ws_serve(handler, "127.0.0.1", PORT):
        # Signal readiness on stdout so the caller can wait for it
        print("READY", flush=True)
        await asyncio.Future()  # run forever


if __name__ == "__main__":
    asyncio.run(main())
