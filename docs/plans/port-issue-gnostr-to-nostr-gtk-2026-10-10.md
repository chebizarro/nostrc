# Port plan: NIP-34 issue reporting, gnostr to nostr-gtk (2026-10-10)

Scope: gnostr `apps/gnostr/src/ui/gnostr-bug-report-dialog.c` (1316 lines) and `gnostr-bug-report-assembly.{c,h}` (99/46) compared with the alpha-6 parallel work in `nostr-gtk/src/gn-nip34-issue-fields.c` (87) and `gnome/groundhog/src/ui/gh-issue-dialog.{c,h}` (590/45).
Goal: one implementation in `nostr_gtk_portable` (GTK 4.6, libadwaita 1.2, no apps/gnostr headers), used by both apps; delete the duplicates.

## 0. Current state
- gnostr has the full dialog: subject, labels plus suggestion chips, description, related commits, an editable System info expander, a crash-log picker, a file attacher (25 MB cap), Blossom upload, repo-announcement (kind 30617) metadata lookup through NDB and then relays, publishing to the announced relays plus maintainer `p` tags, and a follow-up open-status event (dialog.c:416-536, 556-612, 690-810, 939-1107).
- `gnostr-bug-report-assembly.c` is pure GLib. It parses labels, assembles content (crash and attachment URLs, system info, commits) and calls `nip34_create_issue` (assembly.c:97). Tested in `apps/gnostr/tests/test_bug_report_assembly.c` (CMakeLists.txt:777-785).
- The nostr-gtk "port" is only a fields widget (steps, expected, actual, labels, commits, attachment *URLs as text*) with a snapshot struct (gn-nip34-issue-fields.h). gnostr does not use it.
- Groundhog has its own draft builder `gh_issue_draft_new` (gh-issue-dialog.c:143-196). It has byte bounds, label and commit validation, URL-only attachments, and opt-in diagnostics. It also has an exact unsigned-event preview with a consent alert (454-515), a review-staleness check (399-430), relays from the user, signing through Grotto, and `GhRelayPublish` (354-395).
- Result: the label parsing, body assembly and event building logic exists twice (`gnostr_bug_report_*` and `gh_issue_draft_*`). Both end in `nip34_create_issue`.

## 1. Behaviour diff
Features the reimplementation (nostr-gtk fields plus Groundhog dialog) lacks, compared with gnostr:
- Repo-announcement discovery (30617 newest-by-created_at, maintainers, relays): dialog.c:317-536. Groundhog hardcodes REPO_OWNER/REPO_ID and has no maintainer `p` tags (gh-issue-dialog.c:219 passes NULL maintainers).
- Open-status event (kind 1630) after publish: dialog.c:556-612.
- Crash-log discovery, newest first: dialog.c:128-190 (`gnostr_bug_report_discover_crash_logs`).
- An editable System info block: dialog.c:226-262, 1195-1215.
- Real attachments: file chooser, 25 MB cap, a per-item status row, a sequential Blossom upload queue, partial-failure handling and a "Send without logs" retry (dialog.c:678-810, 1010-1107).
- Label suggestion chips: dialog.c:897-937.
- Cancel and processing state handled through a GCancellable plus a spinner: dialog.c:266-300.

Features the reimplementation has that must be kept:
- Bounded input: title 640 B, body 16000 B, diagnostics 8000 B, at most 16 labels of 64 B, UTF-8 validated (gh-issue-dialog.h:12-14, .c:17-18, 44-70, 143-190). The body is never trimmed to fit (.c:182). gnostr has no byte bounds; its only cap is the 25 MB file limit.
- Commit IDs validated as 40 or 64 hex characters (.c:76-100). gnostr passes free text.
- Attachment URLs reject userinfo (.c:111-135).
- Structured fields: Steps, Expected, Actual (gn-nip34-issue-fields.c).
- Consent gating: the exact unsigned event JSON is frozen and previewed, and publishing is re-validated against it (`reviewed_json`, .c:256, 399-430, 471-515). gnostr publishes on Send with no preview.
- Diagnostics are off by default, previewed in place, and only available when `gh_diagnostics_get_default()` is set (.c:305-313, 569-570).
- No auto-fetch. The Groundhog dialog makes no network call until consent. gnostr fetches repo metadata from relays when the dialog opens (dialog.c:478, 533).
- i18n: both use gettext. The nostr-gtk widget uses the `nostr-gtk` domain (gn-portable-i18n.h), and the ported code must use that domain, not gnostr's.
- a11y: the Groundhog dialog uses a template with AdwEntryRow. gnostr builds widgets in code with plain labels (dialog.c:888). The port must set accessible labels or relations on every entry and text view.

## 2. gnostr dependencies to replace with injected interfaces
| gnostr dependency | Where | Replacement |
|---|---|---|
| `GnostrMainWindow` and `gnostr-main-window-private.h` (toasts, signer, publish) | dialog.c:5, 194-215, 1284-1316 | The `toast-requested` signal, or the host shows status itself |
| `util/blossom.h` `gnostr_blossom_upload_async`, hardcoded `BUG_REPORT_BLOSSOM_SERVER` | dialog.c:7, 21, 760 | `GnIssueUploader` interface |
| `nostr_pool`, `gnostr-relays`, `storage_ndb` (repo metadata lookup, publish) | dialog.c:16-18, 416-536, 615-670 | `GnIssueRepoResolver` and `GnIssuePublisher` interfaces |
| `gnostr-build-info.h`, `utils.h` (system info) | dialog.c:8-9, 226-262 | A host-supplied `diagnostics` string (no default collection in the library) |
| Hardcoded repo owner and ID | dialog.c:22-31; Groundhog REPO_OWNER/REPO_ID | `GnIssueTarget` value {owner_hex, repo_id, maintainers[], relays[]} |
| Crash-log directory | dialog.c:140-190 | `GnIssueAttachmentSource` or a host-provided list of `GFile` candidates |

Proposed public API (`include/nostr-gtk-1.0/gn-nip34-issue.h` and `gn-nip34-issue-dialog.h`):
```c
/* Pure model (GLib + nip34 only; GTK-free object lib like gn_content_parser) */
typedef struct { char *owner_hex; char *repo_id; GStrv maintainers; GStrv relays; } GnIssueTarget;
typedef struct { char *title, *body; GStrv labels; } GnIssueDraft;           /* replaces GhIssueDraft */
#define GN_ISSUE_TITLE_MAX_BYTES 640
#define GN_ISSUE_BODY_MAX_BYTES 16000
#define GN_ISSUE_DIAGNOSTICS_MAX_BYTES 8000
GnIssueDraft *gn_issue_draft_new(const char *title, const char *description,
                                 const GnNip34IssueFieldsSnapshot *fields,
                                 const char *diagnostics,
                                 const char *const *uploaded_urls, GError **error);
gboolean gn_issue_draft_equal(const GnIssueDraft*, const GnIssueDraft*);
NostrEvent *gn_issue_draft_build_event(const GnIssueDraft*, const GnIssueTarget*, const char *pubkey);
char *gn_issue_draft_to_unsigned_json(const GnIssueDraft*, const GnIssueTarget*, const char *pubkey);
NostrEvent *gn_issue_build_open_status(const char *issue_id, const GnIssueTarget*, const char *pubkey);
GnIssueTarget *gn_issue_target_from_announcement_json(const char *const *events, gsize n); /* newest wins */

/* Interfaces (GTypeInterface), all async + GCancellable */
GnIssueRepoResolver: void resolve_async(self, const GnIssueTarget *hint, GCancellable*, GAsyncReadyCallback, gpointer);
                     GnIssueTarget *resolve_finish(self, GAsyncResult*, GError**);
GnIssueUploader:     const char *describe_destination(self);   /* shown in consent text */
                     void upload_async(self, GFile*, GCancellable*, GAsyncReadyCallback, gpointer);
                     char *upload_finish(self, GAsyncResult*, GError**);  /* returns public URL */
GnIssuePublisher:    void sign_and_publish_async(self, const char *unsigned_json, const char *const *relays,
                                                 GCancellable*, GAsyncReadyCallback, gpointer);
                     gboolean sign_and_publish_finish(self, GAsyncResult*, char **event_id, GError**);

/* Widget */
GnNip34IssueDialog *gn_nip34_issue_dialog_new(const GnIssueTarget *target,
                     GnIssuePublisher *publisher,          /* required */
                     GnIssueRepoResolver *resolver,        /* nullable: no lookup */
                     GnIssueUploader *uploader,            /* nullable: URL-text attachments only */
                     const char *diagnostics);             /* nullable: checkbox hidden */
void gn_nip34_issue_dialog_set_crash_logs(self, GListModel *gfiles); /* nullable */
signals: "published"(event_id), "toast"(message)
property: "pubkey" (host updates; changing it invalidates review, as account_changed does at gh-issue-dialog.c:293)
```
The resolver runs only after the user presses Review and accepts consent, or after an explicit "Look up repository" action. It never runs on open. Uploads also run only after consent. Then the draft is rebuilt with the URLs, and the event is re-previewed before signing.

## 3. Groundhog requirements to preserve
- Privacy charter checks in `gnome/groundhog/tests/check_privacy.py`: rule `no-gdk-pixbuf` is verified (lines 35, 141, 346-347, 564-592), and the scope is Groundhog `src/**`. The ported library code must not use pixbuf either. nostr-gtk's portable target already avoids it, and a source grep test should keep it that way. The `libsoup-boundary` rule means Groundhog must not reach Blossom through gnostr's soup-based `util/blossom.c`. A Groundhog `GnIssueUploader` must go through GhNet, so it follows network mode, Tor routing, no redirects and the dispatcher (CMakeLists.txt:1925, 2638). Option for v1: Groundhog passes `uploader = NULL`, which keeps today's "Groundhog does not upload files" text (gh-issue-dialog.c:133, 495-496) until a GhNet uploader is built and covered by the charter.
- Consent before any fetch or publish. Keep the frozen-JSON review and the stale-review rejection. Diagnostics stay explicit opt-in with an inline preview. With an uploader present, the consent text must name the destination (`describe_destination`) and list each file. Nothing is selected by default; gnostr already does this for crash logs (dialog.c:1223).
- No auto-fetch: the resolver is nullable and lazy. For Groundhog, pass NULL or a GhNet-backed resolver that runs only after consent.
- Relays: Groundhog asks the user for relays (collect_relays, .c:329-350: wss only, at most 16). Keep this as the fallback when the target has no relays.
- Signing through Grotto plus `GhRelayPublish` partial-success messages (.c:354-395) belong in the Groundhog `GnIssuePublisher` adapter.
- Tests that exist: `gnome/groundhog/tests/ui/test_about_flows.c:234-280` (draft bounds, equality, JSON), `nostr-gtk/tests/test_portable.c:264-268` (fields snapshot), `apps/gnostr/tests/test_bug_report_assembly.c`, and `check_privacy.py`. No render-cache or performance gate covers this dialog. The only relevant gates are the portable-floor build (GTK 4.6/Adw 1.2) and the privacy check.

## 4. File moves and build changes
1. Create `nostr-gtk/src/gn-nip34-issue.c` and `include/nostr-gtk-1.0/gn-nip34-issue.h`. Build them from `gh_issue_draft_*` (gh-issue-dialog.c:17-236, kept as the bounded core) merged with gnostr assembly.c (crash and attachment URL sections, maintainers) and the 30617 parsing from dialog.c:317-399. Add them to a GTK-free OBJECT lib (pattern: `gn_content_parser`), and link `nip34` and `nostr` into `nostr_gtk_portable`.
2. Create `nostr-gtk/src/gn-nip34-issue-iface.c` and `.h` for the three GInterfaces.
3. Create `nostr-gtk/src/gn-nip34-issue-dialog.c` and `.h` (AdwDialog). Port the gnostr dialog body (dialog.c:678-1282: crash list, attachments, upload queue, labels chips, processing state). Embed `GnNip34IssueFields`, and use the Groundhog review/consent flow (gh-issue-dialog.c:399-515). Use only Adw 1.2 API: `AdwAlertDialog` and `AdwDialog` are Adw 1.5, so either gate them with `ADW_CHECK_VERSION` and fall back to `AdwMessageDialog`/`GtkWindow`, or confirm the floor. **Verify this first.** The existing Groundhog dialog uses AdwDialog and AdwAlertDialog.
4. In `nostr-gtk/CMakeLists.txt`, add the new headers to both header lists (around 150-171) and the sources to `NOSTR_GTK_PORTABLE_SOURCES` (around 175-183). Add the translatable files to `nostr-gtk/po/POTFILES`.
5. gnostr: add `apps/gnostr/src/ui/gnostr-issue-adapters.c` with resolver (NDB, then pool; the existing code from dialog.c:416-536), uploader (`util/blossom`, with the server moved to GSettings or kept as a constant), publisher (the main-window signer plus pool, plus the open status), and system info as the diagnostics string. Replace `gnostr_bug_report_dialog_new` callers in `gnostr-main-window-signals.c` and `gnostr-session-view.c` (dialog.c:1303 handler). Delete `gnostr-bug-report-dialog.{c,h}` and `gnostr-bug-report-assembly.{c,h}`, and remove them from the gnostr CMake source list. Keep linking nip34 (CMakeLists.txt:223-229).
6. Groundhog: add `gh-issue-adapters.c` with a publisher (Grotto plus GhRelayPublish, account-change handling), a NULL uploader, a NULL resolver, and diagnostics from `gh_diagnostics_get_default()`. Reduce `gh-issue-dialog.{c,h}` to a thin factory `gh_issue_dialog_new(accounts, settings)` that returns the portable dialog, or delete it and update `gh-app-services.c`. Update CMakeLists.txt:47 (UI list) and 3454-3470 (`groundhog-issue` target) to link `nostr_gtk_portable`, and drop the draft code. Remove the GtkBuilder template for the old dialog if one exists under `data/`.
7. Delete the duplicates: `GhIssueDraft` and `gnostr_bug_report_*`. `GnNip34IssueFields` stays, now owned by the dialog.

## 5. Tests
- Move `apps/gnostr/tests/test_bug_report_assembly.c` to `nostr-gtk/tests/test_nip34_issue_model.c` (GTK-free) and rewrite it against `gn_issue_draft_*`.
- Move the draft cases from `gnome/groundhog/tests/ui/test_about_flows.c:234-280` into the same file (bounds, no trimming, label and commit validation, userinfo rejection, equality, JSON). Leave a Groundhog smoke test that the factory returns the portable dialog.
- Add tests:
  - announcement parsing, newest wins
  - the open-status event
  - the dialog makes no resolver or uploader calls before consent (mock interfaces that count calls)
  - an edit after review invalidates it
  - a pubkey change invalidates it
  - the upload partial-failure path and the "send without logs" path
  - the diagnostics checkbox is hidden when NULL
  - a11y: every input has an accessible label (GtkAccessible test)
- Keep `test_portable.c` for fields. Keep `check_privacy.py`, and extend it to verify that the Groundhog adapter passes a NULL uploader or a GhNet-only uploader.
- Add a nostr-gtk source grep test for no `gdk_pixbuf`, no `soup` and no `apps/gnostr` includes in portable sources.

## 6. Risks and ordering
Risks:
- The libadwaita floor (AdwDialog and AdwAlertDialog are 1.5 or later) may force a fallback path.
- gnostr behaviour changes: it gains a consent preview and byte bounds. Reports over 16000 B, such as large system info, will now be rejected. This is acceptable, but it must be communicated.
- The repo-metadata lookup moves from "on open" to "after consent", so maintainers and relays may be unknown at preview time. Fix: the preview shows the hint target, and the dialog re-previews if the resolver changes the tags.
- The Blossom server is hardcoded. It must remain gnostr-only.
- Signals and lifetimes: the GCancellable must be cancelled on close (both originals do this).

Order (each step builds and passes tests):
1. Verify the Adw floor and decide the fallback.
2. Model, with merged tests.
3. Interfaces.
4. Portable dialog plus mock-interface tests.
5. Switch Groundhog (smallest adapter, no uploader), then run check_privacy.
6. Switch gnostr adapters.
7. Delete the gnostr dialog and assembly, and `GhIssueDraft`.
8. Update the POT files.
9. Optional follow-up: a GhNet Blossom uploader for Groundhog, behind consent and the charter.
