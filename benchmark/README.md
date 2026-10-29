# Relay performance baseline

Build `relay_perf_baseline` with `-DBUILD_BENCHMARKS=ON`. Relay URLs are required;
there are no implicit network targets:

```sh
./build/benchmark/relay_perf_baseline --timeout-ms 1000 ws://127.0.0.1:8080
```

`--timeout-ms` (default 10000) bounds each handshake wait and, separately, each
subscription's wait for EOSE. It is a failure deadline, never a completion signal.
The bounded subscription requests kind 1 with limit 200; it does not publish.

Each relay reports `completed`, `closed` (including the relay's reason),
`transport_loss`, `timeout`, `connect_failure`, or an internal `setup_failure`.
Only a consumed EOSE produces `eose_ms` and an aggregate EOSE sample. An empty
EOSE is a completed measurement. Failed measurements contribute no EOSE timings;
when there are no samples, those timing fields are absent, not zero. Aggregate
results are also written to `baseline_metrics.txt` in the working directory.

Timings use the monotonic clock at the consumer's handshake/channel observation,
not server timestamps. EVENT and EOSE channels are independent: buffered events
are drained on completion, so the first EVENT can be consumed after EOSE. Its
reported timestamp is its actual consumption time, not a derived EOSE time.

Exit codes: **0** only if every requested relay completes; **1** if any relay
fails, including when none complete; **2** for invalid or missing arguments.

With `BUILD_TESTING=ON` too, `ctest -R '^bench_relay_perf_baseline$'` exercises
these outcomes against a loopback-only Python relay. It contacts no public relays.
