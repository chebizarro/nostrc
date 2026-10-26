# nostrdb downstream patches

`third_party/nostrdb` is a git submodule of
[damus-io/nostrdb](https://github.com/damus-io/nostrdb), pinned at
`2d459e8fa9ba0af5af091c37920e5d2078d881ca`. Its bundled LMDB is 0.9.31.
`components/nostrdb` is nostrc's adapter, not another vendor copy.
The existing hex/talstr CMake compatibility headers are separate from this queue.

The root CMake build uses `cmake/NostrdbPatches.cmake` for both `nostrdb`
(libnostr backend) and `nostrdb_vendored` (private storage backend). It copies
`src/`, `deps/lmdb/`, CCAN's SHA256 directory, and the native test source into
the build directory, applies these patches with
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
- **0004:** copy SHA256 input blocks into aligned local words before transforming
  them. `HAVE_UNALIGNED_ACCESS` describes hardware, not permission for an
  unaligned typed C load. Digest bytes and the public API are unchanged.
- **0005:** align JSMN token storage within the JSON parser scratch buffer and
  round its end down to a whole token. Giftwrap decoding can subdivide scratch
  at an arbitrary byte offset; the parser must align its own typed storage.
- **0006:** parse the note kind into a local integer, then use the packed-note
  setter. Passing a pointer to the packed member to an ordinary integer parser
  loses its alignment information and permits an unaligned typed store.
- **0007:** calculate a note ID into an aligned SHA256 result, then copy its bytes
  to the requested ID buffer. Packed giftwrap/PNS notes need not align their ID
  member as a `struct sha256`.
- **0008 (test only):** pass `strlen(ok_note)` for the replay test's `ok_note`,
  rather than the longer `bad_note` length. Event bytes and the replay assertions
  are unchanged; the test no longer reads past its string literal.
- **0009:** copy the encoded content block's 16-bit length into its byte buffer,
  rather than storing through a potentially unaligned `uint16_t *`.
- **0010:** load tag-index note keys with `memcpy` when rebuilding reaction,
  reply and repost metadata, just as in the tag-query path.

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
varint round trips; SHA256 known-answer hashes and JSON parsing at all eight
buffer offsets;
and ingestion/reopen/query of tag values at all eight length
residues, with timestamp ordering assertions. Signature checking is intentionally
outside that structural regression; the relayd tests exercise signed ingestion.

nostrdb's patched native `test.c` is generated at
`$BUILD/nostrdb-patched/<target>/test.c`. Link that source against the matching
patched CMake archive and its normal dependencies, with the same sanitizers.
Use the overlay's `src/` include directory so its inline helpers are patched too.
Run from a disposable copy of the vendor tree because the tests write databases
under `testdata/`; never run them against the original submodule's fixtures.
Actual execution results and the separate native-suite blockers are in the bead.
