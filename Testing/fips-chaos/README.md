# fipsd netns/veth chaos harness

Linux network-namespace scenarios for `fipsd`, alone and against the pinned
upstream Rust daemon (`libfips-core/UPSTREAM.md`): link flaps, MTU changes,
NAT traversal through nftables and loss/latency under load. It is the
netns/veth subset of upstream's `testing/chaos` and `testing/nat` labs
(`nostrc-f66sr.3`), rebuilt on `ip netns` without Docker: every daemon,
relay and STUN responder runs in a namespace the harness creates, and every
assertion is a deadline-bounded observation through the daemons' control
sockets, their logs, the lab services' logs or kernel counters. No sleep
stands in for an event; the only pause is the poll interval between control
snapshots. Runs as root (`ip netns`, `nft`, `tc`).

```
run.sh                 root wrapper: tool check, chaos.py, purge of leftover namespaces on exit
chaos.py               driver: scenario/variant selection, evidence on failure, results.json, exit code
chaoslib/net.py        Lab/Netns: namespaces, veth pairs, bridge, nft, netem, FragCreates, conntrack
chaoslib/nodes.py      Mesh/MeshNode: daemons under `ip netns exec`, configs, link/tree/bloom/session/rekey waits
chaoslib/scenarios/    flap.py  mtu.py  nat.py  netem.py
services/relay.py      NIP-01 relay + NIP-11 document (stdlib asyncio; adverts, ephemeral signals)
services/stun.py       STUN binding responder (XOR-MAPPED-ADDRESS)
```

`chaoslib/nodes.py` imports the daemon lifecycle and control helpers from
`fipsd/tests/udp_interop.py` (`Daemon`, `control`, `NativeFlow`, the
NotYet/Failure wait contract); path-based Unix control sockets are reachable
from the root namespace, so the driver never enters a namespace itself.

## Run

```bash
cmake --build build --target fipsd
sudo Testing/fips-chaos/run.sh --fipsd build/fipsd/fipsd \
    --rust /tmp/fips-rust-target/release/fips      # pinned Rust daemon; omit to skip the cr variants
# options: --scenario flap mtu nat netem   --variant cc cr   --nat-mode masquerade|cone
#          --output DIR (default fips-chaos-results)   --timeout-scale 2   --keep   --purge
```

Each scenario runs as `cc` (fipsd only) and `cr` (fipsd with the Rust daemon
in the role noted below). The driver prints one `RESULT <scenario>/<variant>
PASS|FAIL|SKIP <seconds> <measurements>` line per run and writes
`results.json`; on failure it also writes the daemon and service logs, the
harness trace and per-namespace diagnostics (addresses, routes, qdiscs, nft
ruleset, conntrack, sockets) under `--output/<scenario>-<variant>/` and prints
the tail of every log. Namespaces are named `fc<scenario><variant>-<node>`;
teardown runs in `finally`, `run.sh` purges on exit, and a stale set from a
crashed run is purged before the same scenario starts again (`--purge` does
only that). Exit 0 when every selected run passed, 1 otherwise, 2 for usage or
environment errors. Independent scenarios may run concurrently with
`chaos.py` directly (distinct namespace prefixes); `run.sh`'s exit purge is
global, so use it for sequential runs.

## Scenarios

| Scenario | Topology | What is asserted |
| --- | --- | --- |
| `flap` | n1 — n2 — n3 chain; n1 and n3 are fipsd with n2 as a configured `auto_reconnect` peer; n2 is fipsd (`cc`) or Rust (`cr`) | After `ip link set n1/eth0 down` both ends drop the peer and link rows (the 30 s authenticated-silence floor bounds detection), n2–n3 re-converge alone; after `up` n1's configured retry re-establishes the link, the three-node tree and bloom filters converge, the same native application flow delivers another n1 → n3 datagram, and a fresh two-hop probe completes with an RTT sample |
| `mtu` | n1 — n2 veth; n2 is fipsd or Rust | With both veth ends at MTU 600 from the start, IK link, tree and the 1 KiB bloom announce complete and each namespace's IPv4 `FragCreates` rises (the datagrams really exceeded the MTU); a probe and, in `cr`, a maximum-payload Rust → C native datagram are delivered; then the MTU is raised to 1500 and lowered to 900 under the live link and the same traffic flows again. The configured transport MTU (`show_transports[].mtu`) and the session `mmp.path_mtu` are recorded |
| `nat` | `wan` bridge 198.18.0.0/24 with the lab relay (:7777) and STUN (:3478); routers `nata`/`natb` (198.18.0.10/.11 ↔ 172.31.1.0/24, 172.31.2.0/24) with `ip_forward` and an nftables `masquerade` on the wan leg; nodes `na`/`nb` behind them, default route via their router | STUN binding requests from each router's wan tuple, a kind-37195 advert from each node, a kind-21059 signal addressed to each node, the punch's UDP conntrack entry in each router, then a healthy link on both sides whose `remote_addr` is the peer's reflexive tuple, peers cross-matched, tree converged, a probe (XK session) and in `cr` a native datagram through both NATs. A timeout names the last stage reached (`stages` in the failure text) |
| `netem` | n1 — n2 veth with `netem delay 40ms 10ms loss 5% 25%` on n1's egress from before start; iperf3 UDP 10 Mbit/s n1 → n2 on the same leg when iperf3 is installed | Link and tree form under impairment; with `node.rekey.after_secs: 25` the FMP link rekey (K bit flips on both peer rows) completes within bound and, in `cr` where a Rust native datagram holds an XK session open, the FSP session rekey too; probes and datagrams flow under the new keys; no mismatch/decrypt/bad-state/replay counter rises |

NAT model: Linux `masquerade` preserves the source port when it can and
filters by the full 5-tuple, so both nodes sit behind port-restricted cone
NATs and the punch from both sides must open them; `--nat-mode cone` installs
upstream's explicit SNAT/DNAT full-cone emulation instead. The WAN uses
198.18.0.0/24 (benchmark range) because both daemons refuse to punch a private
reflexive address that no local interface prefix holds. fipsd runs with
`policy: open` and discovers the peer's `udp:nat` advert; the Rust daemon is
given fipsd as a configured `addr: nat` peer (`via_nostr: true`), the upstream
NAT lab shape. The relay verifies event ids, not signatures (no secp256k1 in
the standard library); both daemons verify what they receive.

## What the runs taught (and the harness now encodes)

- **Lab routers must drop unsolicited input.** Plain `masquerade` with no
  filter chain confirmed a conntrack entry for the peer's first punch (it
  arrived before this side had mapped its port and was delivered to the
  router's own stack), which took the reply tuple `:2121`; the node's own
  mapping was then moved to a random port and the two punches never met.
  Real masquerading routers drop such input, so `nat.py` installs
  `iifname "wan" ct state new,invalid drop` and the mapping stays
  port-preserving. A symmetric NAT (`masquerade random`) is the same
  topology and is expected to defeat the punch.
- **The Rust daemon punches from a per-attempt socket.** Its STUN request,
  punch and adopted link use a fresh local port (not its bound `:2121`), so
  fipsd's link shows the Rust peer at `198.18.0.11:<ephemeral>`. Stage
  evidence and `remote_addr` are therefore matched on the reflexive
  *address*; fipsd itself punches from its bound tuple.
- **Open the application session before a diagnostic probe.** A probe tears
  its own session down only locally (as upstream's `probe_teardown` does).
  The remote peer retains its session: subsequent datagrams can be dropped as
  `session.unknown_session`, and silence recovery can leave a new responder
  awaiting msg3 when the next probe starts. Cached discovery is not proof that
  this handshake has completed. `flap.py` keeps a C-native application flow
  across the flap and checks payload delivery before and after it; its baseline
  probe must preserve that session. `mtu.py` and `nat.py` likewise open the
  native flow first, as `udp_interop.py` does. No probe budget is extended.
- **fipsd re-runs traversal toward a connected peer on every advert refresh**
  (every `advert_refresh_secs`, the Rust side answers and punches again). It
  is harmless to the established link and is reported, not asserted;
  `fipsd/src/nostr/driver.c` is the owner.
- **A diagnostic probe to a non-direct target always timed out** in its
  bloom stage (`no_response` after two ticks) even though the lookup had
  been answered and the coordinates cached: the probe adapter never handed
  the lookup-initiation outcome to the probe core. Fixed in
  `fipsd/src/node/handlers/{probe,lookup}.c` with a regression in
  `FipsdNodeHandlersTest` (a target no filter covers now fails as
  `bloom_miss` on the next poll). `flap.py`'s two-hop probe is the
  integration check, after waiting for the middle node's *merged* filter
  (`estimated_count >= 2`), since the first announce carries only the sender.

## What the MTU scenario does and does not prove

Neither fipsd nor the pinned Rust daemon sets `IP_MTU_DISCOVER`/DF on its
UDP sockets or watches interface MTUs: the UDP transport MTU is the
configured value (1357 by default) and only session-level
`PathMtuNotification`/`MtuExceeded` signals from a FIPS transit node lower a
session MTU. On a direct veth the kernel therefore fragments every datagram
larger than the interface MTU and reassembles it at the receiver, which is
what the scenario measures and asserts. It does not assert that either daemon
lowers a reported MTU, because neither does; `transport_mtu_adapted` in the
measurements records that fact per run. An asymmetric MTU (one end lowered,
the far end's frames silently dropped by veth) is a black hole neither daemon
detects at the link layer today; the IPv6 shim's ICMP/PMTU handling is the
sibling task's scope.

## CI

`.github/workflows/fips-ci.yml` job `chaos-netns` runs nightly (`schedule`)
and on `workflow_dispatch` on `ubuntu-24.04`: it reads the pin, checks out
upstream, restores or builds the pinned Rust daemon with the same steps as
`interop-udp`, installs `iproute2 nftables iperf3 conntrack`, builds `fipsd`
with ASan/UBSan and runs `sudo -E Testing/fips-chaos/run.sh` for every
scenario and variant; `build/chaos-results/` is uploaded on failure.
