# fips-gateway

Task `nostrc-iq1n9.3`. `fips-gateway` is the C port of upstream
`src/gateway/*` and `src/bin/fips-gateway.rs` at the pin in
[`libfips-core/UPSTREAM.md`](../../libfips-core/UPSTREAM.md)
(`6aa80504baa0fa312b392009eea0e3e59fb6fe4c`). This page mirrors three upstream
documents for this tree:
- `docs/design/fips-gateway.md` (the design)
- `docs/how-to/deploy-gateway.md` (the deployment recipe)
- `docs/how-to/troubleshoot-gateway.md` (the diagnostic recipes)

Upstream's prose stays the reference for anything stated here only in outline.
The behaviours are upstream's unless [Differences from upstream](#differences-from-upstream)
says otherwise.

The gateway lets unmodified IPv6 hosts on a LAN reach the mesh, and lets mesh
peers reach chosen LAN services. It has two halves:

- **Outbound (LAN to mesh).** A LAN host resolves `<npub>.fips` through the
  gateway's DNS proxy and gets a virtual IPv6 address from a managed pool.
  nftables DNAT rewrites that address to the node's `fd00::/8` mesh address,
  and a masquerade gives the flow the gateway's own `fips0` source.
- **Inbound (mesh to LAN).** Each `gateway.port_forwards[]` entry turns a port
  on the gateway's `fips0` address into a LAN `[host]:port` (DNAT), with a
  LAN-side masquerade so the replies come back through the gateway.

`fips-gateway` is a separate executable. It runs beside `fipsd` on the same
host and reads the same `fips.yaml` (the `gateway:` block, plus `tun.name`).
It is not a peer: it reaches the daemon only through the daemon's `.fips`
resolver, and it sees mesh traffic only as packets on `fips0`. It is
**Linux-only** (nftables and proxy NDP), and CMake adds the target on Linux
alone, when `libnftables` is found.

## Design

### Modules

The portable modules form `fipsd_gateway_core`, which builds on every platform,
so their tests run on macOS too. Only `main.c` and the libnftables backend are
Linux-only.

| File | Upstream | Owns |
| --- | --- | --- |
| `types.{c,h}`, `addrmap.{c,h}` | `HashMap`/`HashSet`, `Instant` | Monotonic instants, IPv6 helpers, CIDR parsing, the pool event list; an open-addressing map from fixed-size keys (addresses, address pairs, offsets) with tombstones and `fipsd_gw_map_reserve`. |
| `pool.{c,h}` | `pool.rs` | The virtual IP pool: allocation in ring order, the mapping state machine, the ceiling, the token bucket, durable marks, restart holds, conntrack evidence and release. Takes the caller's clock, so its tests drive time. |
| `conntrack.{c,h}` | `conntrack.rs`, the reader in `pool.rs` | `/proc/net/nf_conntrack` parsing; the `NETLINK_NETFILTER` ctnetlink dump (Linux); the reader that falls back from proc to netlink, re-decided on every read; the probe and the change-only read log. |
| `state.{c,h}` | `state.rs` | `/var/run/fips-gateway/pool.json`: tmpfs detection, the atomic write through `pool.json.tmp`, load-and-remove, `start_pool` from a previous run's holds, and the save log. |
| `nat.{c,h}` | `nat.rs` | The desired NAT state (mappings, forwards), rendering it as one nftables JSON batch, the cleanup batch, the pending and unconfirmed-removal bookkeeping, the outcome latches, and the NAT worker thread (`fipsd_gw_nat_driver`). |
| `nat_nft.c` | the rustables calls | The libnftables backend: a fresh `nft_ctx` per batch, `nft_run_cmd_from_buffer` with the JSON flag, and the `Error:` lines as the failure text. |
| `net.{c,h}` | `net.rs` | The forwarding and proxy-NDP sysctl checks; the `local <pool> dev lo` route; `ip -6 neigh add/del proxy` (forked, as upstream); interface lookup and the netlink wait for the daemon's TUN; cleanup of every entry added. |
| `dns.{c,h}` | `dns.rs` | The DNS proxy: the screen (`fipsd/src/dns/msg.{c,h}`, shared with the daemon's responder), `.fips` forwarding to the daemon with ID and question matching, pool allocation, the answer, the listener bind with in-use hints, and the serve loop. |
| `control.{c,h}` | `control.rs` | `/run/fips/gateway.sock`: the published snapshot, `show_gateway`/`show_mappings`, the line protocol (the daemon's encoder from `fipsd/src/control/`) and the group (`fipsd_privilege_resolve_group`, best effort). |
| `shutdown.{c,h}` | `shutdown.rs` | Task tracking and `fipsd_gw_stop_tasks`: the bounded, ordered stop of the resolver and the tick. |
| `main.c` | `bin/fips-gateway.rs` | Options, configuration, the start-up checks and waits, set-up, the fibers, the event loop and the shutdown sequence. |

### Concurrency

`main.c` runs the gateway as one `fipsd_runtime` subsystem on libgo fibers:

- **DNS fiber.** Serves the listener. Each `.fips` query is forwarded to the
  daemon and allocated under the pool lock.
- **Tick fiber.** Runs every 10 s. It reads conntrack on the blocking executor
  (`go_blocking_submit`), outside the pool lock. Then it runs the pool tick,
  publishes the control snapshot and sends the tick's pool events. The state
  file write also runs on the executor, without the lock.
- **Loop fiber.** Receives pool events and turns each into a NAT command plus a
  proxy NDP change (the `ip` fork runs on the executor). It ends the gateway
  when the NAT worker or the DNS fiber stops unexpectedly.
- **Control fiber.** Accepts connections, with one fiber per connection.

The NAT worker is a plain pthread that owns the table. A rebuild is a netlink
round trip, and it never runs on a fiber. Every queued change goes into one
rebuild. The pool is shared under an `nsync` mutex, the control snapshot is
reference-counted, and events reach the loop through a channel of 64.

### nftables backend: libnftables

The table is rendered as one JSON document. `libnftables`, the library `nft`
itself is built on (over libnftnl/libmnl), sends that document as one netlink
batch, which the kernel applies as a single transaction. That is the guarantee
upstream's hand-built rustables batch relies on.

libnftables was chosen over driving libnftnl/libmnl directly for these reasons:

- **Testable.** The rule shapes are plain text that tests compare with goldens
  (`fipsd/tests/gateway_golden/nat_{full,empty,cleanup}.json`), not netlink
  attributes encoded by hand. The golden batches also load in real nftables.
- **Less bookkeeping.** The library sizes the socket buffers and collects the
  acknowledgements itself, which upstream does by hand.
- **Readable errors.** A refused batch comes back as the kernel's or the
  parser's own text.
- **Packaged everywhere.** It is the library distributions ship as
  `libnftables-dev` / `nftables-devel`.

The JSON format and the context flags used are stable across libnftables
releases with a JSON parser (0.9 and later; Ubuntu 24.04 ships 1.0.9).

### Table layout and rules

Everything lives in `inet fips_gateway`, as upstream:

- **Chains (six):** `prerouting` (nat, dstnat), `output` (nat, dstnat),
  `postrouting` (nat, srcnat), `forward` (filter, filter), `raw_prerouting`
  (filter, raw) and `input` (filter, filter).
- **Set:** `fips_mesh_sources`, holding every mapped mesh address.

Every rebuild deletes and recreates the table inside one batch, as an add,
delete and add. The batch holds the following rules in upstream's order:

- the pool drop, the forged-source drop and the forward drop;
- the neighbour-solicitation accept and the pool reject;
- the two `oifname fips0` masquerades, the LAN's first;
- the LAN-side masquerade when any forward exists, ahead of every SNAT;
- the per-mapping prerouting DNAT, output DNAT and SNAT;
- the per-forward DNAT.

`fipsd/tests/test_gateway_nat.c` checks each rule shape against upstream's
`nat.rs` tests. Upstream's design § NAT Pipeline gives each rule's match and
action.

### DNS proxy

The behaviour is upstream's:
- **Default listener:** `[::1]:5365`, with a warning when port 5353 (mDNS) is
  configured.
- **Screening:** a response, a non-query shape, a datagram shorter than a
  header or filling the 4096-byte buffer, and the small-service source ports
  are all dropped.
- **Header-only replies:** `NOTIMP` for another opcode and `FORMERR` for a
  missing or bad question.
- **Refused:** `REFUSED` for a non-IN/ANY class and for every non-`.fips`
  name. The proxy never resolves public DNS.
- **Forwarding:** `.fips` queries go to `gateway.dns.upstream` (default
  `[::1]:5354`) with a 5 s timeout and ID and question matching. An
  unreachable daemon gets `SERVFAIL`; a relayed `NXDOMAIN` carries an SOA; an
  answer without an AAAA gets `SERVFAIL`.
- **Allocation:** AAAA and ANY allocate. Any other type refreshes a used
  mapping and is answered NODATA with an SOA of the configured TTL.
- **Answer TTL:** a used mapping is answered with the full TTL, an unused one
  with at most the time left until TTL after its creation.
- **Echoed fields:** every reply carries the client's ID, its RD and CD bits,
  and its question byte for byte.
- **Mesh-interface queries:** a query that arrives on the daemon's TUN
  (`tun.name`) is dropped and never forwarded.

### Virtual IP pool

The pool works exactly as upstream's `pool.rs` (all 56 of its tests are ported):
- **Size:** at most 2^16 addresses, with offset 0 reserved. A CIDR with host
  bits set is used as its prefix; a `/128` is refused.
- **States:** Allocated, Active, Draining, then released. Draining returns to
  Active when traffic resumes, or to Allocated on a query for a used name.
- **Timing:** TTL `gateway.dns.ttl` (default 60), grace
  `gateway.pool_grace_period` (default 60), a 10 s tick.
- **Allocation order:** ring order from a random start.
- **Limits:** a ceiling of 1000 live mappings, where a new name replaces the
  oldest never-used one; a token bucket of 50 that refills at 10 per second;
  one warning per bound, with refusals summarised after 60 quiet seconds; a
  warning for a pool smaller than 1000 + 10 x (TTL + grace).
- **Evidence of use:** a conntrack reply from the mapped mesh address. Trust is
  lost after 3 unread ticks and restored after 3 good reads, and the gateway
  takes 3 reads before it answers its first name.
- **Release:** an address returns only after its answer can no longer be
  cached, its NAT removal is confirmed and 3 reads have followed. It never
  returns to the same node while an old conntrack entry survives.
- **Restart holds:** `/var/run/fips-gateway/pool.json` on tmpfs only, with
  stretches of 512 addresses and a refusal at an unconfirmed mark. A clean
  stop writes the exact live set. The next start holds those addresses for TTL
  plus grace.

### Control socket

`/run/fips/gateway.sock`, in the daemon's `/run/fips`, takes one JSON request
per line, each answered with `{"status":"ok","data":…}` or
`{"status":"error","message":…}`.
- **Permissions:** mode `0770`, owned by group `fips` when that group exists
  (otherwise the effective group is kept, with a warning).
- **`show_gateway`:** `pool_total`, `pool_allocated`, `pool_active`,
  `pool_draining`, `pool_free`, `pool_evicted`, `pool_releasing`,
  `pool_refused`, `nat_mappings`, `dns_listen`, `uptime_secs`, `pool_cidr`,
  `lan_interface`, `dns_upstream`, `dns_ttl`, `pool_grace_period`.
- **`show_mappings`:** `{"mappings":[…]}` with `virtual_ip`, `mesh_addr`,
  `node_addr`, `dns_name`, `state`, `sessions`, `age_secs`, `last_ref_secs`,
  `used`.
- **Errors:** an unknown command gets `unknown command: <name>`, and a request
  before the first tick gets `gateway not yet initialized`.
- **Binding:** a socket already held by a live gateway refuses the bind. A
  failed bind is a warning, and the gateway runs without the socket.

There is no `fipsctl` subcommand, as upstream; use `nc -U` or `socat`.

### Atomic rebuild and the NAT worker

The worker drains the queue into one rebuild. A refused batch leaves the
previous table in place, and the latest desired state is retried every 10 s,
even with no new event. A removal is reported to the pool only once a rebuild
without it has been accepted. Failures log once per change of outcome, with
repeats at debug level.

If the worker stops, the loop fails the gateway with exit status 1, so the
service manager restarts it rather than answer with addresses that have no
rules. One case differs from upstream, because C has no allocation abort: a
failed allocation in the NAT manager would lose a change or a removal report,
so it stops the worker by itself (`FIPSD_GW_NAT_FATAL`), which leads to that
same restart.

### Shutdown order

The order is upstream's `shutdown.rs` and `bin/fips-gateway.rs`:

1. **Stop the tasks.** Close the event channel, so a blocked send fails at
   once, then stop the DNS server and the control listener. Wait at most 1 s
   for the resolver and the tick, and abandon a task that overruns.
2. **Record the pool.** Log the final pool status, then write the exact live
   addresses to `pool.json`. The write is skipped when the tick was abandoned:
   its last periodic write stands.
3. **Clean up the network.** Remove every proxy NDP entry, then the pool
   route.
4. **Delete the table.** The NAT worker deletes it, with a wait of at most
   10 s. A worker that overruns is left behind, and the logger is kept for
   it.
5. Log `fips-gateway shutdown complete` and exit.

The runtime joins its fibers and the blocking executor before step 2, so no
abandoned task can touch freed state.

### Differences from upstream

- **nftables backend.** The C port uses libnftables JSON instead of
  rustables' netlink messages, with the same single-transaction batch.
- **Allocation failure.** Rust aborts the process. Here, the pool reserves
  everything an allocation or reclaim needs before it changes anything, so a
  failure leaves the pool as it was and the query gets `SERVFAIL`. In the NAT
  manager, a failure stops the worker, and the gateway exits for its restart.
- **Logging.** `-l/--log-level`, `FIPSD_LOG_LEVEL`, or `RUST_LOG` holding a
  bare level (`trace` to `error`). Per-module tracing directives are not
  parsed.
- **`gateway.conntrack.*` and `gateway.global_prefix`** are accepted and not
  acted on. Upstream does the same: its `src/config/gateway.rs` parses them,
  and only the OpenWrt init script reads `global_prefix`.
- **Packaging.** Debian and RPM ship a separate `fips-gateway` package,
  requiring `fipsd` and the libnftables runtime (Debian derives it with
  dpkg-shlibdeps; Fedora uses `nftables`). Arch follows upstream's single
  package layout: `fipsd` includes the gateway and depends on `nftables`.
  All Linux recipes require the libnftables headers when building.
  `fips-gateway.service` is generated and installed with the executable,
  but is never enabled or started by package installation.

## Install

On Debian/Ubuntu install `fipsd` and `fips-gateway` together (`apt install
./fipsd_*.deb ./fips-gateway_*.deb`); on Fedora use `dnf install fipsd
fips-gateway`; on Arch install `fipsd` (the gateway is included). The gateway
also needs `ip` from iproute2/iproute at runtime. Packages install its unit,
but do not activate it before a gateway configuration exists.

Configure the shared `/etc/fips/fips.yaml` below, set the required sysctls,
and explicitly `sudo systemctl enable --now fips-gateway.service`.
Check `journalctl -u fips-gateway.service` and query `/run/fips/gateway.sock`.
Use `sudo systemctl disable --now fips-gateway.service` before removing it.
A clean stop removes its nft table, pool route, proxy entries and socket;
Debian purge/RPM erase also drops `/run/fips-gateway` restart state.

## Deploy (deploy-gateway.md)

The recipe is upstream's `docs/how-to/deploy-gateway.md`, with `fipsd` as the
daemon. Each step is listed with what the root-only netns smoke
(`fipsd/tests/gateway_netns_smoke.py`) checks for it. The smoke builds the
how-to's topology in three namespaces, using the netns helpers of
`Testing/fips-chaos/chaoslib/net.py`:
- **gateway host:** fipsd A plus fips-gateway, LAN interface `lan0`;
- **mesh node:** fipsd B over a UDP underlay;
- **LAN host:** an unmodified host at `fd02::20`, with HTTP and UDP services.

1. **Daemon prerequisites.** Run fipsd with `tun.enabled: true` and
   `dns.enabled: true` on `[::1]:5354`. It must be linked to the mesh.
   *Smoke:* A's TUN `fips0` and its DNS come up, and the A–B link is up.
2. **Kernel sysctls.** Set `net.ipv6.conf.all.forwarding=1` and
   `net.ipv6.conf.all.proxy_ndp=1` (or the LAN interface's own setting). The
   gateway refuses to start without forwarding and warns without proxy NDP.
3. **Capability.** The gateway needs `CAP_NET_ADMIN`; the unit runs it as root.
4. **Minimum configuration.** These keys are required:
   ```yaml
   gateway:
     enabled: true
     pool: "fd01::/112"
     lan_interface: "eth0"
   ```
   Optional keys: `dns.listen` (`"[::]:53"` when no other resolver holds port
   53), `dns.upstream`, `dns.ttl`, `pool_grace_period`, and `port_forwards[]`.
5. **Start the service.** Use `systemctl enable --now fips-gateway`, or run
   `fips-gateway -c /etc/fips/fips.yaml`.
   *Smoke:* the log shows `Gateway config loaded`, `DNS upstream is
   reachable`, `Created nftables table 'fips_gateway'` and `fips-gateway
   running`.
6. **Pool route.** The gateway adds `local <pool> dev lo` itself.
   *Smoke:* `ip -6 route show table local` shows `local fd01::/112 dev lo`.
7. **nftables.** *Smoke:* `nft list table inet fips_gateway` shows the six
   chains, the set and both forwards.
8. **Outbound.** LAN clients need a route to the pool via the gateway, and a
   resolver that forwards `.fips` to the listener.
   *Smoke:* `dig AAAA <npub>.fips` answers from `fd01::/112`, and `example.com`
   gets `REFUSED`. `ping6`, TCP (HTTP) and UDP to the pool address reach node
   B through the DNAT.
9. **Operate and verify.** Run
   `echo '{"command":"show_gateway"}' | nc -U /run/fips/gateway.sock`, and
   likewise for `show_mappings`.
   *Smoke:* after a tick, `nat_mappings` is 1 and the mapping is `Active` with
   `used: true`.
10. **Inbound.** Add a forward such as
    `port_forwards: [{listen_port: 8080, proto: tcp, target: "[fd02::20]:80"}]`.
    *Smoke:* node B reaches the LAN host's `:80` and `:53` through A's mesh
    address on `:8080/tcp` and `:5353/udp`.
11. **Compose with the mesh firewall.** Load `/etc/fips/fips.nft` (the
    `fips-firewall` baseline) and add a `fips.d/forward/*.nft` drop-in matching
    the *rewritten* target address and port.
    *Smoke:* an allowing drop-in passes the forward, and a refusing one drops
    it (its counter moves).
12. **Stop.** `SIGTERM` exits 0.
    *Smoke:* the table, the route, the proxy NDP entries and the socket are
    gone, and `pool.json` holds the live address. A restart restores the
    holds, and a new name gets a fresh address, not the held one.

Run the smoke as root on Linux, built with the gateway:

```sh
sudo -E env FIPSD_GATEWAY_PRIVILEGED_SMOKE=1 \
  ctest --test-dir <build> -R '^FipsdGatewayPrivilegedSmokeTest$' --output-on-failure
```

It skips (exit 77) without root or the variable. It touches only its own
namespaces, `/run/fips/gateway.sock` and `/var/run/fips-gateway`, and it
removes them, and any `/run/fips` it created, on exit.

## Troubleshoot (troubleshoot-gateway.md)

The log lines are upstream's, word for word, so upstream's recipes apply. With
the C names:

| Symptom (log line) | Check | Fix |
| --- | --- | --- |
| `No gateway section in configuration` / `... is not enabled` | `-c` path; `gateway.enabled` | Add the block with `enabled: true`, `pool`, `lan_interface`. |
| `cannot bind the gateway DNS listener on <addr>: Address already in use` (with the `ss`/`netstat` hints) | `ss -ulpn 'sport = :<port>'` | Move `gateway.dns.listen`, or make the port-53 resolver forward `.fips` to the default `[::1]:5365`. |
| `IPv6 forwarding is disabled` | `sysctl net.ipv6.conf.all.forwarding` | Set it to 1 (persist it in `/etc/sysctl.d`). |
| `Proxy NDP is disabled, so LAN hosts that resolve pool addresses on-link get no answer` (warning) | `sysctl net.ipv6.conf.{all,<lan>}.proxy_ndp` | Set it to 1, or LAN clients cannot resolve virtual IPs on-link. |
| `Waiting for the FIPS daemon's TUN interface` then the 30 s error | `ip link show fips0`; `tun.name` | Start fipsd with the TUN enabled; both must read the same `tun.name`. |
| `DNS upstream did not become reachable` | `dig @::1 -p 5354 <npub>.fips AAAA` | Enable fipsd's DNS on the address in `gateway.dns.upstream` (`[::1]`, not `127.0.0.1`). |
| `Failed to create nftables table — do you have CAP_NET_ADMIN?` | `nft list tables` as the service user | Run as root or with `CAP_NET_ADMIN`; the kernel needs `nf_tables` and nat support. |
| `Failed to bind gateway control socket` | `ls -ld /run/fips` | Start fipsd first (it owns `/run/fips`); a stale socket of a dead gateway is replaced, a live one refuses. |
| `No conntrack source is readable; session pinning is off` | `ls /proc/net/nf_conntrack`; `conntrack -L -f ipv6` | Load `nf_conntrack`, or grant `CAP_NET_ADMIN` for the netlink dump. Until then no mapping counts as used. |
| `Pool refusing new names: reason=...` | `show_gateway` (`pool_refused`, `pool_free`) | `ceiling`: 1000 mappings that have all carried traffic; `exhausted`: widen the pool; `awaiting-mark`: a state write is pending or failing. A query over the new-name rate (a bucket of 50, refilled at 10/s) gets `SERVFAIL` with no warning, as upstream. |
| `Failed to add NAT rules` / `Failed to retry pending NAT rules` | `nft list table inet fips_gateway`; the `error=` text | The kernel's reason. The gateway retries every 10 s. |
| `Gateway NAT worker stopped` / `NAT worker ran out of memory` | journal | The gateway exits 1 for a restart, by design. |
| Ping works, TCP does not | `conntrack -L -f ipv6 -d <virtual_ip>` | Check the masquerade (`oifname fips0`) and that the reply path is not filtered by the mesh firewall. |
| Mesh peer cannot reach `<gw-npub>.fips:<port>` | `nft list chain inet fips_gateway prerouting`; the target listening | Each forward is `iifname fips0` + proto + port, so test from the mesh, not the LAN. With `fips-firewall` loaded, the drop-in must match the rewritten target. |
| `Invalid gateway.port_forwards: ...` at load | the entry | Targets are `[ipv6]:port` (IPv4 refused); `listen_port` must be non-zero and `(listen_port, proto)` unique. |

## Verification

Each test below has its own ctest in `fipsd_tests`. The eight unit suites run on
every platform, with their Linux-only cases compiled in on Linux. None of them
sleeps: waits are condition variables, channel selects, or bounded `poll`s for
an expected datagram.

| Test | Covers |
| --- | --- |
| `FipsdGatewayPoolTest` | All 56 `pool.rs` tests, plus the map reservation the pool relies on. |
| `FipsdGatewayConntrackTest` | Proc and kernel-format vectors; the ctnetlink dump built byte by byte (multi-message, IPv4 skip, `NLMSG_DONE`, `EPERM`, foreign sequence, `SEEN_REPLY`); the reader's fallback and probe; an unreadable file. |
| `FipsdGatewayStateTest` | The state file round trip, tmpfs gating, holds and stretches, carry-over at start. |
| `FipsdGatewayNatTest` | The golden batches (full, empty, cleanup) and each rule shape; pending retries, confirmed removals and reissued addresses; the latches; the worker (order, batching, stop behind changes, timeout, drop, an out-of-memory stop). |
| `FipsdGatewayNetTest` | The sysctl checks, route and proxy NDP command lines and their cleanup, the report latches. |
| `FipsdGatewayDnsTest` | The screen tables, `.fips` and non-`.fips` answers, upstream matching, eviction ordering, listener binds and hints, and the serve loop over loopback. |
| `FipsdGatewayControlTest` | The protocol vectors and both command shapes. |
| `FipsdGatewayShutdownTest` | The stop ordering and the abandonment of an overrunning task. |
| `FipsdGatewayPrivilegedSmokeTest` | Root-only, opt-in: the deployment above, end to end. |
