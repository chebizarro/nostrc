# W7 profile editor — security, custody and UX review

Date: 2026-11-04. Review tracking: `nostrc-nv2ww`; feature specification: `nostrc-u6v55.23.9`.

## Scope, revision and verdict

**REQUEST CHANGES.** The production panel/host split is sound on the PID-attested Linux route, and the new MANAGEMENT requests cannot save reusable consent. However, the profile gate does not show the JSON it authorizes and gives a false publication-impact statement. Image authorization previews, metadata handling, destination fallback and cancellation also need attention.

Compared local `master` **`eb6fb1b4c40bb5349acf4a1d2aa46a7291b3bbbf`** to `origin/grotto/w7-profile-editor` **`94b2b505252983a635272c1159e2e40e886e4876`**. The merge base is that master commit. `git ls-remote` independently returned that exact branch SHA during this review. The supplied worktree was clean at entry. This is a focused W7 review, not a repository-wide security audit, deployment qualification or reopening of the feature's historical VM acceptance criteria.

Inputs: the bead, design §c.3/§d.2 in `docs/designs/grotto-settings-style-ux-2026-11-03.md:299-333,373-418`, and the **corrected** permission model at `docs/investigations/grotto-approval-persistence-and-gates-2026-10-25.md:487-500`. Older required-confirmation semantics are not the baseline.

**Evidence labels:** **Confirmed** means established by the current source path, not necessarily dynamically reproduced here. **Plausible** identifies a conditional risk or policy-dependent conclusion, with its precondition stated. Retained execution logs are distinguished from checks executed by this reviewer below. P1 = high/landing blocker; P2 = medium; P3 = low.

All source references below are relative to the reviewed worktree.

## Severity-ranked findings

### F1 — P1 / High / Confirmed: the kind-0 gate neither reviews the final JSON nor honestly describes publication

**Evidence:** `apps/grotto/src/ui/approval_dialog.c:667-669,743-810,842-847`; `nips/nip55l/src/glib/signer_approval_service.c:272-329,405-420`; `apps/grotto/src/grotto-profile-service.c:436-459`.

The new headline says “Publish profile for …”, but kind 0 falls into the generic preview branch: **kind number and structured-tag counts**, not the profile content. The daemon's structured summary deliberately excludes kind-0 content. The generic impact row remains **“Grotto returns the result to the app. It does not publish anything.”** The trusted host actually publishes immediately after the signature returns.

The editor shows the nine editable fields, but that is not a complete review of the signed JSON: Save preserves unknown fields from the fetched signed profile. Those extension fields are not shown in the form or this approval. The returned signature is bound to the constructed event, but the user is not shown that event's complete public content. This fails the requested **content = reviewed JSON** property and contradicts the real side effect. No malicious signer or non-Grotto caller is needed; this occurs on an ordinary Save.

**Required correction:** give MANAGEMENT kind 0 a plain-text, non-networking review of the actual final public JSON/complete field set, tied to the pinned identity, and state that approval allows Grotto to publish it. Keep one kind-0 review. Test the rendered content and impact, not only the headline.

### F2 — P2 / Medium / Confirmed: Blossom's separate review hides the actual authorization scope

**Evidence:** `apps/grotto/src/grotto-profile-service.c:474-489,568-573`; `apps/grotto/daemon/grotto_control.c:843-848`; `apps/grotto/src/ui/approval_dialog.c:643-645,680-681,794-807`; `nips/bud02/src/auth.c:195-207,223-248`.

Uploads do use the normal daemon gate; there is no silent local signing shortcut. But the image confirmation shows a picture and public-upload warning, not the destinations. The subsequent kind-24242 review is generic: tag **names/counts**, not the upload verb, blob hash, expiry or server scope. The daemon explanation says “selected Blossom server”, although the host builds **one server-unrestricted authorization** (`server_url = NULL`) and tries the signed 10063 servers in order. It is hash-scoped and defaults to five minutes, so this is not authority to upload arbitrary other bytes; nevertheless the visible scope is incomplete.

Server restrictions are optional in the protocol, so lack of a `server` tag alone is not a protocol violation. It does mean the review must not imply a narrower token: [BUD-11 authorization scope](https://github.com/hzrd149/blossom/blob/master/buds/11.md).

**Required correction:** show the intended destinations/fallback set and the actual action, hash and expiry in the existing image/auth review. Either bind authorization to the intended server set or explain its real hash-only, time-bounded scope. Relay AUTH is better: its canonical relay is supplied by the daemon and shown in the normal review (`signer_approval_service.c:241-265,311-312`; `approval_dialog.c:767-770`).

### F3 — P2 / Medium / Confirmed: uploaded originals retain EXIF and other embedded metadata

**Evidence:** `apps/grotto/src/grotto-profile-service.c:375-399,477-483,547-552,568-573`.

The host reads the original file into `j->bytes`, decodes a separate texture for preview, then hashes and uploads **the original bytes**. There is no re-encode, metadata removal or metadata-specific warning. A JPEG/WebP/PNG containing location, device or other identifying metadata therefore publishes it along with the visible image. Presence of sensitive metadata is conditional; the unchanged-byte path is confirmed.

The format/size guards are real and run before upload: signature-selected PNG/JPEG/WebP decoders, at most 5 MiB, at most 8192 pixels per side and a 24 × 1024² pixel budget. Renamed SVGs are refused before the generic decoder. These are not metadata-stripping measures. Servers cannot be expected to sanitize after the fact: [BUD-02](https://github.com/hzrd149/blossom/blob/master/buds/02.md) requires preserving uploaded bytes.

**Required correction:** strip nonessential metadata client-side before hashing/signing/uploading, with the preview matching the resulting image. If preservation is intentional, expose the risk and an explicit choice instead of silently preserving it. Add a metadata-bearing image case.

### F4 — P2 / Medium / Confirmed: absent identity configuration silently contacts shipped relay defaults

**Evidence:** `apps/grotto/src/grotto-profile-service.c:303-317,337-347`; `apps/grotto/src/relay_store.c:79-88,497-503`; `apps/grotto/data/org.nostr.Grotto.gschema.xml:105-110`; `gnome/grotto-panels/src/grotto-profile-view.c:195-197`.

Precondition: no identity relay configuration and an empty/missing global relay store. `configured()` then reads `g_settings_get_strv("relays")`, including its **schema default**, without checking for a user-written value. Merely mapping the profile starts discovery and sends the identity's author filter to those shipped public relays, before any profile publication review. Thus the destination restriction is not strictly “the identity's relays / explicitly configured relays”. A missing relay configuration does not stay missing.

The surrounding signer code already distinguishes user configuration from schema defaults (`nips/nip55l/src/glib/signer_approval_service.c:620-623`), so this is a concrete W7 fallback difference, not a request to audit other networking.

**Required correction:** use identity/signed-10002 or explicit user configuration, or obtain honest bootstrap consent before using public defaults. Test the empty-configuration case without making external connections.

### F5 — P2 / Medium / Confirmed: the busy editor cannot cancel a pending operation

**Evidence:** `gnome/grotto-panels/src/grotto-profile-view.c:61-67,114-126,140-153,199-223`; `apps/grotto/src/grotto-profile-service.c:102-103,457-464,498-504`.

The form creates Save but no Cancel/Stop action. Once an upload or Save starts, it disables the controls and sets `can_close = FALSE`. There is no busy-dialog close-attempt handler cancelling the request. The signing call can wait up to 180 seconds; publication uses a 240-second OK-wait policy, in addition to discovery/authentication work. A user cannot withdraw from the editor while those operations wait. Closing the image chooser/confirmation can cancel those earlier steps; that does not solve the later busy state.

The lower layer has a useful cancellation design: an operation-owned D-Bus connection is closed after cancellation so an outstanding daemon continuation can be retired. Identity changes/disposal also cancel. That capability is simply not exposed as a normal editor action. [GNOME action-dialog guidance](https://developer.gnome.org/hig/patterns/feedback/dialogs.html#action-dialogs) expects affirmative and cancel actions.

**Required correction:** expose cancellation while work is pending, retain the draft, and distinguish withdrawal before signing from stopping after a relay/server may already have received data. Do not promise rollback of acknowledged publications or uploads.

### F6 — P2 / Medium / Plausible, policy-dependent: watch-only profile images can contact arbitrary HTTPS origins without image consent

**Evidence:** `gnome/grotto-panels/src/grotto-profile-view.c:195-217`; `apps/grotto/src/ui/signer-window.c:847-855`; `apps/grotto/src/grotto-profile-service.c:401-425,523-527`.

The mechanism is confirmed: FETCH is permitted for watch-only identities, and a successful metadata fetch unconditionally downloads both signed `picture`/`banner` URLs. The URL guard accepts arbitrary HTTPS hosts; there is no origin permission or Load Images action. A watched pubkey's controller can change its signed avatar/banner URL to a tracking endpoint. Opening that profile can then disclose the viewer's address/request to that endpoint. Signature verification proves **which identity chose the URL**, not that the local user consented to that origin. No live tracking experiment was performed.

**Policy decision for this review:** rendering a deliberately viewed **local signing identity's own, verified published avatar/banner** is an acceptable narrow exception to the approval-preview “never auto-fetch” rule. W7 explicitly requests that display; it is not an approval dialog resolving caller-controlled links. That exception should not silently become blanket consent for externally controlled watch-only profiles. If selecting a watch-only identity is intended to imply image-network consent, make that policy and disclosure explicit; otherwise gate those image loads. Neither interpretation permits automatic image loads in approval previews.

### F7 — P3 / Low / Confirmed: draft/cancel documentation wrongly suggests uploaded images are unpublished

**Evidence:** `gnome/grotto-panels/README.md:35-43`; `apps/grotto/src/grotto-profile-service.c:480-489,568-573`; `gnome/grotto-panels/src/grotto-profile-view.c:134-137`.

The README says uploaded images remain “unpublished drafts until Save succeeds”. In reality, successful upload has already put a public blob on a server; closing the draft only frees local state. The preview's “This image will be public” warning is useful, but does not explicitly explain immediate publication independent of Save/cancellation.

**Assessment:** not deleting already-uploaded blobs on draft cancellation is acceptable; deletion cannot guarantee removal of copies and should not be presented as an undo guarantee. Correct the documentation and tell the user, before Upload, that the image becomes public immediately and remains uploaded if the profile draft is cancelled. Do not make implementing deletion a new prerequisite.

## Custody and cache assurance

### Confirmed safeguards

- **Panel contains no signer/secret transport.** The public tuple has nine strings, two in-memory textures and public status/timestamp fields; actions are a closed enum. No event JSON, file path, secret, signer or connection enters that API (`gnome/grotto-panels/include/grotto-panels-0/grotto-panels.h:169-208`). The panel forwards to an injected callback and validates results (`gnome/grotto-panels/src/grotto-panel.c:156-201`). Marker rejection is useful admission hygiene, not proof against malicious native code hiding secrets in public text/pixels.
- **Trusted construction and identity pinning.** The Grotto host rechecks unlocked/writable management plus `can_sign && !watch_only` for mutations (`apps/grotto/src/ui/signer-window.c:844-861`). `save_profile()` constructs a fresh kind 0, sets the selected identity's hex pubkey, writes the merged JSON as content and never copies caller-supplied event tags (`apps/grotto/src/grotto-profile-service.c:433-452`). The hook cannot carry extra event tags. The signer response must have a valid signature and the exact expected event ID/pubkey (`:92-118`), binding content, timestamp, kind and tags. **F1 is a review-visibility failure, not evidence that this response binding is absent.**
- **Non-Grotto host restriction on attested Linux.** `SignProfileEvent` is after fresh caller identification and the approver-only dispatch gate (`apps/grotto/daemon/grotto_control.c:661-672,830-849`). Approver proof checks installed executable path and device/inode, or the attested Grotto Flatpak ID (`nips/nip55l/src/glib/signer_caller.c:334-375`). Owning the GUI bus name or supplying a callback/claimed name is not authority. The shared panel host interface grants no D-Bus privileges. No new non-Grotto arbitrary-signing bypass was found on this route.
- **Daemon envelope check.** The method pins the event pubkey to the npub and permits only kind 0, Blossom authorization or relay AUTH; all submit through the same MANAGEMENT gate (`grotto_control.c:830-849`). It does **not** itself require empty kind-0 tags, parse kind-0 content as an object, or narrow Blossom action tags. Those construction invariants currently belong to trusted Grotto host code, not to a cryptographically narrow daemon API. Do not describe this method as safe for an untrusted native approver. Negative tests should cover the intended contract.
- **Signed metadata cache.** Reads and relay candidates call signed deserialization plus `nostr_event_validate()` and require the expected kind and identity hex pubkey before parsing/display (`apps/grotto/src/grotto-profile-service.c:50-67,241-254,353-365`). The underlying validator performs Schnorr verification (`libnostr/src/event.c:652-671`). Observed candidates use greatest `created_at`, with lowest event ID on ties; the cache writer prevents older replacement under its window-owned mutex (`profile-service.c:69-90`). This matches the replaceable-event tie rule in [NIP-01](https://github.com/nostr-protocol/nips/blob/master/01.md).
- **New cache paths are private.** Metadata is written atomically/durably with requested mode 0600; directories are created with 0700 (`profile-service.c:86-89`). Image files also request 0600 (`:403-417`). This is appropriate for new Unix paths; [GLib documents atomic replacement](https://docs.gtk.org/glib/func.file_set_contents_full.html).

### Limits — not demonstrated signature forgery

- **High / Confirmed, conditional and pre-existing platform limitation:** on a non-Linux bus without caller PIDs, same-UID callers are explicitly treated as unattested approvers (`nips/nip55l/src/glib/signer_caller.c:364-368,563-573`). W7 inherits that fallback. Its new raw-event method therefore has no non-Grotto-host isolation on that compatibility route; such a process can also answer its own approval. This is confirmed source behavior, not a newly demonstrated W7 Linux bypass; native passing tests cannot establish Linux-style caller custody. No broader caller-model remediation is added to this review's scope.
- **P3 / Low / Plausible:** existing cache directories/files are not checked for ownership or unsafe modes before reads; `g_mkdir_with_parents()` only applies its mode to newly created directories ([GLib documentation](https://docs.gtk.org/glib/func.mkdir_with_parents.html)). Cache-write errors are ignored. No upgrade/mode-failure or symlink test was found. This is a **plausible, conditional filesystem/privacy gap**, not proof that another UID can modify a normally created private cache.
- **P3 / Low / Plausible rollback/race risk:** a valid historical signed event is still authentic. If someone can replace the only on-disk cache with an older valid event, offline reads have no independent anti-rollback anchor. “Newest wins” means newest valid candidate observed, not proof of worldwide freshness. The mutex is scoped to one host window, not a cross-process transaction (`profile-service.c:63-90,595-600`). Cross-window/process races were not exercised.
- **P3 / Low / Plausible image-substitution risk:** image cache bytes are **not signed**: the filename is a hash of the URL, and loading only decodes the file (`profile-service.c:401-425`). A signed URL authenticates the reference, not mutable server bytes or a rewritten image cache. An attacker able to write that cache can plausibly substitute a decodable offline image. Do not claim the signed-metadata guarantee also authenticates avatar/banner pixels.


## Exactly what changed in nip55l consent

Only `nips/nip55l/src/glib/signer_approval_service.c` changes consent logic in the master-to-branch diff:

| Change | Current evidence | Effect on corrected model |
| --- | --- | --- |
| All MANAGEMENT explanations become signer-sourced | `:360-364` | Correct provenance for server-constructed explanations; does not itself authorize signing. |
| MANAGEMENT snapshots are non-rememberable | `:391-395` | UI cannot advertise reusable management consent. |
| `profile-management` + MANAGEMENT converts an evaluator ALLOW to ASK, including scoped AUTH | `:818-838` | Deliberate one-time management review even for saved/first-party allowance. DENY and ERROR are not converted. |
| Legacy `remember = TRUE` is rejected for MANAGEMENT | `:1213-1225` | No durable grant through the older decision entrypoint. |
| All reusable decision modes are refused for MANAGEMENT, after allow-once/deny-once | `:1242-1255` | No allow/24-hour/trust-routine/block-operation/activation bypass through the modern entrypoint. |

**No weakening found** of deny precedence, third-party explicit grants, tier presets or bounded flow consent. The base evaluator is unchanged: live denies/blocked clients precede live allows; flows are bounded and then presets apply (`nips/nip55l/src/glib/signer_permissions.c:479-521,603-629`). The W7 override only makes the named management path stricter. This is an explicit, task-requested management exception to ordinary remembered-app behavior, **not** a reinstatement of an always-confirm kind-0 class for all applications. Other MANAGEMENT requests also lose reusability, while the forced-ASK test is specifically purpose-scoped.

**Can the first-party MANAGEMENT kind-0 request create a persistent grant? No, on the inspected normal/legacy decision routes.** Both daemon checks reject reusable consent; allow-once leaves `remember = FALSE`. Room-send flow consent also cannot be repurposed for this request: it requires the distinct purpose, flow identity and encryption/seal operation (`signer_approval_service.c:1305-1314`). Existing grants are not deleted; they simply do not suppress this management review. Upload authorization and relay AUTH use the same one-time management policy, so the agent's “can require separate reviews” must not be read as a promise of one review for the entire image/authenticated-relay workflow.

The version is coherently **folded into untagged 0.8.0**, not unchanged code or a new 0.8.1: `VERSION_MANIFEST.md:57-67` records the owner-approved decision and `nips/nip55l/include/nostr/nip55l/signer_ops.h:28-31` agrees. Grotto 0.1.1 and panels 0.1.0 are likewise folded; reused publish/GObject/Blossom/bud02 components have no source/API changes in this diff. This review-only document needs no component bump.

## Privacy and scoped protocol behavior

- **No telemetry added in the reviewed path.** Network work consists of relay NIP-11 requests, author-scoped REQs, publication/AUTH, the two image GETs and selected-list Blossom uploads. No analytics endpoint or tracking SDK was introduced. This is a statement about W7, not every dependency/repository path.
- Discovery uses configured relays and verified identity-authored kinds **0/10002/10063**; signed 10002 controls relay follow-up/publication and signed 10063 controls upload servers (`profile-service.c:19-20,165,303-351,434,474-476`). **F4 is the concrete exception to the requested configured-destinations-only claim.** Image GETs are separate from relay/upload destinations, as explicitly evaluated in F6.
- NIP-05, Lightning and website fields are text; this code does not perform validation lookups, payments or background website fetching. Image GETs require HTTPS, reject userinfo/fragments, do not follow redirects, bound response size and decode only the accepted raster formats (`profile-service.c:44-48,367-425`). These are useful defenses, not per-origin consent or pixel authentication.
- **Tor fails closed in the inspected path.** With `use-tor-for-relays` true, the worker does not call discovery, upload, publish or online image GET; local signed metadata/images remain available (`profile-service.c:512-539`). It implements no Tor profile route. Without cached metadata the error explains this. With cache present the worker clears the Tor error and the UI says “Offline — showing the saved profile” (`:519-527`; `grotto-profile-view.c:95`), which hides the actual reason; improve that distinction rather than claiming Tor support.
- Scoped `nostr-protocol-smells` review: finite user-view/refresh backfill has author/kind/limit-scoped filters, cryptographic candidate validation, timestamp/ID replacement, EOSE-only successful completion and a failure watchdog (`profile-service.c:144-275`). It is not periodic polling and does not need an indefinitely live subscription for this finite operation. CLOSED/negative OK and a bounded AUTH attempt are handled, NIP-11 is advisory, publication uses publisher verdicts/ACKs rather than send success (`:122-142,177-240,284-301,454-471`). The publish timer advances an ACK deadline, not a backfill-completion guess. The authenticated branches are source-reviewed, not externally end-to-end proven here.

## UX/HIG assessment

**Appropriate structure:** Identity retains its separate local name and gains the 96-pixel avatar, banner, public-profile rows and Edit Profile… action. The form is an `AdwDialog` with header, Save, scrollable preferences content, nine fields, multiline About, image pickers/previews and inline failures (`grotto-profile-view.c:140-193,234-263`). Watch-only editing is hidden in the panel and independently refused for mutations by the real host. An unavailable host does not cause the library to acquire network/signing responsibilities.

**Writing:** Edit Profile…, Edit Profile, Save, Choose Avatar…, Choose Banner…, Upload Profile Image?, Upload and Cancel use appropriate header capitalization; explanations/errors are ordinary sentences. This agrees with [GNOME writing guidance](https://developer.gnome.org/hig/guidelines/writing-style.html#capitalization). F1/F2/F7 are semantic honesty defects, more important than capitalization. “Saving asks Grotto to review” is slightly impersonal when it is the user who approves. No unrelated approval-dialog capitalization cleanup is requested.

**Progress/failure:** asynchronous host work keeps GTK responsive. Spinner plus state text is honest indeterminate progress, not fabricated percentages. Offline Save/upload failures retain form values; partial publication has a distinct warning instead of pretending nothing happened (`profile-service.c:465-469`; `grotto-profile-view.c:89-126`). There is no phase/byte progress. That is an acknowledged limitation; the missing escape route during long waits is F5. Invalid fields currently surface a generic admission error rather than field-local explanation.

**Cancellation:** idle dialog closure discards the local draft; chooser/confirmation cancellation is translated to cancellation rather than upload failure. Busy-operation cancellation is missing. Completed public uploads are not rolled back; retain that honest limitation, with F7's disclosure correction, rather than promising reliable deletion.

**Floor:** reviewed controls fit GTK 4.14 / libadwaita 1.5: GtkFileDialog is available since 4.10, AdwDialog since 1.5 ([GTK](https://docs.gtk.org/gtk4/class.FileDialog.html), [Adwaita](https://gnome.pages.gitlab.gnome.org/libadwaita/doc/1.5/class.Dialog.html)). No new AdwSpinner/BottomSheet/ToggleGroup dependency appears. Retained Linux configure logs identify GTK **4.14.5** and libadwaita **1.5.0**. The review host has newer versions; it was not used to claim fresh floor execution.

**Accessibility/360 px:** labeled entry rows, a dedicated accessible About label, image alternative text, icon tooltips, wrapped status/error labels and plain-text profile rows are present (`grotto-profile-view.c:157-188,238-260`). The layout test opens the real form at 360 and wide sizes (`apps/grotto/tests/test-ui-layout.c:750-766`), but its bounds helper checks only the current focused widget (`:77-82`). The panel test drives a 360 × 720 window (`gnome/grotto-panels/tests/test-profile-panel.c:55-64`). Neither proves all fields/actions fit, full keyboard traversal, screen-reader error announcements, large text or long translated URLs. No independent visual/screen-reader execution is claimed. F5's absent Cancel action is the clearest keyboard/HIG defect in this scope.

## Tests: coverage, gaps and execution provenance

### What the new tests actually establish

| Coverage | Evidence and limit |
| --- | --- |
| Panel view/create/edit, all fields, multiline About | `gnome/grotto-panels/tests/test-profile-panel.c:46-88`; injected fake host, not production I/O. |
| Upload failure leaves URL/draft intact; Save failure stays open; cached display; watch-only hides Edit | Same test `:72-97`; useful UI behavior, but no successful real file-picker/Blossom transaction or busy cancellation. |
| Real production SignerWindow Save + real daemon + local relay | `apps/grotto/tests/test-profile-integration.c:111-171`; production profile hook, one MANAGEMENT kind-0 review despite a seeded first-party allow, reusable allow-24h rejected, valid signed author/kind, preserved extension, newest valid relay fixture, re-fetch, actual offline cache path. |
| Approval UI coverage in that integration | `test-profile-integration.c:60-92`; constructs the dialog and checks its headline, then authorizes via a synthetic D-Bus approver. It does **not** click through a real presented review/controller or assert content/impact, empty tags, exact complete JSON, or absent permission-file mutations. That is why F1 can pass it. |
| Panel boundary | `gnome/grotto-panels/tests/test-boundaries.py:20-46,64-100,173-187` extends compiler-schema/closed exported-and-imported symbol checks; `tests/test-admission.c:134-154` rejects credential markers, oversized About and non-texture objects. These constrain the shipped ABI/imports, not arbitrary malicious code in a host. |
| Non-Grotto daemon caller | `apps/grotto/tests/test-control-untrusted.c:1-2,13-17,35`; distinct Linux executable owns the GUI name but is refused before dispatch. The new case passes `{}`/empty identity, so it does not exercise trusted-caller envelope validation with valid hostile payloads. |

**Focused missing cases:** exact JSON/impact/auth-scope rendering; kind/identity mismatch and tag-smuggling negatives at the intended trusted boundary; persistent-grant rejection across other reusable/legacy modes and deny/error preservation; on-disk signature/author tampering and cache modes; successful production image upload, metadata stripping and size/format rejection; empty relay configuration; watch-only image consent; Tor no-network/cache messaging; cancel during daemon review/upload/publish; authenticated relay negative OK/CLOSED/AUTH retry paths. These are gaps in the behaviors reviewed here, not demands for a broad qualification matrix.

### Evidence inspected, not re-executed by this reviewer

Retained evidence directory: `/Users/bizarro/Documents/Projects/nostrc/.git/nostrc-task-evidence/nostrc-u6v55.23.9/`.

- `grotto-w7-native-land-test.log`: ends with PASS for `/signer/profile/save-one-review-real-daemon`.
- `grotto-w7-boundary-panels.log`: native panel CTest **4/4 PASS** (smoke, no-secrets/compiled boundary, bus selftest, Control1 adapter).
- `linux/final-serial-tests.log`: focused local Linux/container route **11/11 PASS**, including panels, clients/untrusted-control coverage, approval controller/integration, profile integration, UI layout, Blueprint drift and desktop contract. `linux/final-configure.log:41,61,66-67` records the floor libraries. This is container evidence, not VM/hosted acceptance.
- Retained affected gate receipt `.../.git/nostrc-test-gate/f35cb3726bf643ad8e80e7bfbcfc72ab/adapter-receipt.json` remains **failed, complete=false**, source **`db3047620cb38d2d02e534ef7ba5523829a7bfaf`**, with `core.macos.asan` refusing the unprovisioned `signer_endpoint` fixture capability before CTest. It was not repaired, bypassed or recast as green. The bead ties this to known `nostrc-klrq8` / `nostrc-4hv51` and records owner-authorized push despite it.

`git diff db3047620..94b2b5052` changes only `VERSION_MANIFEST.md` and nip55l version macros, not the functional source. That makes earlier functional logs relevant, but **does not make them a newly executed final-head gate**. Removed `/tmp` build trees were not recreated for this read-only review.

### Checks executed in this review

- Working-directory/HEAD/master/merge-base verification and exact `git ls-remote` branch SHA: **PASS**.
- `git diff --check master..origin/grotto/w7-profile-editor`: **PASS**.
- In-memory Python `ast.parse` of `gnome/grotto-panels/tests/test-boundaries.py`: **PASS**, syntax only; **not** the compiled-boundary test.
- Hooks setup: reported already installed. Bead/design/corrected-model reads and the scoped source/protocol review completed.
- **Not run here:** build, CTest, GUI interaction/screenshot capture, daemon cryptographic transaction, real Blossom upload, external authenticated relay test, VM deployment, hosted CI or release acceptance. Prior retained passes above are not presented as reviewer execution.

Source-navigation tooling used Jev `find-lines`/`filter-search` for long baseline files; answers guided ranges only, and load-bearing claims were checked in source. The broad design query was partial (0.69), so the named sections were read directly. The reviewed worker, view and focused tests required end-to-end reading. Tool ledger for this review period: 14 calls / 41 requests, 311,906 input tokens, approximately $0.0131; no security decision relies on those relevance scores.

## Disposition of the agent's remaining limitations

| Listed limitation | Review disposition |
| --- | --- |
| Tor profile networking unimplemented; cache available | Accept as explicit fail-closed limitation, not Tor support. Cached-state explanation needs the distinction noted above. |
| Indeterminate upload/publish progress | Honest and acceptable as a limitation; F5's missing cancellation is not excused by it. |
| Relay AUTH / Blossom authorization can add reviews | Correct; they use the ordinary gate and cannot save MANAGEMENT consent. Relay scope is shown; Blossom scope honesty is F2. One kind-0 review is not one total review. |
| Upload is public before Save; cancelled drafts do not delete blobs | Accept non-rollback semantics, but require truthful disclosure/documentation (F7), and address metadata exposure (F3). |
| Local relay fixture, fake-host failure/offline tests; no external Blossom/authenticated-relay proof | Accurate proof boundary. No external acceptance inferred; relevant gaps are enumerated above. |
| No hosted/release proof, VM access/deployment or unrestricted gate pass | Accurate; retained failure remains a failure. No unrelated environment repair is made a W7 prerequisite. |

No product source, test, version or configuration file was edited. No remediation, source commit, push, branch deletion or worktree cleanup was performed. The only worktree artifact added by this review is this requested report; tracking records the review separately from the completed implementation bead.
