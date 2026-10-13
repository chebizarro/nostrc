#!/usr/bin/env python3
"""Optional LLDB observer for the unchanged Debug-build daemon (G00).

Import with `lldb -b -o 'command script import <this file>' -o run --
<build>/apps/grotto/grotto-daemon --bunker`, under the fixture's private bus
and environment. Or set NOSTRC_BUNKER_DEBUGGER_WRAPPER to this executable
when running the bunker-e2e CTest. Normal CTest needs no debugger.
Only presence of key slots is printed, never private key bytes. No values,
return codes, or control flow are changed by the observer.
"""


def returned(frame, location, internal_dict):
    del location, internal_dict
    register = frame.FindRegister("x0")
    if not register.IsValid():
        register = frame.FindRegister("rax")
    value = register.GetValueAsUnsigned() & 0xffffffff
    if value >= 0x80000000:
        value -= 0x100000000
    print("G00 listener return rc=%d" % value, flush=True)
    return False  # Continue; do not substitute the return value.


def entered(frame, location, internal_dict):
    del location, internal_dict
    session = frame.FindVariable("s")
    slots = ("secret", "bunker_secret_hex", "bunker_pubkey_hex")
    values = []
    for name in slots:
        field = session.GetChildMemberWithName(name)
        assert field.IsValid(), "Debug symbols required for " + name
        values.append("s->%s=%s" % (name, "populated" if field.GetValueAsUnsigned() else "NULL"))
    print("G00 listen entry: " + " ".join(values), flush=True)
    caller = frame.GetThread().GetFrameAtIndex(1)
    breakpoint = frame.GetThread().GetProcess().GetTarget().BreakpointCreateByAddress(caller.GetPC())
    breakpoint.SetOneShot(True)
    breakpoint.SetScriptCallbackFunction(__name__ + ".returned")
    return False


def __lldb_init_module(debugger, internal_dict):
    del internal_dict
    debugger.HandleCommand("settings set target.disable-aslr false")
    breakpoint = debugger.GetSelectedTarget().BreakpointCreateByName("nostr_nip46_bunker_listen")
    breakpoint.SetScriptCallbackFunction(__name__ + ".entered")


if __name__ == "__main__":
    import os
    import shlex
    import sys
    command = "command script import " + shlex.quote(os.path.abspath(__file__))
    os.execvp("lldb", ["lldb", "--batch", "-o", command, "-o", "run", "--", *sys.argv[1:]])
