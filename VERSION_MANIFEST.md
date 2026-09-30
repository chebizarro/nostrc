# Component Version Manifest

This file is the repository-wide ledger for independently releasable components
covered by the versioning initiative. **Declared version** is the version in the
build metadata; it is not evidence that a release exists. **Latest release** is
updated only after the matching component-prefixed Git tag has been published.

As of 2026-08-10, the repository has no component-specific release tags.
Consequently, every component below is currently unreleased even where its build
files already declare a version.

| Component | Path | Declared version | Latest release | Release tag | Authoritative version source(s) |
| --- | --- | --- | --- | --- | --- |
| libnostr | `libnostr/` | 1.1.0 | Unreleased | — | `libnostr/CMakeLists.txt` |
| libgo | `libgo/` | 0.1.2 | Unreleased | — | `libgo/CMakeLists.txt` |
| nostr-gobject | `nostr-gobject/` | 2.1.0 | Unreleased | — | `nostr-gobject/CMakeLists.txt`, `nostr-gobject/meson.build` |
| nostr-gtk | `nostr-gtk/` | 1.0.1 | Unreleased | — | `nostr-gtk/CMakeLists.txt`, `nostr-gtk/meson.build` |
| libmarmot | `libmarmot/` | 0.10.0 | Unreleased | — | `libmarmot/CMakeLists.txt`, `libmarmot/meson.build` |
| marmot-gobject | `marmot-gobject/` | 1.4.0 | Unreleased | — | `marmot-gobject/CMakeLists.txt`, `marmot-gobject/meson.build` |
| gnostr | `apps/gnostr/` | 0.1.0 | 0.1.0-preview | `gnostr-v0.1.0-preview` | `apps/gnostr/CMakeLists.txt` |
| groundhog | `gnome/groundhog/` | 0.11.1 | Unreleased | — | `gnome/groundhog/CMakeLists.txt` |
| nostr-homed | `gnome/nostr-homed/` | 0.2.2 | Unreleased | — | `gnome/nostr-homed/CMakeLists.txt`, `gnome/nostr-homed/meson.build`, `gnome/nostr-homed/nostr-homed.pc.in` |
| NIP-46 client/provider | `nips/nip46/` | Unversioned | Unreleased | — | None; authoritative version ownership must be established before release |
| nip19 (NIP-19 codec) | `nips/nip19/` | 0.1.0 | Unreleased | — | `nips/nip19/CMakeLists.txt` (`declare_component_version`; SONAME `libnip19.so.0`) |
| nip34 (NIP-34 git events) | `nips/nip34/` | 0.1.0 | Unreleased | — | `nips/nip34/CMakeLists.txt` (`declare_component_version`; SONAME `libnip34.so.0`) |
| nip55l (Linux signer) | `nips/nip55l/` | 0.5.1 | Unreleased | — | `nips/nip55l/include/nostr/nip55l/signer_ops.h` (`NOSTR_NIP55L_VERSION_*`) |
| nostr-seal | `gnome/nostr-seal/` | 0.1.0 (format nsealed v1) | Unreleased | — | `gnome/nostr-seal/src/main.c` (`NOSTR_SEAL_VERSION`), `gnome/nostr-seal/include/nostr-seal.h` (`NSEAL_FORMAT_VERSION`) |

## Recorded version decisions

Decisions for components affected by another component's change (AGENTS.md,
"Updating versions", step 5).

| Change | Component | Declared | Decision |
| --- | --- | --- | --- |
| libmarmot 0.3.6 -> 0.4.0 (MINOR: breaking LeafNodeTBS wire change, nostrc-2io4; plus nostrc-lz4f, -va60, -5q55, -8u1k) | libmarmot | 0.4.0 | MINOR bump (0.x breaking wire change; migration notes in `libmarmot/README.md`). |
| same | marmot-gobject | 1.1.0 | No bump: no source, API or ABI change. It links libmarmot statically, so its next (first) 1.1.0 release embeds 0.4.0 and must carry libmarmot's wire-compatibility note. |
| same | gnostr | 0.1.0 | No bump: 0.1.0 is not yet released (only `gnostr-v0.1.0-preview`), so the statically linked libmarmot 0.4.0 and the mls-groups `group-error` signal/toast ship in 0.1.0. Its release notes must say that 0.1.0 cannot follow path Commits from the 0.1.0-preview (libmarmot 0.1.0). |
| same | groundhog | 0.9.0 | MINOR in the same wave for G09 Tor and G20b relay groups; the libmarmot change itself only needs a rebuild. Groundhog links libmarmot statically, but its Marmot app integration is not live (nostrc-qp24.13) and `GhStoreMarmot` stores opaque state, so only a rebuild is needed. |
| libnostr 1.0.8 -> 1.0.9 (PATCH: `nostr_envelope_serialize_compact()` REQ/COUNT frames keep their closing `]`; `event_envelope_marshal_json()` escapes the subscription id; nostrc-ptwq) | libnostr | 1.0.9 | PATCH: bug fix, no API or ABI change. |
| same | groundhog | 0.9.0 | No bump for this change: the libsoup transport now builds its REQ with the library serializer instead of by hand, which puts the same bytes on the wire. |
| groundhog 0.9.0 -> 0.9.1 (PATCH: a build without G09 fails closed on network-mode tor or an unknown mode, and never dials .onion; nostrc-6v0i) | groundhog | 0.9.1 | PATCH: privacy bug fix (charter P5). The new `tor-unavailable` status input, banner and Preferences note are app-internal, not a public surface. |
| libmarmot 0.4.0 -> 0.4.1 (PATCH: Welcome GroupSecrets.path_secret sent and applied, nostrc-il4i) | libmarmot | 0.4.1 | PATCH bump: RFC 9420 conformance fix, no API/ABI or state-format change. Wire-compatible both ways: 0.4.0 joiners ignore the new `path_secret` (and keep the old inability to follow some Commits); 0.4.1 joiners accept 0.4.0 Welcomes without it. |
| same | marmot-gobject | 1.1.0 | No bump: no source, API or ABI change; rebuild picks up the fix. |
| same | gnostr, groundhog | 0.1.0, 0.9.1 | No bump: rebuild only (static link, no API change). |
| libgo 0.1.1 -> 0.1.2, libnostr 1.0.9 -> 1.0.10 (PATCH: MPMC channel slots no longer lose an element claimed but not yet filled, capacity-1 rings get two slots; libnostr re-arms a WebSocket write that raced the pending flag; nostrc-75rv) | libgo, libnostr | 0.1.2, 1.0.10 | PATCH: data-loss bug fixes, no API/ABI change. libnostr takes 1.0.10 because nostrc-ptwq already claimed 1.0.9. |
| groundhog 0.9.1 -> 0.10.0 (MINOR: NIP-17 multi-recipient send, nostrc-qp24.78; encrypted attachments core with receive, cache binding and purge, G21 nostrc-qp24.39; absorbs 0.9.1) | groundhog | 0.10.0 | MINOR: new user-visible capability (group DMs). 0.9.1 was never pushed on its own. |
| libmarmot 0.4.1 -> 0.5.0 (MINOR: `marmot_update_group_metadata` gains `out_commit_json`; kind:445 Commits NIP-44-encrypted and ingested by `marmot_process_message`, nostrc-9ata) | libmarmot | 0.5.0 | MINOR bump: 0.x breaking API change (new required parameter) plus a new capability and a wire change of Commit events; migration notes in `libmarmot/README.md`. |
| same | marmot-gobject | 1.2.0 | MINOR bump: new `marmot_gobject_client_update_group_metadata_async/_finish` and `MarmotGobjectClient::group-updated` signal (backward compatible). |
| same | gnostr | 0.1.0 | No bump: 0.1.0 is unreleased. The mls-groups plugin gains an admin-only group rename that publishes its Commit to the group relays, and refreshes views from the client's `group-updated` (the router's post-Commit lookup used the nostr_group_id as an MLS group id and never found the group). |
| same | groundhog | 0.10.0 | No bump beyond 0.10.0 (unreleased). GhStoreMarmot snapshots now also cover the `mls_group_parent` label; test callers follow the new signature. |
| libmarmot 0.5.0 review fixes (W17 review B1, B2, N2, N3, N5; still unreleased 0.5.0) | libmarmot | 0.5.0 | No further bump: folded into the unreleased 0.5.0 MINOR. Adds `marmot_clear_pending_commit()`; producers now leave a pending Commit that `marmot_merge_pending_commit()` applies after a relay OK; every kind:445 (`marmot_create_message()` too) is signed by a fresh ephemeral key; multi-member add/remove/create are one Commit (nostrc-wc6v); new `mls_kv` label `mls_group_pending`. Migration notes in `libmarmot/README.md`. |
| same | marmot-gobject | 1.2.0 | No further bump: folded into unreleased 1.2.0 (adds `merge_pending_commit_async/_finish`, `clear_pending_commit_async/_finish`; `update_group_metadata` no longer emits ::group-updated before the merge). |
| same | gnostr | 0.1.0 | No bump (unreleased): add-member and rename Commits are merged only after a group relay's OK and cleared otherwise, with the outcome shown in the view. |
| same | groundhog | 0.10.0 | No bump (unreleased): GhStoreMarmot snapshots also cover `mls_group_pending`; tests merge their Commits. |
| libmarmot 0.5.0 re-review fixes (W17b R1, R2; still unreleased 0.5.0) | libmarmot | 0.5.0 | No further bump: folded into unreleased 0.5.0. Pending record v2 bound to its parent state and carrying the signed event and Welcomes; merge on our own relay echo; idempotent merge; new `marmot_get_pending_commit()`, `marmot_get_unsent_welcomes()`, `marmot_mark_welcomes_sent()`, `MarmotUnsentWelcome`; new `mls_kv` label `mls_group_welcomes`. |
| same | marmot-gobject | 1.2.0 | No further bump (unreleased 1.2.0): adds `get_pending_commit`, `get_unsent_welcomes`, `mark_welcomes_sent`. |
| same | gnostr | 0.1.0 | No bump (unreleased): pending Commits resolved by a plugin-level resolver (publish until a relay OK, clear only when every relay refused, keep and retry when uncertain, resolve leftovers and unsent Welcomes at startup). |
| same | groundhog | 0.10.0 | No bump (unreleased): GhStoreMarmot snapshots also cover `mls_group_welcomes`. |
| libmarmot 0.5.0 addendum fixes (W17b C1, C2, N1; still unreleased 0.5.0) | libmarmot | 0.5.0 | No further bump: folded into unreleased 0.5.0. Documented that a `Marmot` is not thread-safe; Welcome outbox append-only with stable ids (`MarmotUnsentWelcome.id`), `marmot_mark_welcomes_sent()` now takes the ids to remove (outbox record v2); a duplicate Welcome for a group already joined is refused (`MARMOT_ERR_WELCOME_ALREADY_ACCEPTED`). |
| same | marmot-gobject | 1.2.0 | No further bump (unreleased 1.2.0): every libmarmot call serialized per client; new `marmot_gobject_client_lock/unlock`; `get_unsent_welcomes` returns ids and `mark_welcomes_sent` takes them. |
| same | gnostr | 0.1.0 | No bump (unreleased): direct libmarmot calls hold the client lock; each Welcome is marked sent after its own send; resolver coalesces per group, backs off exponentially with jitter, stops on deactivation, and flushes the outbox on every group update. |
| W17b addendum 2 (D1 and low items; still unreleased) | marmot-gobject | 1.2.0 | No further bump: client signals are posted as idle sources to the context captured at construction instead of `g_main_context_invoke()` from workers holding the lock (no API change; documented on `marmot_gobject_client_new()`). |
| same | libmarmot | 0.5.0 | No further bump: an uncomputable Welcome id fails the outbox operation (`MARMOT_ERR_MEMORY`) instead of using an all-zero id. |
| same | gnostr | 0.1.0 | No bump (unreleased): failed Welcome sends are retried on their own backoff timer, cancelled on deactivation. |
| libnostr 1.0.10 -> 1.0.11 (PATCH: the shared libwebsockets client context keeps no TLS session cache and asks for no TLS 1.2 tickets, so no connection offers or resumes another's session, nostrc-0d0d; frames still queued in a released connection's recv_channel are freed, nostrc-lpvj) | libnostr | 1.0.11 | PATCH: privacy and leak fixes, no API/ABI change (the new `nostr_connection_recv_channel_free()` is in the private `connection-private.h`). Every reconnect now makes a full TLS handshake. |
| same | nostr-gobject, gnostr, groundhog | 2.0.2, 0.1.0, 0.10.0 | No bump: no source or API change (Groundhog's gh-net-tls.h only gains a comment); a rebuild or relink picks the fix up, as it does for signet. |
| groundhog 0.10.0 -> 0.10.1 (PATCH: attachment downloads refuse non-public addresses including embedded-IPv4 IPv6 forms and names resolving to private addresses, nostrc-qi5e; picks up libnostr 1.0.11 TLS no-resumption) | groundhog | 0.10.1 | PATCH: privacy fixes, no new feature. |
| groundhog 0.10.1 -> 0.11.0 (MINOR: attachment UI G22 nostrc-qp24.40; pins, mark read/unread, header menu, backfill notifications, timer rows, arrival-order read marker with store schema v4, multi-send follow-ups; W18 polish) | groundhog | 0.11.0 | MINOR: new user-visible features and a store schema migration. |
| Linux pre-push gate fixes (nostrc-y9xg): libgo's no-op metrics stubs become weak, so GNU ld no longer reports duplicate definitions when gnostr and gnostr-live-log link both libnostrgo.a and libnostr.a | libgo | 0.1.2 | No bump: 0.1.2 is unreleased, and the change is link-time only (no API/ABI change); the fix ships in 0.1.2. |
| same (gnostr's Linux link now succeeds; its image-viewer test links libm; the mls-groups plugin casts its storage to the interface type, identical code) | gnostr | 0.1.0 | No bump (unreleased): build corrections with no behaviour change. |
| libmarmot 0.5.0 -> 0.6.0 (MINOR: `marmot_process_message()` verifies the kind:445 id and signature before decrypting; new `marmot_process_rumor_message()` for gift-wrapped rumors; nostrc-6r6s) | libmarmot | 0.6.0 | MINOR bump: new API, and a 0.x behaviour break (unsigned or mis-signed events are now `MARMOT_ERR_SIGNATURE` / `MARMOT_ERR_EVENT`, not processed). Migration notes in `libmarmot/README.md`. |
| same | marmot-gobject | 1.3.0 | MINOR bump: new `marmot_gobject_client_process_rumor_message_async/_finish` (backward compatible); `process_message_async` now gets the verifying libmarmot path. |
| same | gnostr | 0.1.0 | No bump (unreleased): the mls-groups gift-wrap route sends kind:445 rumors to the rumor path; the relay route is unchanged (nostrdb already verified). |
| same | groundhog | 0.11.0 | No bump: the application does not call libmarmot yet (qp24.13); only GhStoreMarmot tests do, with signed events. |
| libmarmot 0.6.0 -> 0.7.0 (MINOR: optional `MarmotStorage` `begin`/`commit`/`rollback` hooks, every writing operation one transaction; late application messages read with the retained parent; nostrc-qp24.7) | libmarmot | 0.7.0 | MINOR bump: additive storage-interface capability (the struct grew at its end, so custom backends must be rebuilt) plus new behaviour (late messages of the previous epoch are delivered; a received message whose ratchet step cannot be stored fails closed). Built-in backends unchanged. Migration notes in `libmarmot/README.md`. |
| same | marmot-gobject | 1.3.0 | No bump: no source, API or ABI change; a rebuild picks up the new libmarmot behaviour. |
| same | gnostr | 0.1.0 | No bump (unreleased): rebuild only. Its SQLite-backed store has no transaction hooks yet (nostrc-wf71). |
| same | groundhog | 0.11.0 | No bump: GhStoreMarmot's transaction hooks and the new GTK-free Commit lifecycle (`gh-mls-commits`) are not linked into the application until nostrc-qp24.13. `gh_store_begin_named()` and the `mls:*` cut points (test builds only) add no behaviour to the executable. |
| libmarmot 0.7.0 -> 0.8.0 (MINOR, security: the MLS sender ratchets and a bounded skipped-key cache are stored with the group state, serial format 3; consumed secrets are no longer stored; formats 1 and 2 migrate on load; nostrc-ai04) | libmarmot | 0.8.0 | MINOR bump: 0.x incompatible storage-format change (0.7.0 cannot read format 3) and a security fix (per-epoch AES-GCM key reuse, no in-epoch forward secrecy, replays accepted). No public API change. Security advisory and migration notes in `libmarmot/README.md`. |
| same | marmot-gobject | 1.3.0 | No bump: no source, API or ABI change (audit: every send and receive goes through `marmot_create_message()`/`marmot_process_message()` under the client lock). A new test covers distinct generations and refused replays through the client; a rebuild picks up the fix. |
| same | gnostr | 0.1.0 | No bump (unreleased): rebuild only. Audit: the mls-groups plugin sends through the client and holds the client lock for its direct libmarmot calls; MIP-04 media uses random nonces, not a reloaded counter. |
| same | groundhog | 0.11.0 | No bump: test-only change (a GhStoreMarmot crash case cutting `marmot_create_message()` at every write and commit). GhStoreMarmot stores the state opaquely; the format migrates inside libmarmot. |
| libmarmot 0.8.0 -> 0.9.0 (MINOR, security: application messages carry an RFC 9420 s6.3.1 signed PrivateMessageContent and s6.3.2 SenderDataAAD; the inner event's pubkey must be the MLS sender leaf's identity; duplicates by inner event id; nostrc-we6g. Also the 0.8.0 advisory corrected, review B1) | libmarmot | 0.9.0 | MINOR bump: 0.x incompatible wire change of application messages (0.9.0 and <= 0.8.0 cannot read each other's) and a security fix (members could impersonate each other). No public API change. Advisory and compatibility notes in `libmarmot/README.md`. |
| same | marmot-gobject | 1.3.0 | No bump: no source, API or ABI change. It links libmarmot statically: its next release embeds 0.9.0 and must carry the wire note (upgrade whole groups together). `send_message_async` now fails with `MARMOT_ERR_AUTHOR_MISMATCH` for an inner event authored by another key, and `process_message_async` for a forged author. |
| same | gnostr | 0.1.0 | No bump (unreleased): rebuild only. The mls-groups plugin already sets the inner pubkey to the account its KeyPackages and groups use. |
| same | groundhog | 0.11.0 | No bump: documentation in `gh-store-marmot.h`/`gh-mls-commits.h` (review N1: publish only after the outer transaction commits); the application does not send libmarmot messages yet (qp24.13). |
| libmarmot 0.9.0 -> 0.10.0 (MINOR, security: every member leaf carries marmot.member.account-identity-proof.v2 in a LeafNode app_data_dictionary, and receivers require it on each Commit's added or re-filled leaves and on every Welcome-tree leaf except their own and the Welcome sender's; account-proof enrollment API; Welcome rumors carry the sender's pubkey; `MarmotConfig.allow_unproven_members` legacy opt-in; nostrc-7vyi) | libmarmot | 0.10.0 | MINOR bump: 0.x wire change (KeyPackage leaves carry the proof, `mls_extensions` adds 0x0006; 0.10.0 refuses unproven KeyPackages, Adds and Welcome-tree leaves from MDK 0.8 and libmarmot <= 0.9.0 unless in legacy mode), new public API (`marmot_account_proof_template()`, `marmot_set_account_proof()`, `marmot_has_account_proof()`), ABI change (`MarmotConfig` grows) and a security fix (an admin could add a leaf claiming another account and post as it). State format unchanged. Advisory and compatibility notes in `libmarmot/README.md`. |
| same | marmot-gobject | 1.4.0 | MINOR bump: new `marmot_gobject_client_get_account_proof_template()`, `_set_account_proof()` and `_has_account_proof()` (backward compatible). `create_key_package_unsigned_async` now fails with `MARMOT_ERR_KEY_PACKAGE_IDENTITY` until the client is enrolled. |
| same | gnostr | 0.1.0 | No bump (unreleased): the mls-groups KeyPackage manager enrolls the account proof (one kind:450 signing request through the signer, never published) before its first KeyPackage. Welcome rumors from libmarmot now carry the pubkey that Gnostr's NIP-59 unwrap requires. |
| same | groundhog | 0.11.0 | No bump: rebuild only, no source change. Groundhog's tests create groups without an account proof and admit members only through the creator, whose leaf the Welcome's sender rule accepts. |
| libmarmot 0.10.0: the retained parent retires once no competing Commit can win (nostrc-yuj2, security; still unreleased 0.10.0) | libmarmot | 0.10.0 | No further bump: folded into the unreleased 0.10.0 MINOR. The `mls_group_parent` record keeps version 1 and gains a trailer (tier, pending leaves); records without it load, and 0.9.0 cannot read records with it (a state-format change, covered by the MINOR). No public API change. Policy and exposure notes in `libmarmot/README.md`. |
| same | marmot-gobject, gnostr, groundhog | 1.4.0, 0.1.0, 0.11.0 | No bump: rebuild only, no source change. GhStoreMarmot stores the record opaquely; Groundhog's tests read only its unchanged version and epoch prefix. |
| libmarmot 0.10.0 review fixes (W20 B1: the inviter refuses an Add whose Welcome its joiners must reject; `marmot_create_group()` needs an enrollment outside legacy mode; new `marmot_group_account_proof_template()` and `marmot_self_update()` prove an existing leaf, nostrc-rgb5, nostrc-yd0q; still unreleased 0.10.0) | libmarmot | 0.10.0 | No further bump: folded into the unreleased 0.10.0 MINOR (new API, behaviour change for unenrolled creators and for groups with unproven leaves). Notes in `libmarmot/README.md`. |
| same | marmot-gobject | 1.4.0 | No further bump: no source change (its tests enroll the creator). |
| same | gnostr | 0.1.0 | No bump (unreleased): `GnMarmotService` enrolls once through the signer, shared by the KeyPackage manager, DM creation, the create-group dialog and invites, which wait for it and say so. |
| same | groundhog | 0.11.0 | No bump: test-only change (the store tests' actors enroll their account proof, as the reviewer suggested). |
| libmarmot 0.10.0 review fix W20 N1 (`marmot_process_welcome_from()`: the Welcome's sender is the caller-verified NIP-59 seal author; still unreleased 0.10.0) | libmarmot, marmot-gobject | 0.10.0, 1.4.0 | No further bump: additive API folded into the unreleased MINORs (marmot-gobject adds `process_welcome_from_async`). Gnostr (unreleased 0.1.0) passes the seal author from its unwrap. |
| libnostr 1.0.11 -> 1.1.0 (MINOR: new `nostr_relay_set_state_callback_full()`, `nostr_relay_set_auth_callback_full()`, `nostr_relay_set_ok_callback_full()` and the `NostrRelayDestroyNotify` / `NostrRelayOkResponseCallback` typedefs; registered callbacks are refcounted slots, so a callback's user data outlives any call already in progress when it is replaced or removed; the OK setter now takes the relay mutex; nostrc-flp7) | libnostr | 1.1.0 | MINOR: backward-compatible new public API. The existing setters keep their signatures and semantics (the relay never owns their user data); the private `NostrRelayPrivate` callback fields changed, which is not ABI. |
| same (nostrc-flp7: GNostrRelay finalize no longer frees its core-callback data before detaching the callbacks; the relay registry holds weak references; finalize no longer reads the core relay's connection unlocked; GNostrRelay and GNostrPool auth handlers, event sink and cache query user data are refcounted so a worker or relay still using them keeps them alive) | nostr-gobject | 2.0.3 | PATCH: use-after-free fixes, no API change. Requires libnostr >= 1.1.0. User data given to `gnostr_pool_set_auth_handler()`, `_set_event_sink()`, `_set_cache_query()` and `gnostr_relay_set_auth_handler()` may now be destroyed later (once the last relay or running query lets go) and on that thread; every in-tree caller passes NULL. |
| same | gnostr, groundhog, signet | 0.1.0, 0.11.0, unversioned | No bump: no source change; a rebuild picks up the fix. The same detach-then-free pattern in signet's relay pool, nostr-dispatcher's nd-fetch and nip46's client_start is left to follow-up beads (they can move to the `_full` setters). |
| nostr-gobject 2.0.3 -> 2.1.0 (MINOR: `GNostrSubscription:lossless`, `gnostr_subscription_set_lossless()` / `_get_lossless()`; lossless is the default and never drops an event, the nostrc-75o3 200-event drop-oldest queue becomes the opt-in bounded mode; GNostrPool's `gnostr_pool_subscribe()` and `gnostr_pool_subscribe_multi()` select bounded; nostrc-dha5) | nostr-gobject | 2.1.0 | MINOR: new public API. Behaviour change for direct `gnostr_subscription_new()` users (no more drops; a burst waits in memory); pool subscriptions behave as before. 2.0.3 (nostrc-flp7) was never released, so both ship in 2.1.0. |
| same | groundhog | 0.11.1 | PATCH: GhRelayScope selects lossless explicitly; backfills over 200 events (DM inbox, NIP-29 history, MLS kind 445) no longer lose events. |
| same | gnostr | 0.1.0 | No bump (unreleased) and no behaviour change: it subscribes through GNostrPool, which keeps the bounded queue. |
| libnostr 1.1.0 service-thread fix (nostrc-flp7, hosted run 36647380306: SEGV in `__lws_sul_insert` <- `lws_service`): a dial that connects but is hung up on before the upgrade gets only CLOSED_CLIENT_HTTP and WSI_DESTROY from lws 4.3; both now clear the connection's wsi and fail its handshake, so the owner's close no longer calls `lws_wsi_close()` on a freed wsi; no `ci.pwsi`; the close handler no longer arms a timer with `lws_set_timer_usecs(wsi, 0)` | libnostr | 1.1.0 | No further bump: folded into the unreleased 1.1.0 (a PATCH-level fix, no API change). Such a dial now fails at once instead of after the handshake timeout. |
| libmarmot 0.10.0: additive `marmot_get_group_members()` for Groundhog's MLS service (nostrc-qp24.13 part 1) | libmarmot | 0.10.0 | No further bump: additive API folded into unreleased 0.10.0. Groundhog 0.11.1 ships GhMlsService behind GH_FEATURE_ENCRYPTED_GROUPS=0 (no user-visible change). |

## Maintenance

Follow the versioning policy and update procedure in `AGENTS.md`. In
particular:

- Change a component's declared version here in the same commit as all of its
  authoritative build sources.
- Keep **Latest release** and **Release tag** unchanged for unreleased work.
- When publishing a release, use the tag
  `<component>-v<MAJOR>.<MINOR>.<PATCH>[-<PRERELEASE>]` (for example,
  `libnostr-v1.2.3` or `gnostr-v0.1.0-preview`) and then record that exact
  release version and tag here. Prereleases retain the base declared version
  in their authoritative build sources.
- Before publishing, inspect existing tags with
  `git tag --list '<component>-v*' --sort=-version:refname` and verify every
  listed source agrees with **Declared version**.
- A component marked **Unversioned** must gain an authoritative SemVer source
  before its first release.

## Automation status

Manifest maintenance is currently manual: component versions are fragmented
between build systems, and two components have no explicit version source. The
planned `scripts/tag_release.py` automation must treat this manifest as the
release ledger, reject mismatches between the manifest and authoritative
sources, and update the release columns when it creates a tag.
