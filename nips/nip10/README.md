# NIP-10 Helpers

Canonical, thin-API-free helpers for NIP-10 thread tag handling.

## APIs

- `int nostr_nip10_add_marked_e_tag(NostrEvent *ev, const unsigned char event_id[32], const char *relay_opt, NostrEMarker marker, const unsigned char *author_pk_opt);`
- `int nostr_nip10_ensure_p_participants(NostrEvent *reply_ev, const NostrEvent *parent_ev);`
- `int nostr_nip10_get_thread(const NostrEvent *ev, NostrThreadContext *out);`

Where:

- `NostrEMarker` is `{ NOSTR_E_MARK_NONE=0, NOSTR_E_MARK_ROOT, NOSTR_E_MARK_REPLY }`.
- `NostrThreadContext` contains:
  - `bool has_root, has_reply;`
  - `unsigned char root_id[32], reply_id[32];`

## Usage Examples

### Add a marked e-tag
```c
#include "nostr/nip10/nip10.h"
#include "nostr-event.h"

NostrEvent *ev = nostr_event_new();
unsigned char id[32] = {0};
// ... fill id ...
nostr_nip10_add_marked_e_tag(ev, id, NULL, NOSTR_E_MARK_ROOT, NULL);
nostr_nip10_add_marked_e_tag(ev, id, "wss://relay.example", NOSTR_E_MARK_REPLY, NULL);
```

### Ensure p participants on a reply
```c
#include "nostr/nip10/nip10.h"
#include "nostr-event.h"
#include "nostr-tag.h"

NostrEvent *parent = nostr_event_new();
nostr_event_set_pubkey(parent, "<hex pubkey>");
NostrTag *pt = nostr_tag_new("p", "<peer pubkey>", "wss://relay", NULL);
nostr_event_set_tags(parent, nostr_tags_new(1, pt));

NostrEvent *reply = nostr_event_new();
nostr_nip10_ensure_p_participants(reply, parent);
```

### Extract thread context from an event
```c
#include "nostr/nip10/nip10.h"

NostrThreadContext ctx;
memset(&ctx, 0, sizeof ctx);
nostr_nip10_get_thread(ev, &ctx);
if (ctx.has_root) { /* use ctx.root_id (32 bytes) */ }
if (ctx.has_reply) { /* use ctx.reply_id (32 bytes) */ }
```

## Notes

- Functions are canonical (`nostr_*`) and live under `nips/nip10/`.
- `nostr_nip10_add_marked_e_tag()` ensures uniqueness via `nostr_tags_append_unique()`.
- When `relay_opt == NULL`, markers are added at index 3 (with an empty relay at index 2) to comply with NIP-10 position.
- `nostr_nip10_get_thread()` decodes 64-hex IDs to 32-byte binary using `nostr_hex2bin`.

## Building & Tests

- The module builds as `nip10` and installs headers under `include/nostr/nip10/`.
- Unit tests live in `nips/nip10/tests/test_nip10.c` and are wired into CTest as `test_nip10`.
