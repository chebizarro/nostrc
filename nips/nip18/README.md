# NIP-18: Reposts

Implementation of [NIP-18](https://github.com/nostr-protocol/nips/blob/master/18.md) for handling reposts in Nostr.

## Overview

NIP-18 defines two event kinds for reposts:
- **Kind 6**: Repost of kind 1 (text note) events
- **Kind 16**: Generic repost for any other kind

Additionally, this library supports **quote reposts** using the `q` tag in kind 1 events.

## API

### Creating Reposts

```c
#include <nostr/nip18/nip18.h>

// Create a kind 6 repost from an existing event
NostrEvent *repost = nostr_nip18_create_repost(original_event, "wss://relay.example.com", true);

// Create a kind 6 repost from raw event ID and pubkey
NostrEvent *repost = nostr_nip18_create_repost_from_id(event_id, author_pk, relay_hint, event_json);

// Create a kind 16 generic repost
NostrEvent *repost = nostr_nip18_create_generic_repost(article_event, relay_hint, true);
```

### Parsing Reposts

```c
NostrRepostInfo info;
int rc = nostr_nip18_parse_repost(repost_event, &info);
if (rc == 0) {
    // info.repost_event_id - 32-byte binary ID of reposted event
    // info.repost_pubkey - 32-byte binary pubkey of original author
    // info.repost_kind - kind of reposted event (1 for kind 6, varies for kind 16)
    // info.relay_hint - optional relay URL
    // info.embedded_json - optional embedded event JSON
}
nostr_nip18_repost_info_clear(&info);
```

### Checking Event Types

```c
if (nostr_nip18_is_repost(event)) {
    // Event is kind 6 or 16
}

if (nostr_nip18_is_note_repost(event)) {
    // Event is kind 6 (note repost)
}

if (nostr_nip18_is_generic_repost(event)) {
    // Event is kind 16 (generic repost)
}
```

### Quote Reposts

Quote reposts are kind 1 events with a `q` tag referencing another event:

```c
// Add a quote tag to an event
NostrEvent *quote = nostr_event_new();
nostr_event_set_kind(quote, 1);
nostr_event_set_content(quote, "This is a great post!");
nostr_nip18_add_q_tag(quote, quoted_event_id, relay_hint, author_pk);

// Check if an event has a quote
if (nostr_nip18_has_quote(event)) {
    NostrQuoteInfo info;
    nostr_nip18_get_quote(event, &info);
    // info.quoted_event_id - binary ID of quoted event
    // info.quoted_pubkey - binary pubkey of quoted author
    // info.relay_hint - optional relay URL
    nostr_nip18_quote_info_clear(&info);
}
```

## Event Structure

### Kind 6 (Note Repost)

```json
{
  "kind": 6,
  "content": "", // or JSON of reposted event
  "tags": [
    ["e", "<reposted-event-id>", "<relay-hint>"],
    ["p", "<original-author-pubkey>"]
  ]
}
```

### Kind 16 (Generic Repost)

```json
{
  "kind": 16,
  "content": "", // or JSON of reposted event
  "tags": [
    ["e", "<reposted-event-id>", "<relay-hint>"],
    ["p", "<original-author-pubkey>"],
    ["k", "<reposted-event-kind>"]
  ]
}
```

### Quote Repost (Kind 1 with q-tag)

```json
{
  "kind": 1,
  "content": "My commentary on this note...",
  "tags": [
    ["q", "<quoted-event-id>", "<relay-hint>", "<quoted-author-pubkey>"]
  ]
}
```

## Building

The library is built as part of the nips collection:

```bash
mkdir build && cd build
cmake ..
make nip18
```

## Testing

```bash
make test_nip18
./test_nip18
```
