# Upstream control fixtures

The 24 JSON files are byte-for-byte copies of `src/control/snapshots/*.json`
from FIPS commit `6aa80504baa0fa312b392009eea0e3e59fb6fe4c`
(<https://github.com/jmcorgan/fips>), the original compatibility baseline.
The current pin is in `libfips-core/UPSTREAM.md`; these capture origins are
retained rather than relabelled. Its wave26 triage tracks new metrics separately.
They are test oracles, not runtime response templates. See upstream
`src/control/queries.rs` snapshot module, `src/control/probe.rs` and
`src/control/tests/compose.rs` for independent fixture construction.

Classification: 19 base daemon response snapshots (all default-build queries
except `show_metrics`), one nonempty transport-interface response, two CLIENT
status composition/fallback responses, one standalone probe report, and one
catalog of three CLIENT path requests. The latter four categories must not
be misrepresented as new daemon commands. No fixture is loaded into a provider.
The C tests construct the identity from `[0xAB;32]`, typed initial node state,
empty entity/history views, an absent-interface transport, and core probe
stage records independently of the expected files. The path catalog is fed
as requests to a recording live provider and its routing/parameters checked.

Comparison is byte-for-byte AFTER upstream's documented volatile-field
redaction and compact JSON canonicalization: lexical nested object keys;
Response envelopes `status,data` / `status,message` in serde struct order.
The stored fixtures have `data,status` order because upstream wraps them in
`serde_json::Value` before pretty-printing; that outer-only distinction is
normalized explicitly. We do not use parsed-tree equality as the assertion.
Standalone report maps are lexical. Client composition is checked only in
tests: the daemon itself must omit adapter-owned TUN fields.

`control/counter_schema.h` pins zero-counter field names from these contracts,
not golden values or serialized responses. Providers supply actual counters;
the empty constructor's counters start at zero. No data-plane activity is
claimed by the seed views.
