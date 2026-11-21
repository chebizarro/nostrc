# Independent settlement history and judge

Port of `core/src/proto/settle/model/{history,judge}.rs` at
`64f5b4d482cfe14abad108ffc2d92128f7658311`.

This is a separate abstract state machine, not a wrapper around the production
table or its gates. It imports only its own history types and fundamental host
status/export types. It calls no production settlement, rekey or path helper.
Addresses, handshakes, message bytes, epochs and link IDs are abstract names;
no networking or cryptographic acceptance is inferred from model execution.

Like upstream's test/test-hooks model, these sources are compiled only into
test executables with `FIPS_TEST_HOOKS`, not into the default `fips_core` archive.
Headers: `fips/proto/settle/model/history.h` and `model/judge.h`.

The caller owns a copyable value containing fixed arrays (128 entries per
attempt/leg/closed/record vector). A run accepts at most 128 events. Capacity or
clock-overflow errors leave the judge and outputs unchanged. These are C-local
bounds, not changes to protocol rules. Step outputs close at most one abstract
address, as each upstream decision does. `needs` uses caller-owned output
storage. Histories advance their own millisecond clock; there are no clock
reads, allocators, callbacks, IO, mutable globals or shared algorithm helpers.

All 55 upstream model test assertion bodies execute in the Rust oracle, and
C replays every event, judgment and complete public state, including `settled`
and `needs`. The original 13 gate histories additionally compare the C gates
against this independent C judge at each applicable gate decision and stamp.
Native tests cover independent defaults, bounds, atomic run/step failure,
clock overflow and query-output preflight. See `tests/SETTLE.md` for mapping.
