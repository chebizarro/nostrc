# nostrdb downstream patches

`third_party/nostrdb` is a git submodule of
[damus-io/nostrdb](https://github.com/damus-io/nostrdb), pinned at
`2d459e8fa9ba0af5af091c37920e5d2078d881ca`. Its bundled LMDB is 0.9.31.
`components/nostrdb` is nostrc's adapter, not another vendor copy.
The existing hex/talstr CMake compatibility headers are separate from this queue.

The root CMake build uses `cmake/NostrdbPatches.cmake` for both `nostrdb`
(libnostr backend) and `nostrdb_vendored` (private storage backend). It copies
`src/` and `deps/lmdb/` into the build directory, applies these patches with
`patch -f -F 0 -p1`, and compiles those copies. A `patch` executable is required.
The submodule, including its signed test fixtures, is never modified. Original
inputs and patches are configure dependencies. Patch failure stops configure;
a vendor update must reconcile or retire the affected patch, not disable it.

- **0001:** skip empty-value copies in LMDB insertion and same-size overwrite.
  nostrdb's full-text index legitimately passes `{mv_size=0, mv_data=NULL}`;
  calling C `memcpy` with that NULL pointer is undefined even at length zero.
  Nonempty values and `MDB_RESERVE` retain their existing behavior.
- **0002:** load tag-key timestamps and tag-index note IDs with `memcpy`.
  Variable-length tag keys and LMDB's byte-addressed values do not promise
  `uint64_t` alignment. The representation and numeric ordering are unchanged.
- **0003:** decode varint payload bits as unsigned before shifting. The full-text
  query's `UINT64_MAX` bound reaches bit 63, which a signed left shift cannot
  represent. The wire/storage encoding is unchanged.

## Upstream status

These are downstream-only patches for **nostrc-wktgk**, not yet submitted or
accepted upstream. Source checked on 2026-10-26:

- nostrdb master `12398c260ec061fe2db2b696d841edb893ec239b` still contains the
  direct timestamp dereferences and signed varint shift:
  [nostrdb.c](https://github.com/damus-io/nostrdb/blob/12398c260ec061fe2db2b696d841edb893ec239b/src/nostrdb.c),
  [cursor.h](https://github.com/damus-io/nostrdb/blob/12398c260ec061fe2db2b696d841edb893ec239b/src/cursor.h).
- LMDB `mdb.master` `74dd6c3ec648c31857fe0a100f11c2a34619e2d7` still contains
  the unconditional empty-value copies:
  [mdb.c](https://github.com/LMDB/lmdb/blob/74dd6c3ec648c31857fe0a100f11c2a34619e2d7/libraries/liblmdb/mdb.c).

## Focused verification

Configure a Linux Debug build with `GNOSTR_ENABLE_ASAN=ON` and
`GNOSTR_ENABLE_UBSAN=ON`. Build `test_nostrdb_vendor_ub`,
`test_relay_req_contract`, `test_session_relay_storage`, and
`nostr-session-relayd`, then run:

```sh
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir "$BUILD" --output-on-failure \
  -R '^(nostrdb_vendor_ub|relayd_req_contract|relayd_session_relay_storage)$'
```

The regression covers empty-value insertion, overwrite and shrink; full-width
varint round trips; and ingestion/reopen/query of tag values at all eight length
residues, with timestamp ordering assertions. Signature checking is intentionally
outside that structural regression; the relayd tests exercise signed ingestion.

nostrdb's native `test.c` can be linked against the patched CMake archive and
its normal dependencies, with the same sanitizers; run it from a disposable
copy of the vendor tree because its tests write databases under `testdata/`.
Actual execution results and the separate native-suite blockers are in the bead.
