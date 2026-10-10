# Disposable Bahia epoch interop

`signet/tests/interop/run_live_epoch_bahia.py` is an opt-in **live** NIP-46
interop runner. It starts a `nak serve` relay bound to `127.0.0.1`, one real
`signetd`, and uses the provisioner-authenticated `signetctl` management path
to create a synthetic service identity and acquire its writer lease twice.
It then runs Bahia's `signetinterop` Go test with a private mode-0600 fixture.
All keys, the SQLCipher database, logs, relay, and daemon are disposable and
removed/stopped on exit. No production key, endpoint, or existing DB is an
input. The owner and provisioner are distinct synthetic keys. `signetctl
writer-acquire` does not grant the writer management authority; Signet's
existing management authorization and transactional custody checks remain the
authority.

From this exact Signet worktree:

```sh
cmake -S . -B _build -DBUILD_APPS=OFF -DWITH_NOSTRDB=OFF \
  -DLIBNOSTR_WITH_NOSTRDB=OFF -DSIGNET_ENABLE_PASSKEYS=OFF
cmake --build _build --target signetd signetctl
python3 signet/tests/interop/run_live_epoch_bahia.py \
  --build-dir _build --bahia /absolute/path/to/bahia-worktree
```

Prerequisites: `nak` with `nak serve`, Go, CMake, the Signet build dependencies,
and a Bahia checkout containing
`internal/adapters/signet/epoch_interop_integration_test.go` whose expected
Signet commit is this worktree's committed HEAD. The runner rebuilds daemon
and CLI from the worktree before execution. If the Bahia test pins an older
commit, update that **on the Bahia branch**; do not forge the fixture commit.
The test is not registered in default CTest because it depends on another
repository and a local relay executable. It never prints the pairing URI or
secret-bearing test output; on failure, raw logs are withheld.

A passing local test proves only the tested synthetic NIP-46 epoch NIP-44 and
SBOM DSSE operations through this daemon and relay. It is not a deployment,
proof of old-daemon quiescence, or authorization to cut over an existing key.
See `WRITER_EPOCH_CUTOVER.md` for the supported single-active-store topology.
