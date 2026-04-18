# NIP-XXX: Personal Contacts (kind 30085)

**Status**: Application-specific draft (not yet submitted as a NIP)

## Motivation

NIP-02 follow lists and kind-0 profiles cover self-profiles and social graph
relationships, but not a general-purpose address book. GNOME desktop users
expect their contacts (name, email, phone, organization) to sync across
devices. This kind maps vCard 4.0 contacts to Nostr events so that a local
CardDAV bridge can present them to GNOME Contacts / Evolution Data Server.

## Kind

**30085** — parameterized-replaceable event.

## Content

UTF-8 vCard 4.0 payload (no line-folding). This is the full vCard as stored
by the user. Not encrypted at rest on relays (contacts are personal but not
sensitive enough to justify NIP-44 overhead for v1).

## Tags

| Tag | Required | Description |
|-----|----------|-------------|
| `d` | Yes | Stable UUID (matches vCard UID) |
| `name` | Yes | Full name (FN property) for relay-side filtering |
| `t` | No | Always `"contact"` — type marker for filtering |
| `p` | No | npub of the contact if they have a known Nostr identity |

## Example

### vCard Input

```vcard
BEGIN:VCARD
VERSION:4.0
UID:550e8400-e29b-41d4-a716-446655440000
FN:Alice Nakamoto
N:Nakamoto;Alice;;;
EMAIL;TYPE=work:alice@example.com
TEL;TYPE=cell:+1-555-0123
ORG:Nostr Foundation
TITLE:Protocol Engineer
ADR;TYPE=work:;;123 Lightning Lane;Bitcoin City;BC;10001;US
URL:https://alice.example.com
NOTE:Met at Nostr conference 2026
END:VCARD
```

### Nostr Event

```json
{
  "kind": 30085,
  "content": "BEGIN:VCARD\nVERSION:4.0\nUID:550e8400-e29b-41d4-a716-446655440000\nFN:Alice Nakamoto\nN:Nakamoto;Alice;;;\nEMAIL;TYPE=work:alice@example.com\nTEL;TYPE=cell:+1-555-0123\nORG:Nostr Foundation\nTITLE:Protocol Engineer\nADR;TYPE=work:;;123 Lightning Lane;Bitcoin City;BC;10001;US\nURL:https://alice.example.com\nNOTE:Met at Nostr conference 2026\nEND:VCARD",
  "tags": [
    ["d", "550e8400-e29b-41d4-a716-446655440000"],
    ["name", "Alice Nakamoto"],
    ["t", "contact"]
  ],
  "created_at": 1776000000
}
```

## Test Vectors

See `gnome/nostr-dav/testdata/` for round-trip test vectors:

1. **Simple contact** — name + email only
2. **Full contact** — all supported vCard properties
3. **Contact with Nostr identity** — includes `p` tag
4. **Unicode contact** — non-ASCII name and address
5. **Minimal contact** — only UID and FN (required minimum)

## Deletion

Deleting a contact emits a kind-5 deletion event targeting the `d` tag
(standard NIP-09 deletion for parameterized-replaceable events):

```json
{
  "kind": 5,
  "tags": [
    ["a", "30085:<pubkey>:550e8400-e29b-41d4-a716-446655440000"]
  ]
}
```

## Security Considerations

- Contacts are stored in cleartext on relays. Users should be aware that
  their address book is visible to relay operators.
- For v2, consider NIP-44 encryption of the `content` field with the
  user's own key (self-encryption). This would require a flag tag to
  signal encrypted vs cleartext content.
- The `name` tag is intentionally cleartext even if content is encrypted,
  to allow relay-side filtering without decryption.

## Compatibility

- This kind is application-specific until adopted by at least one other client.
- Clients that don't understand kind 30085 will ignore these events.
- The vCard content is standard and can be exported/imported by any CardDAV client.
