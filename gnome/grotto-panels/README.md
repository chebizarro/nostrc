# Grotto Panels

MIT, GTK **4.14** / libadwaita **1.5**. Version **0.1.0**; public include
`<grotto-panels.h>`, pkg-config `grotto-panels-0`, SONAME
`libgrotto-panels-0.so.0`, CMake target `GrottoPanels::Panels`.

This is the G17 W2 **scaffold**, not an Identity/Apps/Activity/Remote Signing
implementation. There are no production panel descriptors or templates yet.
Future resources belong under `/org/nostr/GrottoPanels`; add committed Blueprint
and UI files with their drift check when a real panel is introduced.

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
Absent callbacks hide the corresponding action. No service means unavailable,
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
still requires daemon authorization. Unknown commands/argument shapes fail.
RPC result bodies never cross back to a panel.

Identity models remain empty pending W3's Grotto-owned **public** metadata
composite and W6's daemon snapshot. No private keyring enumeration, shared
accounts file or inferred policy is a substitute. Review, onboarding, Add App,
backup, removal and replacement remain trusted host callbacks. Settings must
hand them off to Grotto; it gains no approval or custody authority.

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
missing, and mount a test-only trivial panel in a bare `AdwWindow` with a split
view. They exercise wide/360px mounting, subpage push/pop, live fake-model
updates, capability loss, plain untrusted text, every host callback, cancellation
and destruction. `GTK_A11Y=test` preserves accessibility processing. Under
ASan/UBSan, keep `detect_leaks=1`; the smoke host alone uses the three
source-proven Pango/fontconfig cache exceptions tracked by `nostrc-ydtdf` in
`tests/lsan.supp`. The private-bus adapter has no suppressions. The library links
without either app. `no-secrets` checks the
public schema, source boundaries, exported/imported symbols and direct ELF
library dependencies. `control1-service` uses the production adapter against a
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
