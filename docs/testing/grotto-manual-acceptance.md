# Grotto Preview: installed desktop acceptance

Status **NOT RUN — no real desktop available in G12 (2026-10-14)**. This is the
exact release handoff for plan §9, not a report of executed steps. Source/Xvfb,
fixture camera images and fake Control1 are not substitutes. Grotto stays Preview
0.1.0; the owner decides promotion only after this evidence and the other final
gates pass. Delivery of G12 does not close the acceptance beads below.

## Clean-session matrix and evidence record

Run **every** applicable step below in each column, on unpatched distro GTK:

| Platform | Session/extension | Result | Evidence |
|---|---|---|---|
| Ubuntu 24.04, stock GNOME | GNOME session, Shell 46; no Ubuntu AppIndicator extension | NOT RUN | — |
| Ubuntu 24.04, Ubuntu session | Ubuntu session, Shell 46; AppIndicator present | NOT RUN | — |
| Ubuntu 26.04 | distro GNOME/Ubuntu session, Shell 50; record indicator extension state | NOT RUN | — |

For each cell record: date, operator, commit, package filename/hash/version,
`cat /etc/os-release`, `echo "$XDG_CURRENT_DESKTOP $XDG_SESSION_TYPE"`,
`gnome-shell --version`, `pkg-config --modversion gtk4 libadwaita-1 glib-2.0`,
portal/backend and Secret Service versions, enabled extensions and their versions,
camera model/driver, scale/text/language, command/result logs and screenshot names.
Do not assume a runner named ubuntu-latest proves Ubuntu 26.04 acceptance.

Use a disposable VM/user, no production accounts or developer keychain. Install
the native package with a private test identity and known test consumers. Keep the
real session bus for installed activation/portal/Shell tests; unit tests use their
own private buses. Never change a developer's default/search-list keychain. Export
only disposable material and erase it using supported UI. Redact tokens, reasons,
secret words and private message bodies from retained evidence. Pairing QR codes
are secrets; do not retain live offers. Use frame/state observations, not sleeps.

Owning beads:
- `nostrc-u6v55.14.1`: full installed matrix, screenshots and accessibility.
- `nostrc-u6v55.9.1`: stock-GNOME windowless first local request and real portal disclosure.
- `nostrc-u6v55.13.1`: G13 live GNOME 46/50 matrix (delivery bead .13 is closed).
- `nostrc-u6v55.7.1`: G06 physical camera and installed bunker unit (delivery .7 closed).
All four are **open**. Complete only the relevant bead after its entire acceptance
set passes; do not close one merely because another platform or a fixture passed.

## A. Install, first request, notifications and lifecycle

| Step | Action | Expected result | Bead |
|---|---|---|---|
| A1 | Install the single native `grotto` package; inspect installed files and `systemctl --user cat grotto-daemon.service`. Check desktop/metainfo/icons, both activation services, autostart template and both network examples. | GUI+daemon together; optional extension recommendation; canonical `org.nostr.Grotto.desktop`, no nostr: handler; no auto-enabled GUI login; base localhost-only policy and no ExecReload. | .14.1 |
| A2 | In a fresh session with both processes absent, have a local test consumer request an operation requiring consent. Do not launch management first. | D-Bus starts exactly one daemon and `grotto --gapplication-service`; **no management/onboarding window unnecessarily appears** for an already provisioned identity; a pending request is retained. Notification Review opens the exact compact review in one window. Repeat in stock GNOME without AppIndicator. | .9.1, .14.1 |
| A3 | Repeat an allowed/denied request; activate launcher twice and desktop New identity/Settings actions; use bunker:, nostrconnect: and local grotto: links. | One appropriate window; implemented actions reach their destination; no additional daemon; unsupported foreign bunker explains without contacting relays; supplied URI previews precede relay contact. | .14.1 |
| A4 | With a pending request, lock/unlock; inspect lock-screen and notification history. | No identity, content, reason, relay, token or private payload appears. Client/human-kind disclosure follows the locked generic fallback; no notification Approve action. Review/Deny act on the exact pending ID only. | .14.1 |
| A5 | Enable DND, deny notification permission, then test notification-service unavailability in a disposable session. Open Home and choose Not now. | Inbox remains usable with pending count; unavailability disclosed; Not now leaves request pending without repeated notification; dismissal is not denial. Stale notification opens Home with no-longer-waiting message, never another request. | .14.1 |
| A6 | Deny/cancel actual Background portal permission; also exercise unavailable portal/backend and enable again. | Actual denial/unavailability correctly disclosed; continuous residence follows permission; request-scoped activation still works. Portal v2 status is generic and reflects Ready/Waiting/Paused, not a false process/identity claim. | .9.1, .14.1 |
| A7 | Enable launch-on-login, log out/in, disable launch, then disable background. Inspect processes, user autostart and portal registration. | Only the selected native or portal autostart exists; native login uses --gapplication-service without a window; background-off also disables login, login-off alone keeps background setting. | .14.1 |
| A8 | Close window/Ctrl+W with pending work; relaunch; Ctrl+Q; cause a later review-required call. Log out/in and upgrade/restart daemon with requests pending. | Widgets destroyed, not hidden; pending work retains daemon deadline; Quit withdraws notifications but leaves authorized daemon signing; next call re-summons/catches up once; owner replacement invalidates old snapshots/dialogs with no stale approval. Logout ends session; upgrade is not a grant expansion. | .14.1 |
| A9 | Lock the real Secret Service collection and cancel its unlock prompt during create/import/sign/export; retry intentionally. | Honest locked/cancelled/failed result, no success toast or nonexistent identity; delayed backend result cannot resurrect a cancelled UI operation; seed/export material is cleared; no private data in logs. | .14.1 |

## B. Identities, local/browser/remote authority, pairing and network

| Step | Action | Expected result | Bead |
|---|---|---|---|
| B1 | Create two disposable identities, complete unpublished self-tests and seed backup; import a key without mnemonic. Make two local consumers select different identities; change management selection and explicit Default for older apps separately. | Valid signature + independent-peer NIP-44 self-test, no publication or grants; seed words available only for mnemonic-backed identity; explicit consumers remain pinned, management selection cannot switch them. | .14.1 |
| B2 | Request via two installed browser origins using the native host. Grant one scope to one origin; test the other and an ordinary non-approver process. | Origin permissions isolated; ordinary caller cannot approve/export/resume by claiming the GUI name. Client APIs never return raw private keys. Record PID-attested Linux behavior, not UID-only compatibility as isolation proof. | .14.1; coverage follow-up nostrc-ew9qv remains separately open |
| B3 | Pair two remote clients to different identities; interleave signing and NIP-04/44; attempt unknown client, malformed event, wrong secret, reused/expired token and changed-params replay. | No unadmitted/malformed signing, attribution and responses stay bound to original client/identity/RPC; hints are not grants; one-use token reviewed once; ordinary replay cannot regain revoked authority. New client defaults Low. | .14.1 |
| B4 | Paste/file-scan/create pairing offer; explicitly choose identity before review. Deny, Not now, approve; reject/interrupt acknowledgement publication, restart, then retry fresh pairing. | Preview before supplied-relay contact, denial/no-review cannot admit; only actual relay OK true completes pending-ack pairing; interruption stays visibly incomplete after restart. No token URI in logs/history. | .7.1, .14.1 |
| B5 | On a **physical camera**, allow permission and scan a disposable nostrconnect QR; repeat denied permission, missing camera, busy camera and camera removal. | Actual capture → appsink → zbar decode; actionable errors and paste fallback; no relay contact until identity choice/consent. Camera stops on close/cancel; no real pairing secret in retained screenshot. Run on both LTS platforms, in each session where behavior differs. | .7.1 |
| B6 | Revoke one client; lower trust while retaining explicit grants; expire remembered grants; pause during remembered signing and a delayed operation; resume through GUI with cancel and confirm, including lock-screen policy. | Revoke beats replay/pending work; exact permissions/expiry visible; lowering tier does not silently delete explicit scopes; pause prevents crypto delivery under old generation; lock/unlock never silently resumes. | .14.1 |
| B7 | Enable remote signing without the network drop-in; then follow apps/grotto/docs/pairing.md's installed-unit procedure with bunker.conf.example. Record effective ExecStart, IPAddressDeny and MainPID. | Base unit cannot reach remote relays (do not infer LISTENING from desired enabled). Drop-in resets network deny and starts --bunker, preserving host caller attestation and namespace-free hardening. Record actual remote EOSE/response success, not merely a websocket send. | .7.1 |
| B8 | Configure add/remove relays live; interrupt one then every relay and restore them. | PID unchanged; unchanged subscriptions preserved, removed delivery work cancelled; truthful DEGRADED/OFFLINE/recovery; no public-relay fallback, no restart required for ConfigureBunker. | .7.1, .14.1 |
| B9 | Replace interactive drop-in with headless example (one install-policy restart). Exercise pre-paired scoped signing, unknown pairing and review-required call. | --bunker --no-approval-ui; scoped work succeeds, new consent fails promptly typed NoApprovalAgent, no GUI activation. Relaxing IP filter alone is not headless setup. | .7.1 |
| B10 | Upgrade a disposable legacy profile with broad allows/denials and old autostart preferences, then restart twice; attempt failed writes/higher schema on copies only. | Unsafe broad event allows become non-executable Needs review; denials and valid non-event scopes preserved; old allowlist is not pairing; migration idempotent, corrupt/future state fails closed, write failure not reported as remembered success. Do not restore broad allows into a live old binary. | .14.1 |

## C. Live Shell extension (all G13 obligations)

Run the full `gnome/nostr-shell-extension/README.md` manual matrix as well. The
following explicitly covers GNOME **46 and 50**, and 24.04 Ubuntu AppIndicator.

| Step | Action | Expected result | Bead |
|---|---|---|---|
| C1 | Enable extension with daemon absent; open menu, then Open Grotto. | Signer not running; reads do not auto-start daemon. Open explicitly activates GUI. | .13.1 |
| C2 | Cause RequestsChanged/status changes, refresh failure, daemon owner replacement and live bunker disruption/recovery. | Badge follows current revision; refresh failure shows unknown, no cached false Ready/zero; no identity or request-content disclosure; recovery follows fresh owner snapshot. | .13.1 |
| C3 | Review requests; Pause; cancel then confirm Resume; Quit approval UI with GUI present and absent. | Empty-ID review reaches inbox/compact review; pause reduces capability directly; resume only trusted GUI confirmation; Quit leaves daemon and does not start an absent GUI. | .13.1 |
| C4 | Toggle extension off/on repeatedly, lock/unlock and log out; test Ubuntu AppIndicator extension present/absent. | All proxies/signals/name ownership cleaned; exactly one indicator when Shell module active, fallback SNI when appropriate; no stale badge/menu/action. Essential signing/inbox works without either indicator. | .13.1 |
| C5 | Traverse indicator/menu via keyboard and Orca on both Shell versions. | Names, state, badge meaning and actions announced; no Gtk/Adw loaded into Shell, no trapped focus or duplicate menu. | .13.1 |

## D. Accessibility and screenshot evidence

Repeat **every** row on each platform, not just in an Xvfb renderer:

| Step | Action | Expected result | Bead |
|---|---|---|---|
| D1 | Keyboard-only onboarding, Home/Activity/Connect/Settings, client/permission and identity/backup pages, compact review; resize across sidebar/bottom-navigation breakpoints while focused. | Reachable named controls and visible focus; Deny initially focused, Enter on unrelated control never approves; Escape/close follow consent semantics; no lost focus on affected stock GTK 4.14.5. | .14.1 |
| D2 | Orca reads identity/client labels, pending count, operation/identity preview, errors/conflicts, pairing/camera state and all action buttons. | Correct names/roles/state changes, sensible reading order and no secret text read without deliberate reveal; confirmation remains understandable. | .14.1 |
| D3 | RTL locale, 200% **text** scaling, and 360px logical width (also combine them); repeat main navigation, approvals, permission editor, onboarding and QR failure. | No clipped primary action, inaccessible overflow, reversed meaning, unreadable fingerprint or lost keyboard focus. Large text remains scrollable; raw JSON is optional Details, not the only review. | .14.1 |
| D4 | Capture light **and** dark screenshots of onboarding, compact approval, client permissions, identities/backup (no revealed secret), and degraded bunker state; include 360px and large-text/RTL evidence. | Real installed UI with disposable data; no tokens/seed/private payload; filenames identify platform/theme/page/scale/locale/commit. Show degraded state truthfully. | .14.1 |

The committed `apps/grotto/data/screenshots/` images are generated by
`signer/ui-layout` using real templates and fixture data. They are **not** these
manual captures and do not close any row above. Record a per-step result and
link evidence; a blanket “matrix passed” without artifacts is insufficient.

## Exit / promotion decision

Attach the three-platform result table and all evidence to `.14.1`; attach the
specific evidence to `.9.1`, `.13.1` and `.7.1` before closing them. Leave
`nostrc-9n2n9` open until the owner supplies an authenticated Launchpad/SRU filing
URL (the G10 stock-GTK source workaround is already landed; never invent a bug ID).
Keep `nostrc-ew9qv` and `nostrc-z6ps9` open until their separate coverage/product
contracts are satisfied. Credential-bearing AUTH relay paths need an owner policy;
do not silently strip/rewrite exact URLs. AppImage `nostrc-h3kh` remains deferred.
Only then re-evaluate every plan §10 promotion gate and obtain the separate owner
choice to remove Preview. No release tag or promotion is authorized by this file.
