# Settlement core (port in progress)

Source: `core/src/proto/settle/` in jmcorgan/fips at
`64f5b4d482cfe14abad108ffc2d92128f7658311`.

This directory ports **params, links, legs, gates and table arbitration**.
Public headers are under `fips/proto/settle/`: `settle.h`, `gates.h`,
`input.h`, `output.h` and `table.h`. The independent test-only history/judge
is under `model/`; it uses no production settlement types or algorithms. Test mapping and qualification:
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

- `table.c`: admitted-msg1/answer protocol, connecting/dial lifecycle, crossing
  tie-break, redial slots and proof-driven swaps, caps, ordered effects, path
  attribution/moves, restart/removal and connection keep rules. Two disjoint
  caller-owned arenas checkpoint fallible operations and swap only after success.
  Infallible per-frame `Heard` reports update directly without copying arenas. A failure preserves
  the live state, previous output list and input lease. Table outputs copy byte
  payloads; Install/Free leases must be consumed before another mutation.

- `model/`: independent abstract history, judgment and query APIs; 55 upstream
  model test cases plus 13 gate/model histories are replayed in C. Only compiled
  with `FIPS_TEST_HOOKS`; the default library contains no model implementation.

Not yet ported: join policy, parity schedules, mutation switches and
explorers/simulators. Join requires the D1-owned rekey redial API
(`Arrival.heard`, `RedialRequested`, `note_redial`), absent from this branch but
now available in D1 commit `d45c2f239`. Importing that dependency awaits
orchestration approval; no adapter stub is supplied.
Test-only Rust replant/naming/drive helpers are not exposed. The production
shell adapters `reap_leg`, `open_link`, `drop_leg`, `unadopt` and refused-count
writers are provided. The test-only helpers/explorers are **not** claimed as covered simply because
the production table and independent model have differential tests. Remaining core work: `nostrc-f12v2.8`.
No fipsd node/peer wiring is included.

`upstream/` contains byte-for-byte snapshots of the upstream README and three
spec files at the pin above. They describe the **whole upstream implementation**,
not a claim that those rules are implemented here. Upstream source and specs
remain authoritative; the C subset boundary above is deliberate.
