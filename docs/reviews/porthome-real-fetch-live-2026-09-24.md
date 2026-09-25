# porthome real-fetch + sandbox live-verify (2026-09-24)

Beads: **nostrc-9k4g** (real kind-30078 pointer fetch + SSRF-guarded helper
wire-in) and **nostrc-ww50** (subprocess sandbox for the provisioner
fetch closure).

## Scope of live verification

The tree is under active development on aarch64 Ubuntu 24.04
(`bizarro@192.168.64.3`); the two units this change touches build and
run standalone (no libnostr / libhanami dep), so we validated them
directly on the target rather than staging a full CMake configure.
End-to-end broker-to-relay smoke is deferred to the next round after
Workers E / F / G land — this note documents unit-level parity between
the Darwin dev host and the aarch64 target.

Target:

```
$ ssh bizarro@192.168.64.3 "hostname && uname -a"
bizarro-QEMU-Virtual-Machine
Linux 7.0.0-31-generic #31~24.04.1-Ubuntu SMP PREEMPT_DYNAMIC aarch64
```

## `test_porthome_fetch_ssrf` (new, bead nostrc-9k4g)

Freestanding unit test that exercises the SSRF helpers now embedded in
`nostr-home-fetch.c`: `is_ipv4_public_unicast`, `is_ipv6_public_unicast`,
and `url_host`. The helpers are duplicated into the test (the fetch
binary carries a large libnostr / libhanami dep closure the test
deliberately avoids) — any drift is caught by review.

```
$ cd /tmp/nostrc-9k4g-ww50
$ cc -Wall -Wextra -Werror -o test_porthome_fetch_ssrf test_porthome_fetch_ssrf.c
$ ./test_porthome_fetch_ssrf
PASS ipv4_ranges
PASS ipv6_ranges
PASS url_host
test_porthome_fetch_ssrf: ALL PASS
```

Coverage:

- IPv4 refuses `0/8`, `10/8`, `127/8`, `169.254/16`, `172.16/12`,
  `192.168/16`, `100.64/10` (CGNAT), `224/4` (multicast),
  `255.255.255.255`; accepts `1.1.1.1`, `8.8.8.8`, `93.184.216.34`.
- IPv6 refuses `::1`, `fe80::/10`, `fc00::/7`, `ff00::/8`,
  `::ffff:127.0.0.1` (v4-mapped loopback); accepts `2606:4700::1111`.
- `url_host` parses IPv6 literals in brackets, port-suffixed hosts, and
  `user:pass@host` URLs — the last one caught a bug in the first draft
  where the userinfo scanner stopped at `:` inside `user:pass`
  (would have returned `"user"` as the host) and was widened to end at
  the first `/`.

## `test_porthome_sandbox` (extended, bead nostrc-ww50)

Six-case coverage; the new case is `test_keep_fds`, which proves the
`nh_porthome_spawn_sandboxed_ex` seam that `auth_porthome_fetch_spawn`
uses to inherit the staging dirfd across `close_fds_except` + `execve`.

```
$ cd /tmp/nostrc-9k4g-ww50
$ cc -Wall -Wextra -Werror -Wno-misleading-indentation \
    nh_porthome_sandbox.c test_porthome_sandbox.c \
    -o test_porthome_sandbox
$ NH_PORTHOME_SANDBOX_ALLOW_NONROOT=1 ./test_porthome_sandbox
PASS uid=1000 (never root)
PASS no_new_privs set
PASS fd hygiene (only 0/1/2 survive)
PASS not_root refusal
PASS relative argv[0] refused
PASS fallback-user api readable
PASS keep_fds inherited
test_porthome_sandbox: ALL PASS
```

The `keep_fds inherited` assertion runs a child that reads
`/proc/self/fd/<n>` for the extra fd we passed via `keep_fds` and
prints `KEEP-OK` if it survives both the `close_fds_except` sweep AND
`execve` (via cleared `FD_CLOEXEC`).

## What the wire-in changed

**Before:** `nh_auth_porthome_fetch_spawn` did a bare
`fork()` / `execv()` under the broker's ambient uid=0. The
`nostr-home-fetch` helper's `main()` refuses to start with
`getuid() == 0`, so the whole real-fetch path returned
`NH_PORTHOME_FETCH_EXIT_ARG` and every production login fell straight
into limited-mode without a single relay REQ going out.

**After:** the spawn goes through `nh_porthome_spawn_sandboxed_ex`
(new `_ex` variant of the pre-existing `nh_porthome_spawn_sandboxed`
which takes a caller-supplied `keep_fds[]`). The sandbox does the
setgroups/setgid/setuid drop to `nostr-home-fetch`/`nobody`,
`PR_SET_NO_NEW_PRIVS`, `PR_SET_DUMPABLE=0`, and RLIMIT caps that the
helper's `refuse to run as root` guard was waiting for. The
`child_staging_fd` is passed through via the keep-list so
`--staging-fd N` still resolves inside the child. Reap now goes
through the sandbox waiter so SIGTERM→SIGKILL escalation and
`timed_out` reporting stay consistent with the profile-image path.

## What the SSRF gate changed

The Blossom URL sanity check
(`nh_porthome_blossom_url_ok`) already refuses obvious loopback string
literals. libhanami wraps libcurl but doesn't expose a
`CURLOPT_OPENSOCKETFUNCTION` seam, so a hostname whose DNS entry maps
to a LAN address would previously slip past. The helper now runs a
`getaddrinfo`-based pre-resolution pass over every Blossom server host
and refuses (`NH_PORTHOME_FETCH_EXIT_SSRF = 65`) if ANY resolved
address is not public-unicast. This is not a DNS-rebinding defense
(the OS resolver may return different addresses later), but it catches
the common misconfiguration classes the design (§8.2) explicitly
called out — and the sandbox drop-privs perimeter around it means a
real rebinding exploit would still land in the `nostr-home-fetch`
uid, not the broker.

## What is still deferred (out of this bead)

- **Live relay + Blossom round-trip** via `nostr-home-publisher` on the
  target: needs the full portable-home CMake configure and the
  wss://relay.sharegap.net / https://blossom.sharegap.net fixtures to
  cohabit with Workers E/F/G in-flight. Deferred to a follow-up smoke
  after those workers land (they touch overlapping install / packaging
  paths).
- **DNS-rebinding hardening**: would need a socket-level hook inside
  libhanami-blossom. Filed for a future bead; the sandbox drop-privs +
  sockaddr-family refusal in the helper is the intended v1 gate.
- **Pointer-live-subscription** for post-login updates: already handled
  by `porthome-syncd/nh_syncd_subscription.c` (§6.2); the broker's
  fetch is intentionally a one-shot REQ (auto-unsub on EOSE via
  `nostr_simple_pool_set_auto_unsub_on_eose`) because the login path
  can't hold a long-lived subscription in the PAM stack.

## nm / ldd baseline

Deferred to the packaging build (the base `nostr-authd` binary already
takes `nostr_porthome` (which contains `nh_porthome_sandbox.o`) as a
PUBLIC dep behind `NH_AUTH_BROKER_ENABLE_PORTHOME`; the extra symbol
`nh_porthome_spawn_sandboxed_ex` lands in the same archive so no new
transitive lib is pulled in — libc alone). `pam_nostr.so` continues
to NOT depend on any of this: the code lives entirely under the
broker's `NH_AUTH_BROKER_ENABLE_PORTHOME` gate.
