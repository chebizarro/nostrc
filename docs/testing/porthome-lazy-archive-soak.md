# Lazy `~/Archive` soak — operator runbook

**Scope:** Phase 4 exit criterion (`nostrc-h10m.8`). This is a read-only
`nostr-home-fuse` projection over an ordinary, writable home. It is not the
legacy `nostrfs@<user>` mount of `/home/<user>`; replacing the real home would
violate the Phase 4 design. The sync daemon remains the sole snapshot writer.
The FUSE process consumes its existing `snapshot.json` format unchanged.

## Preconditions and setup on a disposable GNOME account

1. Build/install `nostr-homed` with
   `NOSTR_HOMED_ENABLE_PORTHOME_EXPERIMENTAL=ON`,
   `NOSTR_HOMED_ENABLE_SYNCD_EXPERIMENTAL=ON`, and
   `NOSTR_HOMED_ENABLE_PORTHOME_FUSE_EXPERIMENTAL=ON`. Install the user
   `nostr-home-sync.service` and `nostr-home-fuse.service` units. Do not run
   this on a developer's live home. Keep a separate real-home file such as
   `$HOME/local-sentinel.txt` and verify it remains writable throughout.
2. On the **source** account, mark `Archive` before its next syncd push:
   write `Archive` as one line in `~/.config/nostr-homed/lazy` (or set
   `NOSTR_HOMED_SYNCD_LAZY_SUBTREES=Archive` for syncd). Publish a new
   personal-home manifest. Decode it on a disposable test account and
   confirm the schema-v2 `Archive` directory has `lazy=true` and its
   descendants still have chunk addresses. The marker is on-wire; the
   destination need not repeat the local policy. Old unmarked manifests
   remain eager. A local destination policy can only add lazy suppression,
   never clear an on-wire marker. Private-service vaults are excluded from
   this personal manifest and use the syncd prefetch cache only.
3. On the **destination**, use a fresh disposable home or an existing
   empty `$HOME/Archive` directory. Never move or discard user data merely
   to pass the mount check. Install this user-service drop-in:

   ```sh
   mkdir -p "$HOME/.config/systemd/user/nostr-home-fuse.service.d"
   cat > "$HOME/.config/systemd/user/nostr-home-fuse.service.d/archive.conf" <<'EOF'
   [Service]
   Environment=NH_FUSE_MOUNTPOINT=%h/Archive
   Environment=NH_FUSE_SUBTREE=Archive
   EOF
   systemctl --user daemon-reload
   ```

   A fresh provisioner creates the marked Archive as an empty anchor,
   skips its descendants, and the production fetch helper runs the v2 rename
   walk. The tested syncd reconciler records descendants and chunk references
   in `snapshot.json` without downloading bodies. **Current integration
   limit:** `nostr-home-syncd` does not register its live pointer subscription
   or pull context, and provisioning does not write that snapshot. Therefore
   the fresh-login production path does not yet supply FUSE's snapshot by
   itself. Do not start a seven-day acceptance run until that source-bound
   pull/seed integration is implemented and exercised end to end. A missing
   snapshot is a visible startup failure, not an empty successful mount.
   A pre-existing nonempty Archive is never shadowed. Marked cold rows are
   excluded from syncd fallback-rescan delete sweeps, including while the
   FUSE mount hides the underlying empty directory.
4. Log into GNOME through the normal broker/PAM path so the one-shot
   `home_seed.fuse` is present. In that user's graphical session, run:

   ```sh
   systemctl --user start nostr-home-sync.service
   systemctl --user start nostr-home-fuse.service
   systemctl --user is-active nostr-home-fuse.service
   findmnt -n -t fuse.porthome --mountpoint "$HOME/Archive"
   test -w "$HOME/local-sentinel.txt"
   test -f "$HOME/Archive/<known-cold-file>"
   ```

   The unit's `ExecStartPost` checks the configured path with `findmnt`.
   `nh_open_session`'s legacy mount verification checks `/home/<user>` for
   `nostrfs@`; it cannot be reused for an overlay *inside* the real home.
   The daemon owns teardown (`auto_unmount` handles crashes); the unit has no
   unconditional unmount hook that could detach another process's mount.
   A missing subtree
   or occupied mountpoint fails startup rather than showing an empty or
   shadowed Archive.

## Seven-day GNOME run

Run the standard-library collector **inside the active GNOME session**. Use
one known immutable, nonsecret sample file in the Archive snapshot. Compute
its expected SHA-256 from the source before the run. Keep the report outside
`~/Archive`; the collector refuses to overwrite prior evidence.

```sh
FUSE_PID=$(systemctl --user show -p MainPID --value nostr-home-fuse.service)
SESSION_PID=$(pgrep -u "$(id -u)" -o gnome-session-binary)
python3 gnome/nostr-homed/tests/integration/soak_lazy_archive.py \
  --label gnome --gio --session-pid "$SESSION_PID" --pid "$FUSE_PID" \
  --mountpoint "$HOME/Archive" --sample '<relative-cold-file>' \
  --sha256 '<64-lowercase-hex>' --duration-seconds 604800 \
  --interval-seconds 60 --read-timeout-seconds 20 \
  --cache-dir "${XDG_CACHE_HOME:-$HOME/.cache}/nostr-homed/blobs" \
  --status "${XDG_RUNTIME_DIR}/nostr-homed/fuse-status.json" \
  --report "$HOME/.local/state/nostr-homed/archive-soak-7d.json"
```

The collector drives GIO metadata lookup, directory enumeration and a full
read/hash in a bounded subprocess on each cycle. It records per-probe latency,
read/hash failures, timeouts (hangs), missing mount/process/session, FUSE
status counters, cache bytes, and FUSE RSS. Its JSON summary includes p50/p95/
p99/max read latency and cache start/end/max. The JSONL is the durable sample
stream. A completed report with all probes passing is still not enough alone:
retain `journalctl --user -u nostr-home-fuse.service -u nostr-home-sync.service
--since <start> --until <end>` and GNOME session crash logs, inspect any
restarts, and report interruptions rather than aggregating them away. The
collector exits nonzero on any failed probe or incomplete duration.

During the run, verify the *first* cold file access increments tier-3 fetches,
subsequent access is served from memory/disk/kernel cache, and cache growth
stays within the configured quota. For an offline check, stop access to the
test Blossom endpoint only after one file is cached. A cached file should
still read; an uncached file should fail promptly with an error, not hang or
return invented bytes. Restore connectivity, retry, and record recovery.
Do not inject this fault into a shared production service. Private-service
vault subtrees are a separate cache-only path: syncd prefetches them, and
FUSE must not initiate authenticated GETs (§10.1 shared-vault deviation).

The owner satisfies the Phase 4 exit criterion only after **604800 elapsed
seconds in one real GNOME session** with a lazy Archive, a completed report,
logs, and findings. A container/VM headless run or a shortened interval is
not equivalent. Keep `nostrc-h10m.8` open until that evidence exists.

## Reproduce a bounded headless mount (not GNOME acceptance)

Use a disposable Linux VM/container with `/dev/fuse`, FUSE 3 and the three
experimental build options above. Replace `BUILD` with its CMake build tree.
The fixture publisher below writes a syncd-format snapshot directly; it does
**not** prove the missing production pull registration. Keep generated seeds
and publisher output in private scratch, and remove the scratch after recording
only nonsecret summary metrics.

```sh
set -eu
BUILD=/path/to/build
D=$(mktemp -d)
chmod 700 "$D"
cleanup() {
  if [ -n "${FUSE_PID:-}" ]; then kill "$FUSE_PID" 2>/dev/null || true; wait "$FUSE_PID" 2>/dev/null || true; fi
  if [ -n "${BLOSSOM_PID:-}" ]; then kill "$BLOSSOM_PID" 2>/dev/null || true; wait "$BLOSSOM_PID" 2>/dev/null || true; fi
}
trap cleanup EXIT
mkdir -p "$D/fixture/Archive/2024" "$D/home/Archive" "$D/state" \
         "$D/cache" "$D/status" "$D/blobs"
printf 'cold archive payload\n' > "$D/fixture/Archive/2024/report.txt"
printf 'real-home sentinel\n' > "$D/home/local.txt"
SEED=$(openssl rand -hex 32)
NSEC=$(openssl rand -hex 32)   # disposable test identity only
FAKE_BLOSSOM_PORT=18081 FAKE_BLOSSOM_DIR="$D/blobs" \
  python3 gnome/nostr-homed/tests/integration/fake_porthome_blossom.py \
  > "$D/blossom.log" 2>&1 & BLOSSOM_PID=$!
# Wait for the local test server before publishing; fail if it never starts.
for i in 1 2 3 4 5; do
  if python3 -c 'import socket; socket.create_connection(("127.0.0.1",18081),1).close()' 2>/dev/null; then break; fi
  sleep 1
done
NH_PORTHOME_ALLOW_INSECURE=1 "$BUILD/gnome/nostr-homed/porthome-fuse-fixture" \
  publish --fixture-dir "$D/fixture" --state-dir "$D/state" \
  --seed-hex "$SEED" --nsec-hex "$NSEC" \
  --blossom http://127.0.0.1:18081 --skip-publish-pointer \
  > "$D/fixture.log" 2>&1
# Model the reconciler's lazy-directory row in this synthetic snapshot.
python3 - "$D/state/snapshot.json" <<'PY'
import json, sys
path = sys.argv[1]
with open(path) as source:
    snapshot = json.load(source)
snapshot['files']['Archive']['lazy'] = True
with open(path, 'w') as target:
    json.dump(snapshot, target)
PY
NH_FUSE_SEED_HEX="$SEED" NH_FUSE_HOME="$D/home" HOME="$D/home" \
NH_FUSE_STATE_DIR="$D/state" NH_FUSE_MOUNTPOINT="$D/home/Archive" \
NH_FUSE_SUBTREE=Archive NH_FUSE_STATUS_DIR="$D/status" \
XDG_CACHE_HOME="$D/cache" NH_FUSE_CHUNK_SIZE=262144 \
NH_PORTHOME_ALLOW_INSECURE=1 NH_FUSE_BLOSSOM=http://127.0.0.1:18081 \
  "$BUILD/gnome/nostr-homed/nostr-home-fuse" > "$D/fuse.log" 2>&1 & FUSE_PID=$!
for i in 1 2 3 4 5; do
  if findmnt -n -t fuse.porthome --mountpoint "$D/home/Archive"; then break; fi
  sleep 1
done
findmnt -n -t fuse.porthome --mountpoint "$D/home/Archive" || exit 1
SHA=$(sha256sum "$D/fixture/Archive/2024/report.txt" | cut -d' ' -f1)
python3 gnome/nostr-homed/tests/integration/soak_lazy_archive.py \
  --label headless-container --pid "$FUSE_PID" \
  --mountpoint "$D/home/Archive" --sample 2024/report.txt --sha256 "$SHA" \
  --duration-seconds 300 --interval-seconds 15 --read-timeout-seconds 20 \
  --cache-dir "$D/cache/nostr-homed/blobs" \
  --status "$D/status/fuse-status.json" --report "$D/soak.json"
# Inspect $D/soak.json and $D/soak.jsonl, then remove task scratch.
# The EXIT trap stops only the two processes created by this recipe.
```

Do not call the fixture's static snapshot a published on-wire manifest. For
that, use the source push/manifest and fresh provision/reconcile tests or a
real authenticated pull after the integration limit above is resolved.

## Shortened local evidence (2026-10-28)

A disposable Ubuntu 24.04 arm64 Docker container with `/dev/fuse` and
`CAP_SYS_ADMIN` was used as the closest available headless approximation.
It used a real `fuse.porthome` mount under a real test home, a local fake
Blossom BUD-01 test server, the existing encrypted fixture publisher, and
a syncd-schema snapshot. No GNOME login, broker, relay, provisioner or
production service was exercised. The original one-hour run started before
the on-wire marker edits and used a fixture snapshot, not a verified relay
pull. It completed 3600.08 seconds with 121/121 read/hash probes passing,
zero hangs, read/hash errors, mount/process losses or replacements. Read
latency was p50 43.320 ms, p95 65.385 ms, p99 70.723 ms, max 101.523 ms;
cache grew from zero to 262,173 bytes and then stayed there, peak FUSE RSS
11,076 KiB, tier 3 fetched once. A second five-minute run used the rebuilt FUSE
binary and a synthetic snapshot `Archive.lazy=true` row: 300.03 elapsed
seconds, 21/21 read/hash probes passing, zero hangs, mount/process losses or
read errors; read latency p50 30.517 ms, p95 46.590 ms, max 57.180 ms;
cache stayed at 50 bytes, peak FUSE RSS 10,672 KiB, tier0 disabled. The
initial smoke read populated tier 3 once. Restarting that mount with no
Blossom endpoint read the cached report and returned `EIO` for an uncached
binary in 0.001 seconds. This is diagnostic container evidence, not GNOME,
relay, hosted, native desktop or release acceptance. After rebasing onto
merged GC, the real `nh_syncd_gc.c` daemon integration test passed: an old
marked Archive snapshot produced a generation-2 pin; GC kept its blob while
that slot was retained, and a dry-run counterfactual classified it for
`would-delete` only after that slot was removed. The private-vault FUSE
source harness also passed with NIP-42/NIP-11 enabled; it is not a live
private-service network proof. A post-rebase two-minute real-mount probe
completed in 120.05 seconds with 13/13 read/hash probes passing, no hangs,
errors or mount/process loss, p50 36.261 ms, p95 47.270 ms, max 79.475 ms,
and stable 50-byte cache. It reused a cached file; no new network fetch was
attempted. Final post-rebase focused CTest selection passed 16/16, including
the real GC integration and the private-vault source harness. Secrets and
container scratch are removed after measurement.
