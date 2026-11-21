# Settlement primitives (partial port)

Source: `core/src/proto/settle/` in jmcorgan/fips at
`64f5b4d482cfe14abad108ffc2d92128f7658311`.

This directory ports **params, links, legs and gates**, not yet the full peer
settlement machine. Input/output vocabulary is declared in sibling headers. Public API: `fips/proto/settle/settle.h`. Test mapping and qualification:
[`tests/SETTLE.md`](../../../tests/SETTLE.md).

- `params.c`: upstream defaults, binary64 backoff/truncation; bounded deadlines
  reuse the shared `fips/host.h` instant operations and `fips_deadline`.
- `links.c`: bounded records/paths, derived-index queries, attribution, capability,
  path migration, shared-connection release and retirement. Queries derive the
  index from records instead of maintaining a second owning collection.
- `legs.c`: dial/answer records, byte-identical resends, failed-send retry,
  retained answers, deadlines, ordered poll/close, and explicit index-lease moves.

The caller supplies storage, time and synchronization. No allocator, clock read,
IO or global mutable state is used. Storage and arguments/outputs must not alias;
initialized owning collections must be drained before reinitialization. Owning
leg/lease values must not be copied or modified by callers. Supplied durations
must be normalized (`nanos < 1000000000`) and instants must share a compatible
host clock. Poll returns borrowed resend bytes until the next mutation; the host
reports send success/failure explicitly. Reaped outputs return owning leases.

C-local limits are caller-selected record/path capacities, transport address
capacity and a 4096-byte transcript bound. Capacity/ID/transcript rejection is
transactional. Poll conservatively preflights `1 + path_count` close slots per
reaped link; this can exceed the number of actual deduplicated closes.

- `gates.c`: all six restart sources, floors, replay/silent/redial memory,
  dampener stamps, refused counts and identity expiry. Caller-selected identity
  capacity and 64-entry per-identity ephemeral bounds fail transactionally.

Not yet ported: peer arbitration table, join policy,
parity schedules, mutation switches, model history/judge and explorers/simulators.
Test-only Rust replant/naming/drive helpers and the shell-specific `reap_one`
entrypoint are not exposed. Those are **not** replaced by a fake judge or by the
Rust primitive differential oracle. Remaining core work: `nostrc-f12v2.8`.
No fipsd node/peer wiring is included.

`upstream/` contains byte-for-byte snapshots of the upstream README and three
spec files at the pin above. They describe the **whole upstream implementation**,
not a claim that those rules are implemented here. Upstream source and specs
remain authoritative; the C subset boundary above is deliberate.
