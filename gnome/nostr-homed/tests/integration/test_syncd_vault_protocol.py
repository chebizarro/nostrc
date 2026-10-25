#!/usr/bin/env python3
"""Real pool publisher against a bounded protocol fixture, not live relay proof."""
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "tools/vaultctl/tests"))
from fixtures import Handler, Lab, verify

class ReauthHandler(Handler):
    def frame(self, value, op=1):
        if (isinstance(value, list) and value[0] == "OK" and
                value[1] in self.server.attempts and not self.server.challenged):
            self.server.challenged = True
            self.late_auth = True
            self.server.events.pop(value[1], None)
            super().frame(["AUTH", "during-publication"])
            super().frame(["OK", value[1], False, "auth-required: reauthenticate"])
            return
        super().frame(value, op)

    def recv(self):
        value = super().recv()
        if value and value[0] == "AUTH" and getattr(self, "late_auth", False):
            event = value[1]
            verify(event)
            assert event["kind"] == 22242
            assert ["challenge", "during-publication"] in event["tags"]
            assert ["relay", self.server.ws] in event["tags"]
            self.server.auths += 1
            self.late_auth = False
            self.frame(["OK", event["id"], True, ""])
            return None
        return value

lab = Lab()
lab.RequestHandlerClass = ReauthHandler
lab.challenged = False
try:
    result = subprocess.run([sys.argv[1], "publish-auth", lab.ws, lab.url],
        capture_output=True, text=True, timeout=45)
    assert result.returncode == 0, result.stdout + result.stderr
    assert lab.challenged and lab.auths >= 2 and lab.reqs >= 2
    assert len(lab.attempts) == 2 and lab.attempts[0] == lab.attempts[1]
    assert not lab.failures, lab.failures
    print(result.stdout.strip())
    print("PASS signed NIP-42 challenge/OK during publication, exact event ID retry, no sleep/timeout completion")
finally:
    lab.finish()
