# MLS Group Messaging Integration into gnostr

## Overview

Integrate libmarmot (MIP-00 through MIP-04) into the gnostr GTK4/libadwaita
desktop client as a **plugin** (`mls-groups`), fully interoperable with the
whitenoise application and any MDK-compatible client.

## Architecture Decision: Plugin (not built-in)

The integration follows the existing plugin pattern (libpeas) for these reasons:

1. **Consistency** — DMs are already a plugin (`nip17-dms`), group messaging
   is a natural sibling
2. **Modularity** — Users who don't want MLS can disable the plugin
3. **Dependency isolation** — libmarmot/marmot-gobject deps are only loaded
   when the plugin is active
4. **The plugin API has everything we need** — event subscription, publishing,
   signing, sidebar items, panel widgets, storage

## Interoperability Conventions (Whitenoise/MDK)

| Convention | Value |
|---|---|
| MLS Ciphersuite | 0x0001 (X25519, AES-128-GCM, SHA-256, Ed25519) |
| Group extension type | 0xF2EE (NostrGroupData) |
| Extension version | 2 |
| Key Package event kind | 443 |
| Welcome event kind | 444 |
| Group Message event kind | 445 |
| KP Relay List kind | 10051 |
| Content encoding | base64 with `["encoding", "base64"]` tag |
| Message wrapping | NIP-44 encryption of MLS ciphertext |
| Welcome delivery | NIP-59 gift wrap (kind:1059) |
| Message delivery | Published directly (kind:445 with `h` tag) |
| Group ID routing | `h` tag = hex(nostr_group_id), 32-byte random |
| Ephemeral pubkeys | Each kind:445 event uses a fresh keypair |
| Group types | DirectMessage, Group |

## Phased Implementation Plan

### Phase 1: Plugin Scaffold + Marmot Service (Foundation)

**Goal**: Plugin loads, initializes marmot-gobject, and is ready for operations.

**Files**:
```
apps/gnostr/plugins/mls-groups/
  ├── CMakeLists.txt
  ├── mls-groups.plugin             # libpeas plugin descriptor
  ├── mls-groups-plugin.h           # Plugin type declaration
  ├── mls-groups-plugin.c           # Plugin impl (activate/deactivate)
  ├── gn-marmot-service.h           # Service singleton header
  └── gn-marmot-service.c           # MarmotGobjectClient lifecycle
```

**Details**:
- `MlsGroupsPlugin` implements `GnostrPlugin`, `GnostrEventHandler`, `GnostrUIExtension`
- `GnMarmotService` is a GObject singleton that owns the `MarmotGobjectClient`
- Uses SQLite storage backend (`marmot_storage_sqlite`) in `~/.local/share/gnostr/marmot.db`
- Service is created on plugin activate, destroyed on deactivate
- Provides `gn_marmot_service_get_default()` accessor

### Phase 2: Key Package Management

**Goal**: Auto-publish key packages on login; manage key package relay list.

**Files**:
```
apps/gnostr/plugins/mls-groups/
  ├── gn-key-package-manager.h
  └── gn-key-package-manager.c
```

**Details**:
- On login: check if user has a valid key package (query kind:443 from relays)
- If missing or expired: create via `marmot_gobject_client_create_key_package_async()`
- Sign the unsigned event via `gnostr_plugin_context_request_sign_event()`
- Publish to user's relays via `gnostr_plugin_context_publish_event_async()`
- Publish kind:10051 relay list (inbox relays for key packages)
- Auto-rotate: re-create key package when epoch changes or on timer
- Store key package references for lifecycle tracking

### Phase 3: Event Routing + Welcome Flow

**Goal**: Subscribe to MLS events, unwrap gift wraps, route to marmot.

**Files**:
```
apps/gnostr/plugins/mls-groups/
  ├── gn-mls-event-router.h
  ├── gn-mls-event-router.c
  ├── gn-welcome-manager.h
  └── gn-welcome-manager.c
```

**Details**:
- Subscribe to kind:1059 (gift wraps addressed to us)
- On gift wrap receipt: unwrap via `gnostr_nip59_unwrap_async()`
- If inner kind == 444 (welcome): `marmot_gobject_client_process_welcome_async()`
- If inner kind == 445 (group message): `marmot_gobject_client_process_message_async()`
- Welcome manager shows pending welcomes in a notification/badge
- Accept/decline UI triggers `accept_welcome_async` / `decline_welcome_async`
- On accept: call `marmot_gobject_client_accept_welcome_async()` → group-joined signal

**Message flow (kind:445)**:
- Subscribe to kind:445 events matching `h` tags of our active groups
- For each event: extract MLS ciphertext from content (NIP-44 decrypt first)
- Call `marmot_gobject_client_process_message_async()`
- On APPLICATION_MESSAGE result: emit signal with decrypted inner event
- On COMMIT result: update group state, refresh members list

### Phase 4: Group Chat UI

**Goal**: Full chat interface for MLS group conversations.

**Files**:
```
apps/gnostr/plugins/mls-groups/
  ├── ui/
  │   ├── gn-group-list-view.h      # Chat list showing all groups
  │   ├── gn-group-list-view.c
  │   ├── gn-group-list-row.h       # Row in group list
  │   ├── gn-group-list-row.c
  │   ├── gn-group-chat-view.h      # Conversation view (messages)
  │   ├── gn-group-chat-view.c
  │   ├── gn-group-message-row.h    # Message bubble widget
  │   ├── gn-group-message-row.c
  │   ├── gn-group-composer.h       # Message input area
  │   └── gn-group-composer.c
  ├── data/
  │   └── ui/
  │       ├── gn-group-list-view.ui
  │       ├── gn-group-list-row.ui
  │       ├── gn-group-chat-view.ui
  │       ├── gn-group-message-row.ui
  │       └── gn-group-composer.ui
  └── model/
      ├── gn-group-list-model.h     # GListModel of MarmotGobjectGroup
      ├── gn-group-list-model.c
      ├── gn-group-message-model.h  # GListModel of messages in a group
      └── gn-group-message-model.c
```

**Details**:
- Sidebar item: "Group Chats" with chat bubble icon, requires auth
- Group list view: GtkListView with GnGroupListRow (name, last message, badge)
- Chat view: AdwNavigationPage with message list + composer
- Message row: Author avatar, name, content, timestamp, reactions
- Composer: Text entry, send button, media attach button
- Send flow: create unsigned rumor event → `marmot_gobject_client_send_message_async()`
  → sign the kind:445 event → publish to group relays

### Phase 5: Group Management

**Goal**: Create, configure, and manage groups.

**Files**:
```
apps/gnostr/plugins/mls-groups/
  ├── ui/
  │   ├── gn-create-group-dialog.h
  │   ├── gn-create-group-dialog.c
  │   ├── gn-group-settings-view.h
  │   ├── gn-group-settings-view.c
  │   ├── gn-group-members-view.h
  │   ├── gn-group-members-view.c
  │   ├── gn-add-members-dialog.h
  │   └── gn-add-members-dialog.c
  ├── data/
  │   └── ui/
  │       ├── gn-create-group-dialog.ui
  │       ├── gn-group-settings-view.ui
  │       ├── gn-group-members-view.ui
  │       └── gn-add-members-dialog.ui
```

**Details**:
- Create group dialog: name, description, select members (from follows)
- Fetch member key packages from relays before creating group
- `marmot_gobject_client_create_group_async()` → merge commit → gift-wrap welcomes
- Send welcome gift wraps to each member
- Publish evolution event (kind:445 commit) to group relays
- Members view: list members with admin badge, add/remove buttons
- Settings view: edit name/description, manage admins, leave group
- Leave group: `marmot_leave_group()` → set Inactive locally

### Phase 6: MLS Direct Messages

**Goal**: 1-on-1 DMs via MLS (whitenoise DirectMessage group type).

**Details**:
- When starting a DM: create a 2-person MLS group (type: DirectMessage)
- Same message flow as group messages, just with 2 members
- Merge into the existing DM UI or show as a separate "Encrypted DM" section
- This provides forward secrecy for DMs (unlike NIP-17)

### Phase 7: Encrypted Media (MIP-04)

**Goal**: Share encrypted media files in groups.

**Details**:
- Use `marmot_encrypt_media()` before upload
- Upload encrypted blob to Blossom server
- Include `imeta` tag with encryption metadata (nonce, hash, epoch)
- On receive: download, `marmot_decrypt_media()`, display
- Integrates with existing blossom upload infrastructure

## Key Technical Decisions

1. **Storage**: SQLite backend (not nostrdb) for marmot — MLS state is
   independent from the Nostr event cache

2. **NIP-59**: Reuse existing `gnostr_nip59_*` functions for gift wrapping
   welcomes and (optionally) messages

3. **Kind:445 messages**: Published directly (not gift-wrapped) per MIP-03 —
   they use ephemeral pubkeys so metadata is already protected. The content
   is NIP-44 encrypted with the group's exporter secret.

4. **D-Bus signer**: Key package signing and event signing go through the
   existing signer service — no raw secret key access in the plugin

5. **Secret key for marmot**: The plugin needs the user's secret key for
   MLS credential creation. This is obtained via the signer service's
   `export_secret` capability, or via a dedicated marmot key derivation
   from the master key.

6. **Group relay management**: Each group has designated relays (from the
   NostrGroupData extension). The plugin subscribes to these relay-specific
   filters for message delivery.

## Dependency Graph

```
mls-groups-plugin
  ├── marmot-gobject-1.0 (GObject wrapper)
  │   └── libmarmot (pure C MLS + Marmot protocol)
  │       ├── libsodium
  │       ├── openssl
  │       └── libnostr
  ├── gnostr-plugin-api (plugin host services)
  ├── nostr-gtk-1.0 (shared widgets)
  └── nip59_giftwrap (gift wrap/unwrap)
```
