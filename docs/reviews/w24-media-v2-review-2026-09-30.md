# W24 review: slice G, MIP-04 encrypted-media-v2, group image codecs, gh-mls-media (nostrc-u7cb)

- **Reviewer:** independent peer reviewer (AGENTS.md "Peer Review")
- **Branch reviewed:** `marmot/w24-media-v2` at `ca34bbf4`, one commit on `82a615e4` (master)
- **Review branch:** `review/w24-media-v2` (this document only)
- **Date:** 2026-09-30
- **Verdict:** **CHANGES-REQUIRED** (small). The cryptographic core is right, and I checked it independently down to the byte. Two cheap fixes are needed before merge:
  - **M1:** the commit breaks libmarmot's meson build.
  - **M2:** the new Groundhog seal API drops the "this file may still carry metadata" signal, and its header claims metadata is removed for every type.
  - **Before nostrc-m6tp wires anything:** **M3** needs a decision. The 0x8007 decoder refuses 12 of 20 ordinary URLs that MDK stores as canonical state, and about a third of a 29k-URL differential corpus. A policy is proposed below.
  - Everything else is Low or Nit.

**Commit**

| Commit | Bead | Change |
|---|---|---|
| `ca34bbf4` | nostrc-u7cb | libmarmot 0.12.0 changes: `marmot-media.h` (v2 seal/open, imeta build/parse, MIME canonicalization), `MarmotMessageResult.app_msg.epoch`, 0x8002/0x8007 codecs, legacy `marmot_encrypt_media()` refused. Also the MDK v0.11.0 fixtures and generator patch, groundhog 0.12.0 `groundhog-mls-media`, and marmot-gobject 1.5.0 deprecating `encrypt_media_async` |

No code or beads were changed. Scratch work lived in `/tmp/rv-w24-scratch`:
- the Python re-derivation;
- C harnesses against an ASAN build in `/tmp/rv-w24-asan`;
- a Rust checker pinned to MDK's `Cargo.lock`;
- a regenerated MDK tree in `/tmp/rv-w24-mdk-gen`.

The mutations were applied one at a time in the review worktree and reverted. The tree is clean.

The sanitizer gate reused its existing shared volumes. No Docker volume was created.

## Summary by focus area

### 1. Spec conformance: holds, independently re-derived

- **Key derivation.** Key info, AAD and imeta order match `features/encrypted-media.md` at `07da8ffb` and MDK v0.11.0 byte for byte:
  - key info is `"encrypted-media-v2"‖0‖sha256(pt)‖0‖m‖0‖filename‖0‖"key"`, with HKDF-SHA256 Expand-only on the media secret;
  - the AAD is the same bytes without `‖0‖"key"`;
  - the imeta field order is `v`, `locator`s, `ciphertext_sha256`, `plaintext_sha256`, `nonce`, `m`, `filename`, then `dim`/`thumbhash`.
- **Parser parity.** `marmot_media_imeta_parse()` matches MDK's `parse_media_attachment()` rule for rule. Both judge the version first, reject a keyless known field, reject duplicates and `blurhash`, require non-empty values, and accept either hex case. The one exception is URL parsing (L7).
- **Independent re-derivation.** I wrote it in pure Python: ChaCha20-Poly1305, HKDF-Expand and RFC 9420 MLS-Exporter. Before use I self-tested each against RFC 8439 §2.8.2 and the RFC 9420 key-schedule exporter vector. It reproduces:
  - all three positive cases (key info, file key, AAD, ciphertext, ciphertext hash and the full imeta tag);
  - the MDK unit key;
  - all eight negative verdicts, under the spec receive order;
  - the 0x8002 image AEAD and component bytes.
- **The exporter step, which the fixtures skip.** With exporter_secret `00..1f`, `MLS-Exporter("marmot","encrypted-media",32)` = `73bd647f…6635`. libmarmot's `marmot_media_secret_from_exporter()` returns exactly that.
- **End to end.** I seeded exporter_secret `00..1f` at epoch 7 and called the public `marmot_media_encrypt()` with `" Image/JPG; x=y"` and `"réview.jpg"`. My independent implementation opened the output.
- **Provenance.** A pristine MDK v0.11.0 (`946e0547`) with the committed patch regenerates `media-v2-mdk-v0.11.0.json` **byte-identically** (sha256 `a1232fd7…`). `imeta-v2.json` is MDK's file, unchanged (`fbab0c8d…`).
- **0x8002 MIME note:** see N1.

### 2. Decryption order, zeroization and source epoch: hold

- **Receive order.** `marmot_media_v2_open()` (`media.c:304-362`) validates the reference, compares the ciphertext SHA-256 (`sodium_memcmp`), then opens the AEAD, then checks the plaintext SHA-256.
  - libsodium verifies the tag before it decrypts, and zero-fills the output on failure.
  - On a plaintext-hash mismatch the buffer is wiped before `free`.
  - The file key, media secret and exporter copy are wiped on every path.
  - Mutations confirm both the order and the binding: skipping the ciphertext-hash check is caught, and so is dropping the filename from the AAD.
  - One small gap: the HKDF helper leaves a stack copy of the key (N3).
- **Source epoch.** The epoch is not in the imeta. `app_msg.epoch` is `used_epoch`, the epoch whose secret opened the kind-445 layer.
  - The MLS layer then only runs when that epoch is current (`mls_group.epoch == used_epoch`) or the retained parent (`used_epoch + 1`, `messages.c:987-1035`).
  - So a sender cannot steer a receiver to any epoch other than the one its PrivateMessage authenticated in.
  - A late message reports the parent epoch. `test_media_source_epoch_of_late_message` pins this, and a mutation to "report the current epoch" is caught.
  - Old exporter secrets are never pruned in normal operation, so retained epochs decrypt.
  - Only two paths still let a sender pick the epoch:
    - the retained legacy reader (L5);
    - the explicit `allow_legacy_raw_messages` opt-in, where nothing is MLS-authenticated anyway.

### 3. ABI: in-tree is right, out-of-tree is not protected

- **Layout change.** On LP64, `commit.updated_group` moves from offset 24 to 32. The structs are sequential, not a union.
- **In-tree callers are fine.** They are libmarmot's own code and tests, `marmot-gobject-client.c`, `gh-mls-service.c` and `test_store_marmot.c`.
  - All are built from the same tree, and all use zeroing or designated initializers. None is positional or sized.
  - All 42 targeted tests and the sanitizer gate pass.
- **Not protected:** the SONAME does not change (L2), and marmot-gobject re-exports the layout (L1).

### 4. URL avatar 0x8007: fail-closed is safe in one direction only

- **Producing is safe.** Nothing libmarmot stores is rejected by MDK.
- **Decoding refuses honest MDK state.** 12 of 20 ordinary URLs fail, including IDN avatars, and so do 34% of a mutation-derived corpus. This is harmless today, because nothing calls the decoder. It forks groups the day it gates Commits. See M3 for the data and the recommended policy.

### 5. Groundhog `gh-mls-media`: mostly holds

- **Network.** Every byte goes through `GhBlossomClient` and so through GhNetHttp. That brings the network mode, Tor (a fresh circuit per request), https-only, no redirects, throwaway upload keys, download size caps and public-only download hosts.
- **Locators.** The upload's locator is `<server>/<sha256>`, built locally, never a server-chosen URL.
- **Native files only.** Enforced before any I/O (a mutation is caught). The inherited caveats are in L6.
- **Stripping.** JPEG/PNG metadata really is stripped before sealing (a mutation is caught), but the flag for every other type is lost (M2).
- **Epoch changes.** `gh_mls_media_check_epoch()` returns EPOCH_CHANGED so the caller seals and uploads again (tested, mutation caught). It reads an unreconciled record (L3).
- **Decrypted plaintext** is not wiped (L4).

### 6. marmot-gobject and gnostr

- **Disabling gnostr's MLS media send is acceptable, and nothing needs surfacing, because users never had it.**
  - The attach button is created hidden (`apps/gnostr/plugins/mls-groups/ui/gn-group-composer.c:149`).
  - Nothing calls `gn_group_composer_set_media_enabled()` or connects `media-attach-requested`.
  - `gn_mls_media_manager_upload_async()`/`_download_async()` have no callers.
  - The manifest wording overstates the impact (N4).
- **Versions**

| Component | Old → new | Assessment |
|---|---|---|
| libmarmot | 0.11.0 → 0.12.0 | Correct (0.x MINOR for a breaking change, called out); SONAME issue in L2 |
| groundhog | 0.11.2 → 0.12.0 | Correct (MINOR, new library) |
| marmot-gobject | 1.4.0 → 1.5.0 | AGENTS.md text says MAJOR; the project's practice for this never-released component says MINOR (L1) |
| gnostr | 0.1.0, no bump | Correct (unreleased; no user-visible change) |

## Findings

### M1 (Medium): libmarmot's meson build no longer compiles

**Where**
- `libmarmot/meson.build:179-208` builds `test_media` (`:198`) with plain `marmot_deps`.
- Its source now includes `<jansson.h>` (`tests/test_media.c:15`) and uses `MARMOT_MEDIA_VECTORS_DIR` (`:30`).
- Only CMake supplies these (`libmarmot/CMakeLists.txt:304-307`).

**Failure scenario**
- Run: `meson setup B libmarmot -Dnostrc_build_dir=<cmake build>`, then `meson compile -C B`.
- Result: `tests/test_media.c:15:10: fatal error: 'jansson.h' file not found`. The undefined vectors dir fails next.
- The same command on master `82a615e4` compiles (exit 0).
- `tests` defaults to true, so a packager's plain meson build of the 0.12.0 this commit declares in `meson.build` fails. No CI job builds libmarmot with meson.

**Fix:** give `test_media` `dependency('jansson')` and `-DMARMOT_MEDIA_VECTORS_DIR="<source>/tests/vectors/media"` in meson, mirroring CMake. Consider a meson smoke job.

### M2 (Medium): the seal result hides `may_have_metadata`, and the header overclaims

**Where**
- `gh-mls-media.h:14` and `:96` say "metadata removed".
- `GhMlsMediaSealed` (`:88-92`) has no metadata flag.
- `gh_mls_media_seal()` (`gh-mls-media.c:343-379`) drops `prepared->may_have_metadata`.
- `gh_attachment_prepare()` strips only JPEG and PNG. "Anything else is returned unchanged" (`gh-metadata-strip.h:30`).
- The NIP-17 path instead drives the charter's one-time notice from that flag (`gh-attachment.h:65`, `gh-attachment-ui.c:402`).

**Failure scenario**
- The nostrc-q3a6 UI is built on this header.
- A user attaches an HEIC or WebP photo, or an MP4 or PDF, carrying GPS or author XMP.
- It is sealed and sent unchanged to every group member.
- The UI has no way to show "Files can contain hidden details such as location". The only service-level signal says the metadata is gone.

**Fix**
- Add the flag to `GhMlsMediaSealed`, copied from `prepared`.
- Reword the header: "JPEG/PNG metadata removed; other types unchanged".
- Test both cases.
- Optionally offer a neutral filename default. The display name is sent verbatim, and `IMG_20260930_142233.jpg` is itself a timestamp.

### M3 (Medium; must be settled before nostrc-m6tp): the 0x8007 decoder refuses state MDK accepts

**Where**
- `group_image.c:438-599`:
  - hosts are limited to ASCII letters, digits, `-` and `.` (`:489-492`);
  - `xn--` labels are refused (`:505`);
  - `^ | [ ]` are refused in the path (`:555-558`);
  - `^ | [ ] \ { }` and backtick are refused in the query (`:576-580`);
  - decode demands a byte-equal re-normalization (`:655-662`).
- It is documented in `marmot-media.h:242-251`, and nostrc-m6tp item 4 already names the risk.

**Evidence (differential test)**
- I ran MDK v0.11.0's own `validate_and_normalize_group_avatar_url` (url 2.5.8 / idna 1.1.0, pinned from MDK's `Cargo.lock`) against libmarmot. Inputs were 60,000 mutated URLs plus 20 ordinary ones.
- **libmarmot → MDK: 0 forks.** Every byte string libmarmot accepts or emits is MDK-canonical (47,630 agree). The producer side is safe.
- **MDK → libmarmot:** `marmot_group_avatar_url_decode()` refuses **9,868 of 29,370** distinct MDK-canonical stored URLs. That includes **12 of the 20 ordinary ones**:
  - `https://xn--bcher-kva.example/a.png` (from `bücher.example`)
  - `https://xn--r8jz45g.jp/avatar.png`
  - `https://my_host.example.com/a.png`
  - `…/avatar.png?size[]=512`, `…?w=512|h=512`, `…?sig=a^b` and ``…?q=`x` ``
  - `/a^b.png`, `/a|b.png` and `/[x].png`

**Failure scenario**
- nostrc-m6tp makes 0x8007 decode part of Commit/Welcome admission.
- An MDK admin sets an IDN avatar.
- MDK members accept the Commit. libmarmot members reject it, so the group forks.
- The spec makes validity WHATWG-defined and says contact or render policy MUST NOT affect it (`group-avatar-url-v1.md:57-73`).
- The same normalization defines 0x800b `default_blob_endpoints` (`group-encrypted-media-v2.md:84-86`). That component is required for media-capable app groups, so the exposure is larger there.

**"WHATWG" is not one function.** I checked a third implementation, Node 26's `URL` (ada-url, the current living standard):
- It agrees with MDK on 11 of the 12 honest URLs libmarmot refuses, so those refusals diverge from *both* parsers.
- The exception is `/a^b.png`: ada percent-encodes `^` in a path (`/a%5Eb.png`), where url 2.5.8 keeps it raw.
- It pops a drive-letter-like segment, `https://h/a/./b:/.%2e/c` → `/a/c`, as libmarmot does. url 2.5.8 keeps it (`/a/b:/c`).

So the two reference parsers already store different "canonical" bytes for the same input. The author's "serializer behaviour differs by version" comment is right.

**Recommended policy**
1. **Validity is consensus; rendering is local.** libmarmot must never reject a Commit because a URL lies outside *its* subset.
2. **Producer:** stay fail-closed, as now. Measured safe.
3. **Decoder:** return one of three results:
   - **VALID:** inside the subset and byte-equal.
   - **INVALID:** provably not WHATWG output. Any of these:
     - empty or too long, or not UTF-8;
     - any byte outside printable ASCII. An https serialization never contains one;
     - a scheme other than `https://`;
     - `@` in the authority, or `#` anywhere;
     - uppercase or `%` in the host, or `:443`;
     - dot segments, or an empty path;
     - an unencoded space, `"`, `<` or `>` in the path or query, or `'` in the query;
     - an in-subset value whose re-normalization differs.
   - **UNVERIFIED:** everything else.

   Admission accepts UNVERIFIED and stores the bytes verbatim, never rewritten, but marks the avatar not to be contacted, so a placeholder renders. The residual risk is an admin crafting an UNVERIFIED value that MDK rejects. That is admin-authored, and rarer than honest IDN avatars.
4. **Raise upstream first.** Ask marmot-protocol to pin the definition, either to a WHATWG snapshot plus a named reference parser or to a closed ASCII profile. Without that, no implementation can be byte-compatible with every other one.
   - Simply adopting a full WHATWG parser (for example ada-url's C API) would not buy MDK parity: ada and url 2.5.8 already disagree on `^` and on drive-letter-like segments.
   - Once the definition is pinned, implement it, and run this review's differential corpus in CI against MDK's crate.
5. **Keep the tri-state decoder after that.** VALID is the intersection subset (today's code), INVALID is "non-canonical under every version", and UNVERIFIED is the rest. It is the only policy that cannot reject an honest peer's Commit while implementations drift.
6. Make nostrc-m6tp depend on this decision explicitly.

### L1 (Low): marmot-gobject took MINOR for a break that AGENTS.md calls MAJOR

**Where:** `marmot-gobject/CMakeLists.txt:10`, `meson.build:2`, `VERSION_MANIFEST.md:19,145`, `marmot-gobject-client.h:688-694`.

**Why it is a break**
- `encrypt_media_async`'s documented behavior ("Asynchronously encrypts media") now always fails. That is a ≥1.0.0 component, and AGENTS.md:41-45 says MAJOR.
- The installed headers also expose libmarmot. `marmot-gobject-enums.h:13` includes `<marmot/marmot-types.h>`, and `marmot_gobject_client_get_marmot()` hands out `struct Marmot *` for direct calls, which gnostr makes. So the `MarmotMessageResult` layout is part of marmot-gobject's public surface.

**Mitigation:** it has never been released, and the manifest has precedent for MINOR or folding while unreleased (`VERSION_MANIFEST.md:62,90`).

**Failure scenario:** an external consumer built against the 1.4.0 headers loads 1.5.0 under the same SONAME (`.so.1`). Its direct `marmot_process_message()` call then gets the 8-byte stack overwrite described in L2, and it reads the epoch as `commit.updated_group`.

**Fix:** go to 2.0.0 before the first release, or write the "unreleased folds" rule into AGENTS.md. Either way, add a compile-time deprecation attribute to the declaration.

### L2 (Low; High for a released shared library): the SONAME does not record libmarmot's 0.x ABI break

**Where:** `libmarmot/CMakeLists.txt:109` and `meson.build:149` set the SOVERSION to MAJOR (0). The meson shared build emits `libmarmot.0.dylib` for both 0.11.0 and 0.12.0.

**Failure scenario**
- A distribution ships 0.12 as a drop-in `.so.0`.
- A 0.11-built application passes its 32-byte stack `MarmotMessageResult` to `marmot_process_message()`.
- `process_group_event()` runs `memset(result, 0, sizeof(*result))` (`messages.c:815`), which is now 40 bytes. That writes 8 bytes past the caller's struct on **every** call, and a Commit result's `updated_group` lands there too.
- On an application message, the app reads `commit.updated_group` at offset 24 and gets the epoch as a pointer.
- "Rebuild callers" (README) is not enforced. Moving `epoch` to the end of the struct would not help: the library writes the whole struct into caller-allocated storage.

**Fix:** for 0.x, use SOVERSION `0.MINOR`. This is a pre-existing policy; this is the first public struct-layout break under it.

### L3 (Low): `gh_mls_media_check_epoch()` reads the unreconciled record

**Where**
- `gh-mls-media.c:436-456` uses `marmot_get_group()` (`marmot.c:226-232`), which is a plain storage read.
- `marmot_media_encrypt()` (`media.c:762`) and `marmot_create_message()` (`messages.c:559`) first reconcile the record to the MLS state (`commits.c:777-807`).

**Failure scenario**
1. A crash lands between `marmot_commit_persist` writing MLS state N+1 and writing the group record.
2. The UI resumes a pending send.
3. The check compares the stale N with the sealed N and passes.
4. `marmot_create_message()` reconciles to N+1 and sends in N+1.
5. Every receiver derives N+1's media secret, so the attachment is permanently "damaged".

Separately, check and send are two calls, so their atomicity relies on the caller.

**Fix:** a libmarmot "send only in epoch E" entry point that returns an epoch-changed error, to be used by nostrc-q3a6. At minimum, reconcile before the check.

### L4 (Low): decrypted group attachments are not wiped on free

**Where:** `gh-mls-media.c:577` returns `g_bytes_new_take(pt, pt_len)`. The NIP-17 open path returns a wipe-on-free buffer (`gh-attachment.c:230`, `secret_bytes_new` and `OPENSSL_cleanse`), and the stripper wipes its own copy.

**Failure scenario:** photos and documents stay in freed heap, where a later heap disclosure or core dump can read them. Groundhog guarantees the opposite for one-to-one attachments.

**Fix:** return through `secret_bytes_new()`, or a wiping free function.

### L5 (Low): the retained legacy reader is untested and trusts sender-chosen inputs

**Where:** `media.c:837-895`. gnostr takes the epoch from the received tag (`gn-mls-media-manager.c:131,496`) and leaves `file_hash` zero, so `media.c:884` skips the hash check.

**What changed:** master's `test_media.c` had nine `marmot_decrypt_media` cases, and marmot-gobject's test included a decrypt round trip. The commit deletes all of them.

**Failure scenario**
- A regression in the read path is now silent. I verified it still works by hand: a Python-built legacy ciphertext opens, and a flipped hash is refused.
- The API decrypts any *received* legacy tag, not only "references already stored locally". The sender picks the epoch and can omit the hash, which is exactly what v2 removes.
- It is unreachable from gnostr's UI today.

**Fix**
- Add a fixed legacy fixture test.
- Gate the reader behind an explicit opt-in (like `allow_legacy_raw_messages`), or retire it in nostrc-i7d1.

### L6 (Low): `gh_mls_media_read_file_async()` reads the whole file first, and "native" includes GVfs FUSE

**Where:** `gh-mls-media.c:256` loads everything before `gh_mls_media_seal()` checks `max_size`. `g_file_is_native()` (`:269`) is TRUE for `/run/user/$UID/gvfs/…` paths.

**Failure scenario**
- A 6 GB video is read fully into RAM before it is refused at 25 MiB.
- A file under an SMB FUSE mount is fetched by gvfsd outside GhNetHttp and Tor.
- Both behaviors are inherited from the NIP-17 path (`gh-attachment-ui.c:522`).

**Fix:** query `standard::size` and `filesystem::remote` before loading.

### L7 (Low): the locator URL check approximates WHATWG

**Where:** `media.c:381-442`, compared with MDK's `Url::parse` (`host_safety.rs:31`).

**Divergences**
- It refuses `%`-escaped hosts that MDK accepts, such as `https://ex%61mple.com/x`.
- It accepts hosts WHATWG refuses: `https://1.2.3.256/x`, `https://[zzz]/x`, and bad punycode.
- `is_blank()` (`:367`) is ASCII-only, whereas Rust's `trim()` is Unicode-aware.

**Impact:** attachment-local only. One client shows an attachment the other omits; no state forks.

**Fix:** reuse M3's parser when it lands.

### Nits

- **N1. 0x8002 `media_type`.**
  - `group_image.c:72-81` applies the shared v2 canonicalization.
  - The spec cites the frozen v1 algorithm (`group-blossom-image-v1.md:66-68,114-115`). Under v1, `image/` and `a/b/c` are canonical.
  - MDK v0.11.0 does what libmarmot does: `canonicalize_marmot_media_type` is documented as "frozen" but enforces token and length rules. So libmarmot's choice is right for interop.
  - Comment it in the code, and raise the spec/MDK discrepancy upstream.
- **N2. The exporter step.** The fixtures start from an already-exported media secret (vectors `README.md:34`). Add `exporter_secret → media_secret`. For example, `000102…1f` gives `73bd647f044b114b9b3dc182b780653dc65c1b3fbaa3eda7a30f1db93e506635`; ideally take it from openmls `export_secret` in the generator.
- **N3. HKDF residue.** `mls_crypto_hkdf_expand()` (`mls_crypto.c:85-118`) leaves `t_curr`/`t_prev` on the stack. For a 32-byte output, that is the file key itself. This is pre-existing and affects every user of the key schedule; `sodium_memzero` both before returning.
- **N4. Manifest wording.** `VERSION_MANIFEST.md:147` says gnostr's "media send now fails". It was never reachable (§6), so there is no user-visible change. Say so.

## Evidence

**Build and test**
- **Build:** `cmake -G Ninja -DBUILD_GROUNDHOG=ON` and `ninja` on macOS: OK (2470 steps).
- **ctest, 42 targeted tests, 42/42 pass:** `marmot_*`, `marmot_gobject_test`, `groundhog-{store-marmot,mls-*,privacy-mls,media*,blossom,attachment*}` and `gnostr-test-{mls,media,blossom}*`.
- **`scripts/check-unsequenced-args.py`:** clean.
- **`scripts/linux-gate.sh --sanitizers`:** pass, 50 tests including `groundhog-mls-media`.

**Sanitizers and fuzzing**
- **macOS ASAN+UBSAN (Debug):** `test_media`, `test_commits`, `test_storage`, `test_types` and `test_protocol` are clean.
  - libmarmot's `test_media` is not in the CI sanitizer set. Consider adding it, along with `test_commits`.
- **Fuzzing under ASAN+UBSAN:** 300,000 mutated inputs across the avatar normalizer, the locator and imeta parsers, and the 0x8002/0x8007 decoders. No reports. Normalization is idempotent on the 47k accepted inputs, and encode/decode round-trips.

**Revert checks.** Nine mutations, each one caught by a failing test:
- the `group-event` context;
- no ciphertext-hash check;
- the late message reporting the current epoch;
- last-wins duplicates;
- the AAD without the filename;
- the epoch check always passing;
- non-native files accepted;
- avatar decode accepting non-normalized URLs;
- sealing the unstripped file.

**Cross-checks**
- **Independent re-derivation, MLS-Exporter, end-to-end and provenance:** see §1.
- **0x8007 differential test against MDK:** see M3. It was rerun against Node 26's `URL` (ada-url) for the WHATWG-version check.
- **Legacy reader equivalence:** checked by hand; see L5.
- **Meson:** the branch fails and master compiles; see M1.

**Not shown:** a live MDK v0.11 attachment exchange in both directions. That needs nostrc-a5u5, as the bead notes.
