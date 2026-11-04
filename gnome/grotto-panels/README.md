# Grotto Panels

MIT, GTK **4.14** / libadwaita **1.5**. Version **0.1.0**; public include
`<grotto-panels.h>`, pkg-config `grotto-panels-0`, SONAME
`libgrotto-panels-0.so.0`, CMake target `GrottoPanels::Panels`.

G17's shared **Identity panel** is implemented; Apps/Activity/Remote Signing
remain later slices. `grotto_identity_panel_new(service, NULL)` shows the daemon
default (or first listed identity); a public key pins a detail page. Other
Identities pushes pinned pages without changing the default. Add Identity is a
flat action; Backup, removal and public-key QR stay host callbacks.
Resources use `/org/nostr/GrottoPanels`; committed Blueprint/UI pairs are checked
by `signer/blueprint-drift`. Hosts which already own window chrome set the
panel's `show-window-controls` property to false; pinned subpages inherit it.
The independent `grotto-panels` gettext domain is extracted from `po/POTFILES.in`
and installed by both build systems (gettext/msgfmt is a build dependency).
The template and dynamic sentences use that domain, not the host's domain.

## Ownership and injection

Subclass the abstract `GrottoPanel` (`AdwNavigationPage`) and set its `service`
property. Each panel retains its own service reference; there is no global
service or callback table. Models contain immutable `GrottoPanelsItem` objects
with an explicit public-metadata schema. Update models using `items-changed`
(replace immutable items); never pass opaque daemon dictionaries to a widget.
`grotto_panel_create_metadata_row()` renders both labels and reasons as plain
text, without interpreting daemon policy fields.

All API calls, model changes and signals run on the GTK main context. Rebind
view models when `service-changed` fires, including initial attachment. The base
class disconnects availability/capability listeners and cancels the old view
cancellable on detachment. Subclasses must also disconnect their own model
listeners and reject cancelled/stale async results. Host callbacks are copied;
the supplied destroy notifier runs once on replacement or panel disposal.
The callback table stays private: use `grotto_panel_host_can()` and the guarded
`grotto_panel_host_invoke()`. Absent callbacks hide the corresponding action. No service means unavailable,
not a local permission fallback.

Grotto owns the Control1 adapter in
`apps/grotto/src/grotto-panels-control1-service.*`. It subscribes before listing,
pins each call to the daemon's unique owner, rejects stale owner/signal
snapshots, deduplicates IDs, preserves request deadlines and owner-scoped
resolved-request tombstones. Activity sequence is only a cursor, never a
snapshot revision. Keep this adapter alive at the **application** level; lazy
panel remounts must not recreate pending state. It neither replaces nor owns the
approval controller.

Create a GIO proxy for bus **`org.nostr.Signer`**, object
**`/org/nostr/signer/grotto`**, interface **`org.nostr.Grotto.Control1`** on the GTK
main context, then inject `grotto_panels_control1_service_new(proxy)`. The current
daemon's coarse feature map does not authorize commands. Until W6 adds
caller-specific `management-capabilities` (`a{sb}`), the adapter's commands are
read-only/unavailable. The closed command names in the adapter define the
future map; no manager role is implemented or assumed here. Cosmetic manager
commands, session pause and approver-only changes are distinct now; every RPC
still requires daemon authorization. **Manager means reads and cosmetic labels,
not tiers.** `SET_CLIENT_TRUST` (tier/preset), permission replacements, defaults,
resume and remote configuration remain approver-only. A label command rejects
trust/permission fields; the adapter maps it only to `SetClientLabel`. W6 adds
manager authorization to the daemon; it does not expand this authority split.

Commands accept `GrottoPanelCommandArgs` and typed permission records, not
`GVariant`, dictionaries, arbitrary object payloads or RPC method names. Every
unused member must be zero. Metadata construction, model replacement, commands
and host actions reject known credential encodings/markers, including recovery
phrases, before any model publication or backend dispatch. Model getters reject
generic-object carriers; only the final immutable metadata item type is allowed.
The compiler-schema test checks **all** fields/types (including typedef aliases),
and the closed compiled import/export surface forbids D-Bus/network/dynamic-I/O
code in the library regardless of source layout or split string literals.
These are API admission/regression boundaries, not a sandbox for malicious
native code or a universal detector of arbitrary tokens disguised as normal
public text. RPC result bodies and remote error text never enter a panel.

Both services use `G_IO_ERROR`: unauthorized/locked → `PERMISSION_DENIED`,
revision conflict → `BUSY`, unsupported → `NOT_SUPPORTED`, invalid arguments →
`INVALID_ARGUMENT`, owner loss/cancellation → `CANCELLED`, unavailable →
`NOT_INITIALIZED`, other backend failure → `FAILED`. Error text is generic.
The test fake records typed calls and provides configured errors and explicitly
held completion (deterministic latency), including cancellation and owner loss;
it does not evaluate signer policy or invent a successful mutation.

The common Control1 service's Identity model stays empty until W6's daemon
snapshot. Grotto supplies `GrottoIdentityService`, joining its existing
signal-driven Control1/home model with public AccountsStore metadata. It exports
only public identifiers, local labels, setup state, watch/signable flags, the
legacy default and app counts. Label writes stay in Grotto; default changes use
`SetDefaultIdentityForLegacy`, pinned to the daemon owner. No panel enumerates
keyring items or reads accounts.ini. Settings must not reuse Grotto's local store.
`SET_IDENTITY_LABEL` is Grotto-local until W6; the common adapter refuses it.
The `open_in_signer` callback receives the public-only navigation targets
`public-key:<npub>` (public QR) and `apps:<npub>` (app list). It never receives a
QR payload, credential or proposed mutation. Grotto's trusted Backup page owns
recovery, encrypted export and raw-key dialogs; these are not library resources.
Review, onboarding, Add App, backup, removal and replacement remain trusted host
callbacks. Settings must hand them off to Grotto; it gains no custody authority.

## Build and focused checks

From the repository root:

```sh
cmake -S gnome/grotto-panels -B /tmp/grotto-panels -G Ninja
cmake --build /tmp/grotto-panels
ctest --test-dir /tmp/grotto-panels -R '^grotto-panels/' --output-on-failure

meson setup /tmp/grotto-panels-meson gnome/grotto-panels
meson compile -C /tmp/grotto-panels-meson
meson test -C /tmp/grotto-panels-meson --print-errorlogs
```

Linux smoke tests require `dbus-run-session` and `xvfb-run`, fail visibly when
missing, and mount the real Identity panel against the fake service, plus the scaffold
panel in a bare `AdwWindow` with a split view. They exercise wide/360px mounting, subpage push/pop, live fake-model
updates, capability loss, plain untrusted text, every host callback, cancellation
and destruction. `GTK_A11Y=test` preserves accessibility processing. Under
ASan/UBSan, keep `detect_leaks=1`; the smoke host alone uses the three
source-proven Pango/fontconfig cache exceptions tracked by `nostrc-ydtdf` in
`tests/lsan.supp`. The private-bus adapter has no suppressions. The library links
without either app. `no-secrets` executes metadata/model/command smuggling attempts for credential
payloads, checks the compiler's complete schema and compiled import/export/ELF
closure, and builds counterfactuals for both independent-review bypasses. Host
action smuggling, deferred failures and in-flight detach run in `smoke-host`.
Tests require Clang for structural C declarations; a missing tool fails visibly. `control1-service` uses the production adapter against a
fixture implementing the canonical Control1 XML on `nostrc-test-bus`; it is not
proof of the future manager gate or real Nostr Settings hosting (W6).

Affected selection registers `grotto.panels` and a standalone Linux profile.
For an explicitly **partial** component projection in an appropriate Linux
environment:

```sh
scripts/test-gate.sh --base "$BASE" --head "$HEAD_SHA" --profile grotto.panels.linux --explain
scripts/test-gate.sh --base "$BASE" --head "$HEAD_SHA" --profile grotto.panels.linux
```

The unfiltered affected plan also retains downstream Grotto/Settings and version
consumers. A standalone profile is not full host/CI/release acceptance. Meson
consumers install this library first and resolve `dependency('grotto-panels-0',
version: '>=0.1.0')`; Grotto does so without linking its implementation into the
shared library. CMake consumers resolve `find_package(GrottoPanels 0.1 CONFIG)`.
Native host recipes set `GROTTO_USE_SYSTEM_PANELS=ON`: the runtime/SDK are supplied
by the separate Debian/RPM provider, rather than stranded in Grotto's private
install subtree. See [packaging](packaging/README.md) for the explicit installed
consumer smoke and its native-Debian versus RPM-diagnostic boundary.
