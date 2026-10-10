# Port plan: NIP-21 / NIP-18 (nostr: URIs, reposts, quotes) gnostr -> nostr-gtk

Date: 2026-10-10. Read-only planning; nothing changed besides this file.

## 0. What actually exists (correcting the brief)

There are four NIP-19/21 decoders and two NIP-18 resolvers today:

| # | Code | Lives in | Used by |
|---|------|----------|---------|
| A | `gnostr_uri_*` (`apps/gnostr/src/util/nip21_uri.c`, 303 lines; `.h` 217) | gnostr app | **Nobody.** Compiled at `apps/gnostr/CMakeLists.txt:673`; included but unused by `nostr-gtk/src/nostr-note-card-row.c:35` (a stray app-header include in the library). Dead code. |
| B | `gnostr_nostr_target_parse` (`apps/gnostr/src/util/gnostr-nostr-target.{c,h}`) | gnostr app | gnostr's real nostr: entry point (dispatcher / org.nostr.Handler1): refuses nsec/ncryptsec/nrelay with typed GError, kind->view table, NIP-01 id+sig validation, `event_matches_target`. Tested by `gnostr-test-nostr-target` (`apps/gnostr/CMakeLists.txt:966-983`). |
| C | content descriptor decode in `nostr-gtk/src/content_renderer.c:420-450, 539-574, 766` | nostr-gtk portable (`CMakeLists.txt:196`) | Both apps' inline nostr: token rendering. Uses nostr-gobject `nostr_nip19.h` (line 12). Already shared - not a port target, but must switch decoder. |
| D | `gn_nostr_reference_*` / `gn_nostr_repost_descriptor_*` / `gn_nostr_build_{repost,quote}_template` (`nostr-gtk/src/gn-nostr-reference.c`, 308 lines) | nostr-gtk portable (`CMakeLists.txt:167,177`) | Groundhog (`gh-conversation-view.c:1250-1280`, `gh-public-note-ui.c:264,371`, `gh-public-post-dialog.c:303`, `gh-public-note-post.c:111-112`) **and already gnostr** (`gnostr-timeline-view-app-factory.c:996`, kind 6/16/q classification). |

NIP-18 rendering/resolution: gnostr's lives in `apps/gnostr/src/ui/gnostr-timeline-view-app-factory.c:1195-1445` (NDB lookups of the original/quoted note and author profile, then `nostr_gtk_note_card_row_set_repost_info` / `set_quote_info`, else emits `request-embed` which `gnostr-timeline-embed.c:313+` resolves from NDB and then **auto-fetches from relays**). Groundhog's lives in `gh-conversation-view.c` (inert summary row, a11y label) and `gh-public-note-ui.c` (explicit "Find on Relays" REQ through `GhRelayScope` + `GhAuthPolicy`, gated on writable store).

So "port gnostr's implementation" means: the *pure* half is B (+ A's builders), the *widget* half is the card-row repost/quote presentation (`nostr-note-card-row.c:5822-5900, 6015-6075`), and the *resolution* half is the factory block. D is the reimplementation that must be merged, not simply deleted: it is already the thing both apps call for NIP-18 tag parsing.

Recommendation: one library module, `gn-nostr-reference` (keep the D name/ABI since both apps already link it), grown to absorb B's semantics and A's builders; delete A; reduce B to a thin gnostr-only kind->view table on top of it; move the repost/quote card presentation into a portable widget fed by an injected resolver.

## 1. Feature / behaviour diff

### gnostr originals have, reimplementation (D) lacks
- Typed errors: B returns `GNOSTR_NOSTR_TARGET_ERROR_{INVALID,REFUSED,UNSUPPORTED}` (`gnostr-nostr-target.h:55-62`); D returns bare NULL (`gn-nostr-reference.c:32-86`) so callers cannot tell "nsec refused" from "garbage" (matters for a11y/user messages).
- Event validation: B `gnostr_nostr_event_parse` (NIP-01 canonical id + Schnorr) and `gnostr_nostr_event_matches_target`. D's `embedded_event_verified` flag (`gn-nostr-reference.c:197`) trusts the caller; Groundhog passes FALSE everywhere, so kind-6 embedded originals are never used. Port B's verifier as `gn_nostr_event_verify()` so D can verify itself.
- Builders: A `gnostr_uri_build_{npub,note,nprofile,nevent,naddr}`, `gnostr_uri_to_string`, `type_to_string`, `get_bech32` (`nip21_uri.h:95-213`). D only builds nevent/naddr inside descriptor parse (`:213-229`). Needed to fix gnostr bug below.
- Case-insensitive `NOSTR:` prefix and percent-decoding (`nip21_uri.c:25-61`). D is case-sensitive (`:34`). Keep percent-decoding **only** at the dispatcher/D-Bus boundary (gnostr), not in the library parser.
- `note` entity separate type (A: `GNOSTR_URI_TYPE_NOTE`); D collapses note/nevent into EVENT - acceptable; add `gchar *entity_hrp` if callers need it.
- Repost/quote presentation: card row "reposted by" attribution, inline quote card, repost count (`nostr-note-card-row.c:5822, 5877, 5895, 6015`). Groundhog has no equivalent widget (summary text only).
- Local-cache resolution of original/quoted note + author name (factory `:1262-1340`, `:1395-1435`).

### reimplementation (D) does better - must keep
- Bounded parsing: URI <= 2048 bytes, rejects `?#/` (`gn-nostr-reference.c:33-35`); event JSON <= 256 KiB, tags <= 1024 (`:118,132`), tags_json <= 128 KiB (`:244`). A/B have no bounds.
- nsec never decoded at all (header comment; test `test_portable.c:165`).
- Relay hints copied but documented inert ("never contacted here", header).
- Strict naddr coordinate validation `kind:64hex:d` (`:107-120`), kind > 0, hex64 checks.
- `author_authenticated` distinguishes hinted vs verified author.
- Canonical shareable `uri` on every descriptor (`:207-229`).
- Unsigned template builders (`gn_nostr_build_repost_template`/`quote_template`) - no signing/relay side effects.
- No GObject/nostr-gobject singleton dependency (libnostr nip19 + json-glib only), so it can sit in `nostr_gtk_portable` with no GTK.

### Groundhog UI does better than gnostr UI - must keep
- No auto-fetch: unresolved references render an inert summary with i18n'd, a11y-friendly labels `_("Quoted Nostr note: %s")` etc. (`gh-conversation-view.c:1278-1280`); network lookup only on explicit "Find on Relays" (`gh-public-note-ui.c:255-300`), gated on writable store and cancelled when consent is withdrawn (`:144-147`), routed through GhRelayScope + GhAuthPolicy purpose `GH_AUTH_PURPOSE_CONTACT_DIRECTORY`.
- gnostr card-row strings are untranslated: `"Quoting %s"`, `"Unknown"`, `"(content unavailable)"` (`nostr-note-card-row.c:6058-6068`). Ported widget must use `gn-portable-i18n` (`nostr-gtk/CMakeLists.txt:185`).

### Bugs found in gnostr original
- `g_strdup_printf("nostr:note1%s", hex_id)` builds an invalid bech32 URI: `gnostr-timeline-view-app-factory.c:1346, 1441`; also `gnostr-chess-card.c:803`. Replace with `gn_nostr_reference_build_note()`/descriptor `->uri`.
- `nostr-note-card-row.c:35` includes app header `nip21_uri.h` for nothing - one of the reasons card row is in the "coupled" list.
- Three different NIP-19 backends (nostr-gobject GNostrNip19 in A/B/C; libnostr `nostr_nip19_*` in D) -> divergent acceptance. Consolidate on D's.

## 2. gnostr dependencies to replace with injected interfaces

| Dependency | Where | Replacement |
|---|---|---|
| `GNostrNip19` (nostr-gobject) | A, B, C | libnostr `nostr_nip19_*` via `gn_nostr_reference_parse` (already linked: `CMakeLists.txt:239`) |
| `storage_ndb_get_note_by_id_nontxn`, `storage_ndb_begin_query`, `storage_ndb_get_profile_by_pubkey` | factory `:1262-1339, 1395-1429`; embed `:313+` | `GnNostrReferenceResolver` interface (below) - gnostr implements over NDB, Groundhog over `GhStore` public notes (`gh-public-note-ui.c` `lookup()` `:75-90`) |
| relay fetch on `request-embed` | `gnostr-timeline-embed.c:313-400` | resolver `fetch_async` - *only called on explicit user action* from the widget; gnostr may call it eagerly from its own adapter (policy stays in app) |
| `gn_nostr_event_item_*`, `GnostrTimelineSnapshotRow` | factory `:980-996, 1199-1210` | app-side; stays in gnostr factory, which just passes a `GnNostrRepostDescriptor` + resolved data to the widget |
| profile display name | factory NDB profile path | resolver `display_name` callback |
| GSettings | none in A/B/D | n/a |

New public API (portable, `nostr-gtk-1.0/gn-nostr-reference.h`):

```c
/* errors (from B) */
#define GN_NOSTR_REFERENCE_ERROR (gn_nostr_reference_error_quark())
typedef enum { GN_NOSTR_REFERENCE_ERROR_INVALID,
               GN_NOSTR_REFERENCE_ERROR_REFUSED,      /* nsec, ncryptsec */
               GN_NOSTR_REFERENCE_ERROR_UNSUPPORTED } /* nrelay, other */
  GnNostrReferenceError;
GnNostrReference *gn_nostr_reference_parse_full(const gchar *uri, GError **error);
/* gn_nostr_reference_parse() stays = parse_full(uri, NULL) */

/* builders (from A) */
gchar *gn_nostr_reference_build_person(const gchar *pubkey_hex, const gchar *const *relays); /* npub if no relays else nprofile */
gchar *gn_nostr_reference_build_event(const gchar *id_hex, const gchar *author_hex, gint kind, const gchar *const *relays); /* note or nevent */
gchar *gn_nostr_reference_build_address(const gchar *author_hex, gint kind, const gchar *d, const gchar *const *relays);

/* verification (from B) */
gboolean gn_nostr_event_verify(const gchar *event_json, GError **error);          /* id + sig, bounded */
gboolean gn_nostr_reference_matches_event(const GnNostrReference *r, const gchar *event_json);
```

Resolver (portable, `nostr-gtk-1.0/gn-nostr-reference-resolver.h`, GInterface, no GTK):

```c
G_DECLARE_INTERFACE(GnNostrReferenceResolver, gn_nostr_reference_resolver, GN, NOSTR_REFERENCE_RESOLVER, GObject)
struct _GnNostrReferenceResolverInterface {
  GTypeInterface parent_iface;
  /* synchronous, local-only, must not touch the network */
  gchar *(*lookup_local)(GnNostrReferenceResolver *self, const GnNostrReference *ref); /* verified event JSON or NULL */
  gchar *(*display_name)(GnNostrReferenceResolver *self, const gchar *pubkey_hex);    /* local profile only */
  /* network; widgets call this only from an explicit user action */
  gboolean (*can_fetch)(GnNostrReferenceResolver *self);  /* FALSE = locked store / offline / no consent */
  void  (*fetch_async)(GnNostrReferenceResolver *self, const GnNostrReference *ref,
                       GCancellable *c, GAsyncReadyCallback cb, gpointer data);
  gchar *(*fetch_finish)(GnNostrReferenceResolver *self, GAsyncResult *res, GError **error);
};
```

Widget (portable GTK, 4.6/adw 1.2 safe): `GnNostrReferenceCard` (`nostr-gtk/src/gn-nostr-reference-card.c`) - replaces `quote_embed_box` construction in the card row: states INERT (summary + "Find" button only if `can_fetch`), RESOLVED (author + 3-line content), UNAVAILABLE; accessible label/description; i18n via `gn-portable-i18n`; never emits a fetch on bind. Setters: `gn_nostr_reference_card_set_descriptor(card, const GnNostrRepostDescriptor*)`, `_set_resolver(card, GnNostrReferenceResolver*)`. Card row gets `nostr_gtk_note_card_row_set_reference(row, descriptor, resolver)`; `set_quote_info`/`set_repost_info` kept as deprecated wrappers for 1.x ABI.

## 3. Groundhog requirements to preserve
- Privacy charter `docs/designs/groundhog-privacy-ux-charter-2026-09-28.md`: consent before fetch, no auto-fetch. Groundhog resolver: `lookup_local` = `GhStore` public-note cache only; `fetch_async` = existing `start_find` path (`gh-public-note-ui.c:255+`) via `GhRelayScope`/`GhAuthPolicy` (Tor/network-mode applied inside GhNet/relay layer - resolver must not open its own sockets); `can_fetch` = `writable_store()` and network mode != offline. Cancellation on store lock (`:144-147`) must map to `GCancellable`.
- No gdk-pixbuf: verified - rule `no-gdk-pixbuf` in `gnome/groundhog/tests/check_privacy.py:35, 346-347, 591` scans Groundhog `src/**` only. The ported card is text-only; avatars (if any) go through existing GdkTexture/`GnAnimatedImage` path. Also libsoup-boundary/tls rules: library code must not use libsoup (check_privacy does not scan nostr-gtk - add nostr-gtk portable sources to a scan, see §5).
- Relay hints stay inert; never auto-contacted (D header).
- i18n/a11y: summary strings at `gh-conversation-view.c:1278-1280`, `reference_summary` hook `:1469-1480`.
- Render-cache/perf: gnostr's `GnContentRenderResult` cache on items (factory `:798, 1096`) and off-screen skip (`:1542`, nostrc-nke8) must keep working - widget does no work on bind beyond `lookup_local`, and gnostr adapter keeps its `is_snapshot_row` fast path. Groundhog conversation-view perf gates (G-tests in `gnome/groundhog/CMakeLists.txt`) unchanged since Groundhog keeps its row and only adopts the card if desired.
- Existing tests: `nostr-gtk/tests/test_portable.c:147-190` (D), `gnome/groundhog/tests/ui/test_conversation_view.c:990-998`, `test-groundhog-conversation-view` (`CMakeLists.txt:1441`), `check_privacy` (`:936`), `test_privacy_e2e.c`, gnostr `test_nostr_target.c`.

## 4. File moves / build changes
1. Extend `nostr-gtk/src/gn-nostr-reference.c` + header with errors, builders, verify (port logic from `gnostr-nostr-target.c` event-verify and `nip21_uri.c:230-303` builders, re-expressed on libnostr nip19). Keep in `NOSTR_GTK_PORTABLE_SOURCES` (`nostr-gtk/CMakeLists.txt:177`).
2. Add `nostr-gtk/src/gn-nostr-reference-resolver.c` + `include/nostr-gtk-1.0/gn-nostr-reference-resolver.h` to portable sources/headers (`:164-182`).
3. Add `nostr-gtk/src/gn-nostr-reference-card.c` + header + optional `data/ui/gn-nostr-reference-card.blp` to `NOSTR_GTK_CORE_SOURCES` (`:113-121`), not the coupled list.
4. `nostr-note-card-row.c`: drop `#include "nip21_uri.h"` (`:35`); replace `quote_embed_box` (`:6015-6075`) with `GnNostrReferenceCard`; translate strings. (Step toward moving card row out of `NOSTR_GTK_GNOSTR_COUPLED_SOURCES` `:126-131`.)
5. `content_renderer.c`: replace nostr-gobject nip19 decode (`:420-450, 539-574`) with `gn_nostr_reference_parse`; drop include `:12`.
6. Delete `apps/gnostr/src/util/nip21_uri.{c,h}`; remove `apps/gnostr/CMakeLists.txt:673`.
7. `apps/gnostr/src/util/gnostr-nostr-target.c`: reimplement `gnostr_nostr_target_parse` as a thin wrapper over `gn_nostr_reference_parse_full` (keep kind->view table and desktop-file sync test there - gnostr-specific); event parse -> `gn_nostr_event_verify`. Fix header comment `:10`.
8. New `apps/gnostr/src/ui/gnostr-reference-resolver-ndb.c` implementing the resolver over `storage_ndb_*` + relay fetch moved out of `gnostr-timeline-embed.c:313-400`; factory `:1195-1445` collapses to `nostr_gtk_note_card_row_set_reference(row, nip18_descriptor, resolver)`; fix `note1%s` at `:1346, 1441` and `gnostr-chess-card.c:803`.
9. Groundhog: new `gnome/groundhog/src/ui/gh-reference-resolver.c` (wraps `gh-public-note-ui.c` lookup/start_find); `gh-conversation-view.c:1246-1280` keep parse, optionally render `GnNostrReferenceCard` with Groundhog resolver; `gh-public-note-ui.c:264,371` and `gh-public-post-dialog.c:303` switch to `parse_full` for error messages; `gh-public-note-post.c:111` unchanged.
10. Meson: none found for these components (CMake only); update `nostr-gtk-portable-1.0.pc`/install header list (`:155-172, 449-468`).
No adapter to remove on the Groundhog side (it calls D directly); the "adapter" to remove is gnostr's private NDB/embed resolution inside the factory.

## 5. Tests
- Keep: `test_portable.c` NIP-18 cases; `test_conversation_view.c:990+`; `gnostr-test-nostr-target` (now exercising the wrapper).
- Move: URI vectors from `test_nostr_target.c` (nsec/ncryptsec/nrelay refusal, relay hints, naddr) into `nostr-gtk/tests/test_portable.c` against `parse_full` error codes; event verify vectors likewise.
- Add: builder round-trips (npub/note/nprofile/nevent/naddr); bounds (2049-byte URI, 1025 tags, 128 KiB tags); case-insensitive prefix decision test; regression for `note1<hex>`; `GnNostrReferenceCard` widget test with a fake resolver asserting `fetch_async` is never called on bind/map and only after activating the Find button, and that `can_fetch=FALSE` hides it; a11y label test; Groundhog privacy e2e asserting no REQ on rendering a quote; extend `check_privacy.py` (or a nostr-gtk twin) with `no-gdk-pixbuf`/no-libsoup over `nostr-gtk/src/gn-nostr-reference*.c`.

## 6. Risks and ordering
Order: (1) extend D API + tests (no behaviour change); (2) switch content_renderer and gnostr-nostr-target to D, delete nip21_uri, fix note1 bug; (3) add resolver interface + card widget with fake-resolver tests; (4) gnostr NDB resolver, collapse factory block; (5) Groundhog resolver + optional card adoption; (6) deprecate `set_quote_info`.
Risks:
- Acceptance changes: D rejects `?`/`#`/`/`, percent-encoding and uppercase `NOSTR:` that GNostrNip19-based paths accepted - dispatcher URIs could regress; normalise at the gnostr D-Bus/CLI boundary.
- Decoder differences (libnostr vs nostr-gobject) on nevent kind=0 / naddr empty `d` (B allows `""`, D requires identifier) - decide and test; naddr with empty d is valid per NIP-01 for replaceable kinds -> D must accept `""`.
- gnostr loses implicit auto-fetch unless its adapter keeps it; that is an app policy choice - keep gnostr eager via its own resolver call, never from the widget.
- ABI: card row is installed 1.x ABI; keep old setters as wrappers.
- Perf: the widget must not parse JSON on every bind; cache resolved results in the descriptor/item.
