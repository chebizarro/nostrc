# gnostr-signer becomes Grotto (W31)

**Owner decision (2026-10-05):** the signer is renamed **Grotto**, application
id **`org.nostr.Grotto`**. The rename lands before the signer's UX pass and
before any package is published (W30), so nothing public ever carries the old
name. Tracking: nostrc-8otj.

## What changes and what does not

| Thing | Today | After | Why |
|---|---|---|---|
| Display name | GNostr Signer | **Grotto** | owner decision |
| Application id (GApplication, desktop file, metainfo, icon name, D-Bus activation file for the app) | `org.gnostr.Signer` | **`org.nostr.Grotto`** | owner decision |
| GSettings schema id and path | `org.gnostr.Signer`, `/org/gnostr/Signer/` | `org.nostr.Grotto`, `/org/nostr/Grotto/` | follows the app id; settings reset once (pre-release, no migration) |
| GResource prefix | `/org/gnostr/Signer/` | `/org/nostr/Grotto/` | follows the app id |
| Binaries | `gnostr-signer`, `gnostr-signer-daemon` | **`grotto`**, **`grotto-daemon`** | hard cut: nothing public yet, no compatibility symlinks |
| Source directory, CMake targets, C prefixes | `apps/gnostr-signer`, `gnostr_signer_*`, `GnostrSigner*` | `apps/grotto`, `grotto_*`, `Grotto*` | one name throughout |
| Config and data directories | `gnostr-signer/` under XDG config/data | `grotto/` | **moved once at startup** when the old directory exists and the new one does not (accounts.ini, delegations, event history, multisig state) |
| systemd user unit, launchd label, Homebrew formula, packaging files, snap names, fuzz workflow | `gnostr-signer*` | `grotto*` | follows the binaries |
| **D-Bus protocol** — bus name `org.nostr.Signer`, object path `/org/nostr/signer`, interface and error names | unchanged | **unchanged** | it is the wire contract (NIP-55L) that Groundhog, gnostr, the browser bridge and every test speak; a client must not care which signer answers |
| **Stored key items** — Secret Service schemas `org.gnostr.Signer/identity`, `/key`, `/migration`, `org.gnostr.NostrKey`, `org.gnostr.Key`; Keychain services "Gnostr Identity Key", "Gnostr Signer Migration" | unchanged | **unchanged** | they are storage-format identifiers no user sees; renaming them means migrating the most valuable data in the system for no visible gain. Recorded as legacy format constants. (Owner may veto; the cost is a tested one-way migration with a fallback read of the old schema.) |
| Browser native-messaging host `org.nostr.signer_bridge` | unchanged | unchanged | protocol-facing name the extension is pinned to |
| nip55l library and its reference daemon `nostr-signer-daemon` | unchanged | unchanged | they implement the NIP, not the app |
| Historical documents (reviews, plans, beads) | unchanged | unchanged | they describe what was done under the old name |

Measured footprint (2026-10-05): `gnostr-signer` 1,150 occurrences in 258
files, `gnostr_signer` 448/46, `GnostrSigner` 228/41, `org.gnostr.Signer`
247/77; 132 of the files are under `apps/gnostr-signer`, 38 under
`apps/gnostr`, the rest packaging, snap, flake, CI and documentation.

## Phases

1. **R1 — mechanical rename.** `git mv apps/gnostr-signer apps/grotto`; ids,
   binaries, targets, C prefixes, resource paths, schema, desktop/metainfo/
   service/icon files, packaging recipes, snap directories, workflows, and
   every reference from `apps/gnostr`, Groundhog (signer launcher, onboarding
   copy, tests) and the top-level build. One commit, no behaviour change,
   reviewed by diff statistics plus a build and the full test suites.
   Guard: a static check that the old names appear only in the allow-listed
   legacy constants and historical documents.
2. **R2 — data continuity.** One-time move of the XDG config/data directories;
   tests for: old only, new only, both (new wins, old left alone), neither.
   The stored-key schemas are untouched, so identities are found without any
   migration; a test proves a key stored by a pre-rename build is listed and
   usable after it.
3. **R3 — identity.** New icon and metainfo copy for Grotto, "(preview)"
   labelling until the UX pass, About dialog.
4. **R4 — UX pass** (separate wave): onboarding, the startup layout error
   (AdwPasswordEntryRow in a GtkBox), approval dialog, hardware keystore's
   Keychain calls on macOS (nostrc-2hmd), `--instance` parity with Groundhog.

## Risks

- **Template and type names.** GObject type names appear in Blueprint/`.ui`
  templates and in `g_type_from_name` lookups; a partial rename compiles and
  then fails at runtime. Mitigation: rename with a script that covers `.c`,
  `.h`, `.blp`, `.ui`, `.xml` and CMake together, and run every GUI test.
- **Activation.** The D-Bus *service file* that starts the daemon for
  `org.nostr.Signer` keeps its bus name but must `Exec=grotto-daemon`.
- **Third parties.** Homebrew formula, AUR and snap names change; none is
  published yet.
