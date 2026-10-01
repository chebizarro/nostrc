//! nostrc W24b slice H throwaway fixture emitter for MDK v0.11.0
//! (git 946e0547485c9a2c393c2048ec3a968fd50fb441). NOT part of MDK; never vendored.
//!
//!   emit <out.json>
//!
//! Two captures of adopted-profile Commits for libmarmot's Commit processor
//! (nostrc-qp24.5.1.3).  In both, one member is an observer whose
//! KeyPackage private keys are recorded: libmarmot joins as that member and
//! follows the group.
//!
//! - `mdk_engine`: real MDK v0.11.0 `cgka-engine` instances (the engine's
//!   default components plus Nostr routing; SelfRemove registered as an
//!   Optional feature so every leaf advertises it, as the group must for a
//!   SelfRemove, without the group requiring it).  A sequence of kind:445
//!   events from MDK's own peeler: a rename, a non-admin self-update, an
//!   Add, an admin change, a SelfRemove proposal and the Commit MDK makes of
//!   it, a Remove, the Remove of an admin (with MDK's admin-policy coupling)
//!   and a routing rotation.
//! - `openmls_forgeries`: Commits and proposals built directly with MDK's
//!   pinned OpenMLS (which does not judge Marmot semantics) in an adopted
//!   group: what MDK refuses to send, for libmarmot's negatives, and a
//!   by-reference AppDataUpdate.

use std::collections::BTreeSet;
use std::sync::Arc;

use base64::Engine as _;
use base64::engine::general_purpose::STANDARD as B64;
use cgka_engine::account_identity_proof::{
    AccountIdentityProofRequest, AccountIdentityProofSigner, account_identity_proof_component,
};
use cgka_engine::feature_registry::FeatureRegistry;
use cgka_engine::key_package::key_package_metadata;
use cgka_engine::{Engine, EngineBuilder, ManualConvergenceClock};
use cgka_traits::TransportEndpoint;
use cgka_traits::app_components::{
    AppComponentData, GROUP_ADMIN_POLICY_COMPONENT_ID, GROUP_LIFECYCLE_COMPONENT_ID,
    GROUP_PROFILE_COMPONENT_ID, GroupProfileV1, NOSTR_ROUTING_COMPONENT_ID, NostrRoutingV1,
    encode_components_list, encode_group_profile_v1, encode_nostr_routing_v1,
};
use cgka_traits::capabilities::{Capability, CapabilityRequirement, Feature, RequirementLevel};
use cgka_traits::engine::{CgkaEngine, CreateGroupRequest, KeyPackage, SendIntent, SendResult};
use cgka_traits::group::ProtocolProfile;
use cgka_traits::storage::{AccountDeviceSignerStorage, KeyPackageBundleStorage, StorageProvider};
use cgka_traits::transport::TransportMessage;
use cgka_traits::types::{GroupId, MemberId, MessageId};
use k256::schnorr::SigningKey;
use k256::schnorr::signature::hazmat::PrehashSigner;
use nostr::nips::nip59::extract_rumor_async;
use nostr::prelude::*;
use openmls_basic_credential::SignatureKeyPair;
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use storage_sqlite::SqliteAccountStorage;
use tls_codec::Serialize as _;
use transport_nostr_adapter::NostrKeyPackagePublication;
use transport_nostr_peeler::{NostrMlsPeeler, NostrTransportEvent, SdkSigner};

const RELAY_A: &str = "wss://relay-a.example.com";
const RELAY_B: &str = "wss://relay-b.example.com";
const RELAY_C: &str = "wss://relay-c.example.com";

/// Test-only account key, derived exactly like MDK's own test support
/// (crates/cgka-engine/tests/support/mod.rs `signing_key`).
fn account_signing_key(seed: &[u8]) -> SigningKey {
    let mut counter = 0u64;
    loop {
        let mut material = [0u8; 32];
        let mut hasher = Sha256::new();
        hasher.update(b"cgka-engine-test-identity-v1");
        hasher.update(seed);
        hasher.update(counter.to_be_bytes());
        material.copy_from_slice(&hasher.finalize());
        if let Ok(sk) = SigningKey::from_bytes(&material) {
            return sk;
        }
        counter += 1;
    }
}

fn identity(seed: &[u8]) -> [u8; 32] {
    account_signing_key(seed).verifying_key().to_bytes().into()
}

struct ProofSigner(SigningKey);

impl AccountIdentityProofSigner for ProofSigner {
    fn sign_account_identity_proof(
        &self,
        request: &AccountIdentityProofRequest,
    ) -> Result<[u8; 64], String> {
        if self.0.verifying_key().to_bytes().as_slice() != request.account_identity.as_slice() {
            return Err("request account identity does not match key".into());
        }
        let signature = self
            .0
            .sign_prehash(&request.proof_event_id()?)
            .map_err(|e| e.to_string())?;
        Ok(signature.to_bytes())
    }
}

// ── MDK engine parties ─────────────────────────────────────────────────────

struct Party {
    name: &'static str,
    engine: Engine<SqliteAccountStorage>,
    storage: SqliteAccountStorage,
    clock: ManualConvergenceClock,
    keys: Keys,
    pk: [u8; 32],
}

/// The engine default (FeatureRegistry::new()) plus SelfRemove as an
/// Optional feature: leaves advertise proposal 0x000a, groups do not
/// require it.
fn registry() -> FeatureRegistry {
    let mut r = FeatureRegistry::new();
    r.register(
        Feature("self-remove"),
        CapabilityRequirement {
            requires: Capability::Proposal(10),
            level: RequirementLevel::Optional,
            description: "SelfRemove advertised, not required",
        },
    );
    r
}

/// cgka-engine defaults (profile, admin policy, lifecycle) plus Nostr routing.
fn components() -> Vec<u16> {
    vec![
        GROUP_PROFILE_COMPONENT_ID,
        GROUP_ADMIN_POLICY_COMPONENT_ID,
        NOSTR_ROUTING_COMPONENT_ID,
        GROUP_LIFECYCLE_COMPONENT_ID,
    ]
}

fn party(name: &'static str, seed: &[u8]) -> Party {
    let signing = account_signing_key(seed);
    let sk: [u8; 32] = signing.to_bytes().into();
    let pk: [u8; 32] = signing.verifying_key().to_bytes().into();
    let keys = Keys::new(SecretKey::from_slice(&sk).expect("secret key"));
    let storage = SqliteAccountStorage::in_memory().unwrap();
    let wall = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_millis() as u64;
    let clock = ManualConvergenceClock::new(1_000, wall);
    let engine = EngineBuilder::new(storage.clone())
        .identity(pk.to_vec())
        .account_identity_proof_signer(Arc::new(ProofSigner(signing)))
        .supported_app_components(components())
        .feature_registry(registry())
        .protocol_profile(ProtocolProfile::Current)
        .convergence_clock(Arc::new(clock.clone()))
        .peeler(Box::new(NostrMlsPeeler::new().with_welcome_signer(keys.clone())))
        .build()
        .expect("build current-profile engine");
    Party { name, engine, storage, clock, keys, pk }
}

fn find_bytes(value: &Value, key: &str) -> Option<Vec<u8>> {
    match value {
        Value::Object(map) => {
            if let Some(v) = map.get(key)
                && let Some(bytes) = as_bytes(v)
            {
                return Some(bytes);
            }
            map.values().find_map(|v| find_bytes(v, key))
        }
        Value::Array(items) => items.iter().find_map(|v| find_bytes(v, key)),
        _ => None,
    }
}

fn as_bytes(value: &Value) -> Option<Vec<u8>> {
    match value {
        Value::Array(items) if !items.is_empty() && items.iter().all(|i| i.is_u64()) => {
            Some(items.iter().map(|i| i.as_u64().unwrap() as u8).collect())
        }
        Value::Object(map) => map.values().find_map(as_bytes),
        Value::String(s) => hex::decode(s).ok(),
        _ => None,
    }
}

/// The observer's private material, in the layout libmarmot stores under the
/// "kp_priv"/"kp_full" labels (init, encryption, Ed25519 seed + public key).
fn party_private(p: &Party) -> Value {
    let bundles = p.storage.stored_key_package_bundles().expect("stored bundles");
    assert_eq!(bundles.len(), 1, "exactly one fresh KeyPackage bundle");
    let bundle: Value = serde_json::from_slice(&bundles[0].value).expect("bundle json");
    let init = find_bytes(&bundle, "private_init_key").expect("private_init_key");
    let enc = find_bytes(&bundle, "private_encryption_key").expect("private_encryption_key");
    let binding = p
        .storage
        .account_device_signer(&MemberId::new(p.pk.to_vec()))
        .expect("signer binding")
        .expect("signer binding present");
    let signer = SignatureKeyPair::read(
        p.storage.mls_storage(),
        &binding.mls_signature_public_key,
        openmls_traits::types::SignatureScheme::ED25519,
    )
    .expect("signer stored");
    let signer_json = serde_json::to_value(&signer).expect("signer json");
    let private = find_bytes(&signer_json, "private").expect("signer private");
    json!({
        "init_key_private": hex::encode(init),
        "encryption_key_private": hex::encode(enc),
        "signature_key_private_seed": hex::encode(&private[..32]),
        "signature_key_public": hex::encode(&binding.mls_signature_public_key),
    })
}

fn sign_transport_event(event: &NostrTransportEvent, keys: &Keys) -> Event {
    let tags = event
        .tags
        .iter()
        .map(|t| Tag::parse(t.clone()).expect("tag"))
        .collect::<Vec<_>>();
    EventBuilder::new(Kind::Custom(event.kind as u16), event.content.clone())
        .tags(tags)
        .custom_created_at(Timestamp::from_secs(event.created_at))
        .finalize(keys)
        .expect("sign event")
}

/// A kind:30443 event exactly as marmot-app publishes it.
fn key_package_event(p: &Party, kp: &KeyPackage, created_at: u64) -> Event {
    let metadata = key_package_metadata(kp).expect("kp metadata");
    let slot = {
        let mut h = Sha256::new();
        h.update(b"w24h-kp-slot");
        h.update(p.pk);
        hex::encode(h.finalize())
    };
    let publication = NostrKeyPackagePublication {
        client_name: None,
        account_id: MemberId::new(p.pk.to_vec()),
        key_package: kp.clone(),
        key_package_slot_id: slot,
        key_package_ref: metadata.key_package_ref_hex.clone(),
        mls_ciphersuite: format!("0x{:04x}", metadata.ciphersuite),
        mls_extensions: metadata.mls_extensions.iter().map(|id| format!("0x{id:04x}")).collect(),
        mls_proposals: metadata.mls_proposals.iter().map(|id| format!("0x{id:04x}")).collect(),
        app_components: metadata
            .app_components
            .iter()
            .filter(|id| **id >= cgka_traits::app_components::PRIVATE_USE_APP_COMPONENT_ID_START)
            .map(|id| format!("0x{id:04x}"))
            .collect(),
        publish_endpoints: vec![TransportEndpoint(RELAY_A.to_owned())],
    };
    let unsigned = publication.to_event_at(created_at).expect("kp event");
    sign_transport_event(&unsigned, &p.keys)
}

async fn fresh_kp(p: &mut Party) -> (KeyPackage, Event) {
    let kp = p.engine.fresh_key_package().await.expect("fresh kp");
    let event = key_package_event(p, &kp, 1_790_000_000);
    let id = MessageId::new(event.id.to_bytes().to_vec());
    let kp = KeyPackage::with_source_event_id(kp.bytes().to_vec(), id)
        .with_protocol_profile(ProtocolProfile::Current);
    (kp, event)
}

fn routing(nostr_group_id: [u8; 32], relays: &[&str]) -> AppComponentData {
    let routing = NostrRoutingV1::new(nostr_group_id, relays.iter().map(|r| r.to_string()).collect())
        .expect("routing");
    AppComponentData {
        component_id: NOSTR_ROUTING_COMPONENT_ID,
        data: encode_nostr_routing_v1(&routing).expect("encode routing"),
    }
}

/// marmot.group.admin-policy.v1: admins<V> of sorted 32-byte keys.
fn admin_policy(mut admins: Vec<[u8; 32]>) -> Vec<u8> {
    admins.sort();
    let n = admins.len() * 32;
    let mut out = if n < 64 { vec![n as u8] } else { vec![0x40 | (n >> 8) as u8, n as u8] };
    assert!(n < 16384, "two-byte varint");
    for a in admins {
        out.extend_from_slice(&a);
    }
    out
}

fn event_json(msg: &TransportMessage) -> String {
    NostrTransportEvent::from_transport_message(msg)
        .expect("transport event")
        .to_verified_nostr_event()
        .expect("verified event")
        .as_json()
}

/// Ingest, then let convergence (and any auto-commit, e.g. of a SelfRemove)
/// run to quiescence on the manual clock.  The Commits this party publishes
/// on its own are returned, confirmed.
async fn deliver(p: &mut Party, msg: &TransportMessage, gid: &GroupId) -> Vec<TransportMessage> {
    let outcome = p.engine.ingest(msg.clone()).await.expect("ingest");
    eprintln!("  {} ingest: {outcome:?}", p.name);
    let _ = p.engine.drain_events();
    settle(p, gid).await
}

async fn settle(p: &mut Party, gid: &GroupId) -> Vec<TransportMessage> {
    let mut out = Vec::new();
    for _ in 0..32 {
        let pending = p.engine.drain_pending_convergence_groups();
        let cutoff = p.engine.prepare_convergence_cutoff_delay_ms(gid).expect("cutoff");
        p.clock.advance_ms(cutoff.unwrap_or(0) + 60_000);
        let results = p.engine.advance_convergence(gid).await.expect("advance convergence");
        let mut progressed = false;
        for r in results {
            match r {
                SendResult::GroupEvolution { msg, pending, .. } => {
                    p.engine.confirm_published(pending).await.expect("confirm");
                    out.push(msg);
                    progressed = true;
                }
                SendResult::NoChange { .. } => {}
                other => eprintln!("  {} convergence result: {other:?}", p.name),
            }
        }
        for a in p.engine.drain_auto_publish() {
            p.engine.confirm_published(a.pending).await.expect("confirm auto");
            out.push(a.msg);
            progressed = true;
        }
        let _ = p.engine.drain_events();
        if !progressed && !pending.contains(gid) && cutoff.is_none() {
            break;
        }
    }
    out
}

async fn evolve(p: &mut Party, intent: SendIntent) -> (TransportMessage, Vec<TransportMessage>) {
    match p.engine.send(intent).await.expect("send") {
        SendResult::GroupEvolution { msg, welcomes, pending } => {
            p.engine.confirm_published(pending).await.expect("confirm");
            let _ = p.engine.drain_events();
            (msg, welcomes)
        }
        other => panic!("{}: expected GroupEvolution, got {other:?}", p.name),
    }
}

async fn unwrap_welcome(welcome: &TransportMessage, recipient: &Keys) -> Option<(String, Vec<u8>)> {
    let event = NostrTransportEvent::from_transport_message(welcome).ok()?;
    let gift_wrap = event.to_verified_nostr_event().ok()?;
    let unwrapped = extract_rumor_async(&SdkSigner(Arc::new(recipient.clone())), &gift_wrap)
        .await
        .ok()?;
    let bytes = B64.decode(unwrapped.rumor.content.as_bytes()).ok()?;
    Some((unwrapped.rumor.as_json(), bytes))
}

fn step(name: &str, kind: &str, by: &Party, msg: &TransportMessage, epoch_after: u64) -> Value {
    json!({
        "name": name,
        "kind": kind,
        "by": by.name,
        "by_account": hex::encode(by.pk),
        "event_json": event_json(msg),
        "epoch_after": epoch_after,
    })
}

/// An application message (an unsigned kind:9 inner event) from `p`.
async fn app_message(p: &mut Party, gid: &GroupId, text: &str) -> Option<TransportMessage> {
    let mut inner = UnsignedEvent::new(
        p.keys.public_key(),
        Timestamp::from_secs(1_790_000_100),
        Kind::Custom(9),
        Vec::<Tag>::new(),
        text,
    );
    inner.ensure_id();
    match p
        .engine
        .send(SendIntent::AppMessage {
            group_id: gid.clone(),
            payload: inner.as_json().into_bytes(),
            expected_epoch: None,
        })
        .await
    {
        Ok(SendResult::ApplicationMessage { msg, .. }) => Some(msg),
        other => {
            eprintln!("  app message not captured: {other:?}");
            None
        }
    }
}

async fn mdk_engine() -> Value {
    let mut alice = party("alice", b"w24h-mdk-alice");
    let mut bob = party("bob", b"w24h-mdk-bob");
    let mut carol = party("carol", b"w24h-mdk-carol");
    let mut observer = party("observer", b"w24h-mdk-observer");
    let mut dave = party("dave", b"w24h-mdk-dave");

    let (kp_bob, _) = fresh_kp(&mut bob).await;
    let (kp_carol, _) = fresh_kp(&mut carol).await;
    let (kp_obs, ev_obs) = fresh_kp(&mut observer).await;
    let obs_private = party_private(&observer);
    let obs_meta = key_package_metadata(&kp_obs).expect("meta");

    let mut nostr_group_id = [0u8; 32];
    nostr_group_id.copy_from_slice(&Sha256::digest(b"w24h-nostr-group-id"));
    let (gid, result) = alice
        .engine
        .create_group(CreateGroupRequest {
            name: "W24-H commits".into(),
            description: "MDK v0.11.0 Commit fixture".into(),
            members: vec![kp_bob, kp_carol, kp_obs.clone()],
            required_features: vec![],
            app_components: vec![routing(nostr_group_id, &[RELAY_B, RELAY_A])],
            initial_admins: vec![],
        })
        .await
        .expect("create group");
    let welcomes = match result {
        SendResult::FoundingGroupCreated { welcomes } => welcomes,
        other => panic!("expected FoundingGroupCreated, got {other:?}"),
    };
    let mut observer_rumor = None;
    for w in welcomes {
        if let Some((rumor, _)) = unwrap_welcome(&w, &observer.keys).await {
            observer_rumor = Some(rumor);
        } else if unwrap_welcome(&w, &bob.keys).await.is_some() {
            bob.engine.join_welcome(w).await.expect("bob joins");
        } else {
            carol.engine.join_welcome(w).await.expect("carol joins");
        }
    }
    let observer_rumor = observer_rumor.expect("observer welcome");
    let mut steps = Vec::new();

    // 0. A message in the founding epoch, before any Commit.
    if let Some(app) = app_message(&mut alice, &gid, "before any commit").await {
        steps.push(step("message_founding_epoch", "application", &alice, &app, alice.engine.epoch(&gid).unwrap().0));
    }

    // 1. Rename (admin, inline AppDataUpdate 0x8001).
    let (msg, _) = evolve(&mut alice, SendIntent::UpdateGroupData {
        group_id: gid.clone(),
        name: Some("W24-H renamed".into()),
        description: Some("renamed by MDK".into()),
    })
    .await;
    for p in [&mut bob, &mut carol] {
        assert!(deliver(p, &msg, &gid).await.is_empty());
    }
    steps.push(step("rename", "commit", &alice, &msg, alice.engine.epoch(&gid).unwrap().0));
    if let Some(app) = app_message(&mut alice, &gid, "after rename").await {
        steps.push(step("message_after_rename", "application", &alice, &app, alice.engine.epoch(&gid).unwrap().0));
    }

    // 2. A non-admin's self-update.
    let (msg, _) = evolve(&mut bob, SendIntent::SelfUpdate { group_id: gid.clone() }).await;
    for p in [&mut alice, &mut carol] {
        assert!(deliver(p, &msg, &gid).await.is_empty());
    }
    steps.push(step("self_update", "commit", &bob, &msg, bob.engine.epoch(&gid).unwrap().0));
    if let Some(app) = app_message(&mut alice, &gid, "after self_update").await {
        steps.push(step("message_after_self_update", "application", &alice, &app, alice.engine.epoch(&gid).unwrap().0));
    }

    // 3. Add (admin).
    let (kp_dave, _) = fresh_kp(&mut dave).await;
    let (msg, welcomes) = evolve(&mut alice, SendIntent::Invite {
        group_id: gid.clone(),
        key_packages: vec![kp_dave],
        initial_admins: vec![],
    })
    .await;
    for p in [&mut bob, &mut carol] {
        assert!(deliver(p, &msg, &gid).await.is_empty());
    }
    dave.engine.join_welcome(welcomes[0].clone()).await.expect("dave joins");
    steps.push(step("add", "commit", &alice, &msg, alice.engine.epoch(&gid).unwrap().0));
    if let Some(app) = app_message(&mut alice, &gid, "after add").await {
        steps.push(step("message_after_add", "application", &alice, &app, alice.engine.epoch(&gid).unwrap().0));
    }

    // 4. Admin change: Bob becomes a co-admin (0x8003).
    let co_admins = admin_policy(vec![alice.pk, bob.pk]);
    let (msg, _) = evolve(&mut alice, SendIntent::UpdateAppComponents {
        group_id: gid.clone(),
        updates: vec![AppComponentData {
            component_id: GROUP_ADMIN_POLICY_COMPONENT_ID,
            data: co_admins,
        }],
    })
    .await;
    for p in [&mut bob, &mut carol, &mut dave] {
        assert!(deliver(p, &msg, &gid).await.is_empty());
    }
    steps.push(step("admin_change", "commit", &alice, &msg, alice.engine.epoch(&gid).unwrap().0));
    if let Some(app) = app_message(&mut alice, &gid, "after admin_change").await {
        steps.push(step("message_after_admin_change", "application", &alice, &app, alice.engine.epoch(&gid).unwrap().0));
    }

    // 5. Dave (no admin) leaves: his SelfRemove, committed by Alice's
    //    auto-committer (the first to settle).
    let proposal = match dave.engine.send(SendIntent::Leave { group_id: gid.clone() }).await.expect("leave") {
        SendResult::Proposal { msg } => msg,
        other => panic!("expected a proposal, got {other:?}"),
    };
    steps.push(step("self_remove_proposal", "proposal", &dave, &proposal, dave.engine.epoch(&gid).unwrap().0));
    let commits = deliver(&mut alice, &proposal, &gid).await;
    assert_eq!(commits.len(), 1, "alice auto-commits the SelfRemove");
    let msg = commits[0].clone();
    for p in [&mut bob, &mut carol] {
        // Both before any convergence pass: their own auto-committers must
        // not race Alice's Commit.
        let outcome = p.engine.ingest(proposal.clone()).await.expect("ingest proposal");
        eprintln!("  {} ingest: {outcome:?}", p.name);
        assert!(deliver(p, &msg, &gid).await.is_empty());
    }
    steps.push(step("self_remove_commit", "commit", &alice, &msg, alice.engine.epoch(&gid).unwrap().0));
    if let Some(app) = app_message(&mut alice, &gid, "after self_remove_commit").await {
        steps.push(step("message_after_self_remove_commit", "application", &alice, &app, alice.engine.epoch(&gid).unwrap().0));
    }

    // 6. Remove (admin) of Carol, not an admin.
    let (msg, _) = evolve(&mut alice, SendIntent::RemoveMembers {
        group_id: gid.clone(),
        members: vec![MemberId::new(carol.pk.to_vec())],
    })
    .await;
    assert!(deliver(&mut bob, &msg, &gid).await.is_empty());
    steps.push(step("remove", "commit", &alice, &msg, alice.engine.epoch(&gid).unwrap().0));
    if let Some(app) = app_message(&mut alice, &gid, "after remove").await {
        steps.push(step("message_after_remove", "application", &alice, &app, alice.engine.epoch(&gid).unwrap().0));
    }

    // 7. Remove of an admin (Bob): MDK couples the admin-policy update.
    let (msg, _) = evolve(&mut alice, SendIntent::RemoveMembers {
        group_id: gid.clone(),
        members: vec![MemberId::new(bob.pk.to_vec())],
    })
    .await;
    steps.push(step("remove_admin", "commit", &alice, &msg, alice.engine.epoch(&gid).unwrap().0));
    if let Some(app) = app_message(&mut alice, &gid, "after remove_admin").await {
        steps.push(step("message_after_remove_admin", "application", &alice, &app, alice.engine.epoch(&gid).unwrap().0));
    }

    // 8. Routing rotation: a fresh nostr_group_id and another relay set,
    //    published at the old address.
    let mut rotated = [0u8; 32];
    rotated.copy_from_slice(&Sha256::digest(b"w24h-rotated-nostr-group-id"));
    let (msg, _) = evolve(&mut alice, SendIntent::UpdateAppComponents {
        group_id: gid.clone(),
        updates: vec![routing(rotated, &[RELAY_C, RELAY_A])],
    })
    .await;
    steps.push(step("routing_rotation", "commit", &alice, &msg, alice.engine.epoch(&gid).unwrap().0));

    // 9. A message at the new address, after the rotation.
    if let Some(app) = app_message(&mut alice, &gid, "after the rotation").await {
        steps.push(step("message_after_rotation", "application", &alice, &app, alice.engine.epoch(&gid).unwrap().0));
    }

    json!({
        "accounts": {
            "alice": hex::encode(alice.pk), "bob": hex::encode(bob.pk), "carol": hex::encode(carol.pk),
            "dave": hex::encode(dave.pk), "observer": hex::encode(observer.pk),
        },
        "observer_key_package_mls_message": hex::encode(kp_obs.bytes()),
        "observer_key_package_ref": obs_meta.key_package_ref_hex,
        "observer_key_package_event_json": ev_obs.as_json(),
        "observer_private": obs_private,
        "welcome_rumor_json": observer_rumor,
        "group_id": hex::encode(gid.as_slice()),
        "nostr_group_id": hex::encode(nostr_group_id),
        "rotated_nostr_group_id": hex::encode(rotated),
        "relays": [RELAY_A, RELAY_B],
        "rotated_relays": [RELAY_A, RELAY_C],
        "steps": steps,
    })
}

// ── OpenMLS-direct forgeries (same pinned OpenMLS as MDK v0.11.0) ───────────

mod forgeries {
    use super::*;
    use openmls::component::ComponentData;
    use openmls::extensions::{AppDataDictionary, AppDataDictionaryExtension};
    use openmls::group::GroupContext;
    use openmls::messages::proposals::{AppDataUpdateOperation, AppDataUpdateProposal, Proposal};
    use openmls::prelude::{
        BasicCredential, Capabilities, CredentialWithKey, Extension, ExtensionType, Extensions,
        KeyPackage as MlsKeyPackage, LeafNode, LeafNodeIndex, MlsGroup, MlsGroupCreateConfig,
        MlsGroupJoinConfig, MlsMessageBodyIn, MlsMessageIn, ProcessedMessageContent, ProposalType,
        RequiredCapabilitiesExtension, StagedWelcome,
    };
    use openmls_rust_crypto::OpenMlsRustCrypto;
    use openmls_traits::OpenMlsProvider;
    use openmls_traits::types::Ciphersuite;
    use tls_codec::Deserialize as _;

    const CS: Ciphersuite = Ciphersuite::MLS_128_DHKEMX25519_AES128GCM_SHA256_Ed25519;
    /// libmarmot's adopted leaf advertisement since W24 slice I
    /// (MLS_ADOPTED_SUPPORTED_COMPONENTS + 0x0001).
    const LEAF_COMPONENTS: [u16; 8] = [0x0001, 0x8001, 0x8003, 0x8004, 0x8006, 0x8009, 0x800b, 0x800c];
    const REQUIRED_COMPONENTS: [u16; 5] = [0x8001, 0x8003, 0x8004, 0x8009, 0x800c];
    /// A group that has not enabled lifecycle-v1 (0x800c neither required nor present).
    const REQUIRED_NO_LIFECYCLE: [u16; 4] = [0x8001, 0x8003, 0x8004, 0x8009];
    /// The agent text stream receive role's capability (as libmarmot's leaves).
    const RECEIVE_ROLE: u16 = 0xF2D1;

    fn proof(seed: &[u8], sig_pub: &[u8], tamper: bool) -> Vec<u8> {
        let mut p = account_identity_proof_component(
            &identity(seed),
            sig_pub,
            CS,
            CS.signature_algorithm(),
            1_790_000_000,
            &ProofSigner(account_signing_key(seed)),
        )
        .expect("proof");
        if tamper {
            p[40] ^= 1; // first signature byte: a proof that does not verify
        }
        p
    }

    fn leaf_extensions(proof: Option<Vec<u8>>) -> Extensions<LeafNode> {
        let mut dict = AppDataDictionary::new();
        dict.insert(0x0001, encode_components_list(&LEAF_COMPONENTS.iter().copied().collect()));
        dict.insert(0x0002, encode_components_list(&BTreeSet::new()));
        if let Some(p) = proof {
            dict.insert(0x8009, p);
        }
        Extensions::single(Extension::AppDataDictionary(AppDataDictionaryExtension::new(dict)))
            .expect("leaf extensions")
    }

    fn capabilities() -> Capabilities {
        Capabilities::new(
            None,
            Some(&[CS]),
            Some(&[ExtensionType::AppDataDictionary, ExtensionType::Unknown(RECEIVE_ROLE)]),
            Some(&[ProposalType::AppDataUpdate]),
            None,
        )
    }

    pub struct Member {
        pub seed: &'static [u8],
        pub provider: OpenMlsRustCrypto,
        pub signer: SignatureKeyPair,
        pub group: Option<MlsGroup>,
    }

    fn member(seed: &'static [u8]) -> Member {
        Member {
            seed,
            provider: OpenMlsRustCrypto::default(),
            signer: SignatureKeyPair::new(CS.signature_algorithm()).unwrap(),
            group: None,
        }
    }

    fn cwk(m: &Member) -> CredentialWithKey {
        CredentialWithKey {
            credential: BasicCredential::new(identity(m.seed).to_vec()).into(),
            signature_key: m.signer.public().into(),
        }
    }

    /// A KeyPackage of `m` (stored in its provider), with a proof (valid,
    /// tampered) or none.
    fn key_package(m: &Member, proof_kind: u8) -> (MlsKeyPackage, Value) {
        let p = match proof_kind {
            0 => Some(proof(m.seed, &m.signer.to_public_vec(), false)),
            1 => Some(proof(m.seed, &m.signer.to_public_vec(), true)),
            _ => None,
        };
        let bundle = MlsKeyPackage::builder()
            .leaf_node_capabilities(capabilities())
            .leaf_node_extensions(leaf_extensions(p))
            .build(CS, &m.provider, &m.signer, cwk(m))
            .expect("kp");
        let bundle_json = serde_json::to_value(&bundle).expect("bundle json");
        let signer_json = serde_json::to_value(&m.signer).expect("signer json");
        let private = json!({
            "init_key_private": hex::encode(find_bytes(&bundle_json, "private_init_key").unwrap()),
            "encryption_key_private": hex::encode(find_bytes(&bundle_json, "private_encryption_key").unwrap()),
            "signature_key_private_seed": hex::encode(&find_bytes(&signer_json, "private").unwrap()[..32]),
            "signature_key_public": hex::encode(m.signer.to_public_vec()),
        });
        (bundle.key_package().clone(), private)
    }

    fn group_context(admins: &[[u8; 32]]) -> Extensions<GroupContext> {
        group_context_with(admins, &REQUIRED_COMPONENTS, [0x43; 32])
    }

    fn group_context_with(admins: &[[u8; 32]], required_components: &[u16], nostr_gid: [u8; 32]) -> Extensions<GroupContext> {
        let required = RequiredCapabilitiesExtension::new(
            &[ExtensionType::AppDataDictionary],
            &[ProposalType::AppDataUpdate],
            &[],
        );
        let mut dict = AppDataDictionary::new();
        dict.insert(0x0001, encode_components_list(&required_components.iter().copied().collect()));
        dict.insert(
            0x8001,
            encode_group_profile_v1(&GroupProfileV1 {
                name: "W24-H forgeries".into(),
                description: String::new(),
            })
            .unwrap(),
        );
        dict.insert(0x8003, admin_policy(admins.to_vec()));
        dict.insert(0x8004, routing(nostr_gid, &[RELAY_A]).data);
        if required_components.contains(&0x800c) {
            dict.insert(0x800c, vec![0]);
        }
        Extensions::from_vec(vec![
            Extension::RequiredCapabilities(required),
            Extension::AppDataDictionary(AppDataDictionaryExtension::new(dict)),
        ])
        .expect("group context")
    }

    fn mls_message_in(bytes: &[u8]) -> MlsMessageIn {
        MlsMessageIn::tls_deserialize_exact(bytes).expect("MLSMessage")
    }

    fn join(m: &mut Member, welcome: &[u8]) {
        let MlsMessageBodyIn::Welcome(w) = mls_message_in(welcome).extract() else {
            panic!("not a Welcome");
        };
        let config = MlsGroupJoinConfig::builder()
            .wire_format_policy(openmls::prelude::PURE_PLAINTEXT_WIRE_FORMAT_POLICY)
            .use_ratchet_tree_extension(true)
            .build();
        let staged = StagedWelcome::new_from_welcome(&m.provider, &config, w, None).expect("staged");
        m.group = Some(staged.into_group(&m.provider).expect("joined"));
    }

    /// `m` processes and merges a Commit (resolving AppDataUpdates as MDK does).
    fn apply(m: &mut Member, commit: &[u8]) {
        let group = m.group.as_mut().unwrap();
        let proto = mls_message_in(commit).try_into_protocol_message().expect("protocol");
        let processed = group.process_message(&m.provider, proto).expect("process");
        let processed = if let ProcessedMessageContent::UnresolvedAppDataCommit(u) = processed.content() {
            let updates = u.app_data_update_proposals().cloned().collect::<Vec<_>>();
            let mut updater = group.app_data_dictionary_updater();
            for update in updates {
                match update.operation() {
                    AppDataUpdateOperation::Update(d) => {
                        updater.set(ComponentData::from_parts(update.component_id(), d.clone()))
                    }
                    AppDataUpdateOperation::Remove => {
                        updater.remove(&update.component_id());
                    }
                }
            }
            let changes = updater.changes();
            group.resolve_app_data_commit(&m.provider, processed, changes).expect("resolve")
        } else {
            processed
        };
        match processed.into_content() {
            ProcessedMessageContent::StagedCommitMessage(sc) => {
                group.merge_staged_commit(&m.provider, *sc).expect("merge")
            }
            _ => panic!("not a Commit"),
        }
    }

    /// A Commit of `m` from its current epoch (removes, adds, AppDataUpdate
    /// operations, an optional GroupContextExtensions list, or only a path),
    /// left unmerged: its bytes.
    fn commit(
        m: &mut Member,
        removes: Vec<u32>,
        adds: Vec<MlsKeyPackage>,
        ops: Vec<(u16, Option<Vec<u8>>)>,
        gce: Option<Extensions<GroupContext>>,
        consume_store: bool,
    ) -> Vec<u8> {
        let snapshot = m.provider.storage().values.read().unwrap().clone();
        let group = m.group.as_mut().unwrap();
        let own_ops = ops.clone();
        let proposals = ops
            .into_iter()
            .map(|(id, data)| {
                Proposal::AppDataUpdate(Box::new(match data {
                    Some(d) => AppDataUpdateProposal::update(id, d),
                    None => AppDataUpdateProposal::remove(id),
                }))
            })
            .collect::<Vec<_>>();
        let mut builder = group
            .commit_builder()
            .consume_proposal_store(consume_store)
            .force_self_update(true)
            .propose_removals(removes.into_iter().map(LeafNodeIndex::new))
            .propose_adds(adds)
            .add_proposals(proposals);
        if let Some(gce) = gce {
            builder = builder.propose_group_context_extensions(gce).expect("gce");
        }
        let mut builder = builder.load_psks(m.provider.storage()).expect("psks");
        let mut app_data = builder.app_data_dictionary_updater();
        // The dictionary the Commit's own operations make (the builder's
        // list also names proposals in the store, which only a Commit that
        // consumes the store carries).
        let ops: Vec<(u16, Option<Vec<u8>>)> = if consume_store {
            builder
                .app_data_update_proposals()
                .map(|p| {
                    (p.component_id(), match p.operation() {
                        AppDataUpdateOperation::Update(d) => Some(d.as_slice().to_vec()),
                        AppDataUpdateOperation::Remove => None,
                    })
                })
                .collect()
        } else {
            own_ops
        };
        for (id, data) in ops {
            match data {
                Some(d) => app_data.set(ComponentData::from_parts(id, d.into())),
                None => {
                    app_data.remove(&id);
                }
            }
        }
        let changes = app_data.changes();
        builder.with_app_data_dictionary_updates(changes);
        let bundle = builder
            .build(m.provider.rand(), m.provider.crypto(), &m.signer, |_| true)
            .expect("build")
            .stage_commit(&m.provider)
            .expect("stage");
        let (msg, _w, _gi) = bundle.into_messages();
        let bytes = msg.tls_serialize_detached().expect("commit bytes");
        group.clear_pending_commit(m.provider.storage()).expect("clear");
        // OpenMLS keeps state of a staged-then-cleared Commit (a later
        // Commit's UpdatePath then fails to decrypt): every forgery starts
        // from the same stored epoch, restored whole.
        *m.provider.storage().values.write().unwrap() = snapshot;
        reload(m);
        bytes
    }

    /// The group as its (restored) storage holds it.
    fn reload(m: &mut Member) {
        let gid = m.group.as_ref().unwrap().group_id().clone();
        m.group = Some(
            MlsGroup::load(m.provider.storage(), &gid).expect("load").expect("stored group"),
        );
    }

    /// `m` keeps another member's standalone proposal for its next Commit.
    fn receive_proposal(m: &mut Member, proposal: &[u8]) {
        let group = m.group.as_mut().unwrap();
        let proto = mls_message_in(proposal).try_into_protocol_message().expect("protocol");
        let processed = group.process_message(&m.provider, proto).expect("process proposal");
        match processed.into_content() {
            ProcessedMessageContent::ProposalMessage(qp) => {
                group.store_pending_proposal(m.provider.storage(), *qp).expect("store proposal")
            }
            _ => panic!("not a proposal"),
        }
    }

    fn propose(m: &mut Member, id: u16, data: Vec<u8>) -> Vec<u8> {
        let group = m.group.as_mut().unwrap();
        let (msg, _ref) = group
            .propose_app_data_update(&m.provider, &m.signer, id, AppDataUpdateOperation::Update(data.into()))
            .expect("propose");
        msg.tls_serialize_detached().expect("proposal bytes")
    }

    fn profile(name: &str) -> Vec<u8> {
        encode_group_profile_v1(&GroupProfileV1 { name: name.into(), description: String::new() })
            .unwrap()
    }

    fn components(ids: &[u16]) -> Vec<u8> {
        encode_components_list(&ids.iter().copied().collect())
    }

    /// Valid component states, encoded by MDK v0.11.0's own encoders.
    fn blossom_image() -> Vec<u8> {
        use cgka_traits::app_components::{GroupBlossomImageV1, encode_group_blossom_image_v1};
        encode_group_blossom_image_v1(&GroupBlossomImageV1 {
            image_hash: vec![0x11; 32],
            image_key: vec![0x22; 32],
            image_nonce: vec![0x33; 12],
            image_upload_key: vec![0x44; 32],
            media_type: "image/png".into(),
        })
        .expect("0x8002")
    }

    fn avatar_url(url: &str) -> Vec<u8> {
        use cgka_traits::app_components::{GroupAvatarUrlV1, encode_group_avatar_url_v1};
        encode_group_avatar_url_v1(&GroupAvatarUrlV1 { url: url.into(), dim: vec![], thumbhash: vec![] })
            .expect("0x8007")
    }

    fn media_v2(endpoint: &str) -> Vec<u8> {
        use cgka_traits::app_components::{EncryptedMediaPolicyV2, encode_encrypted_media_policy_v2};
        encode_encrypted_media_policy_v2(
            &EncryptedMediaPolicyV2::blossom_default([endpoint.to_owned()]).expect("policy"),
        )
        .expect("0x800b")
    }

    fn agent_stream_receive() -> Vec<u8> {
        cgka_traits::agent_text_stream::AgentTextStreamQuicPolicyV1::user_to_agent_default()
            .encode_component_state()
            .expect("0x8006")
    }

    /// A second group (creator XL, co-admin WL and the observer) that has not
    /// enabled lifecycle-v1, for MDK's enablement rules (slice H review M1,
    /// re-review R3).
    fn lifecycle_less(o: &Member) -> Value {
        let mut xl = member(b"w24h-lc-x");
        let mut wl = member(b"w24h-lc-w");
        let (kp_o, o_private) = key_package(o, 0);
        let (kp_w, _) = key_package(&wl, 0);
        let config = MlsGroupCreateConfig::builder()
            .ciphersuite(CS)
            .capabilities(capabilities())
            .with_leaf_node_extensions(leaf_extensions(Some(proof(xl.seed, &xl.signer.to_public_vec(), false))))
            .expect("leaf extensions")
            .with_group_context_extensions(group_context_with(&[identity(xl.seed), identity(wl.seed)], &REQUIRED_NO_LIFECYCLE, [0x44; 32]))
            .wire_format_policy(openmls::prelude::PURE_PLAINTEXT_WIRE_FORMAT_POLICY)
            .use_ratchet_tree_extension(true)
            .build();
        let mut group = MlsGroup::new(&xl.provider, &xl.signer, &config, cwk(&xl)).expect("group");
        let (_c, welcome, _gi) = group
            .add_members(&xl.provider, &xl.signer, &[kp_w, kp_o.clone()])
            .expect("add WL and the observer");
        group.merge_pending_commit(&xl.provider).expect("merge");
        xl.group = Some(group);
        let welcome = welcome.tls_serialize_detached().expect("welcome");
        join(&mut wl, &welcome);
        let enabled = components(&REQUIRED_COMPONENTS);
        let mut c = serde_json::Map::new();
        let mut put = |name: &str, bytes: Vec<u8>| {
            c.insert(name.into(), json!({ "by": "xl", "message": hex::encode(bytes) }));
        };
        // MDK's enablement shape: require 0x800c and install `active`, inline,
        // nothing else (EnableDisbanding).
        put("ok_enable_lifecycle", commit(&mut xl, vec![], vec![], vec![(0x0001, Some(enabled.clone())), (0x800c, Some(vec![0]))], None, false));
        put("enable_lifecycle_with_rename", commit(&mut xl, vec![], vec![], vec![(0x0001, Some(enabled.clone())), (0x800c, Some(vec![0])), (0x8001, Some(profile("renamed while enabling")))], None, false));
        put("lifecycle_state_unrequired", commit(&mut xl, vec![], vec![], vec![(0x800c, Some(vec![0]))], None, false));
        // Re-review R3: the enablement with its 0x0001 update by reference
        // (WL's standalone proposal, an admin's), its 0x800c state inline:
        // MDK requires every enablement proposal inline.
        let pw = propose(&mut wl, 0x0001, enabled.clone());
        receive_proposal(&mut xl, &pw);
        let enable_by_ref = commit(&mut xl, vec![], vec![], vec![(0x800c, Some(vec![0]))], None, true);
        xl.group
            .as_mut()
            .unwrap()
            .clear_pending_proposals(xl.provider.storage())
            .expect("clear proposals");
        json!({
            "by_ref": {
                "proposal_wl_requirements": hex::encode(pw),
                "commit_xl_enable_by_ref": hex::encode(enable_by_ref),
            },
            "observer_key_package": hex::encode(kp_o.tls_serialize_detached().unwrap()),
            "observer_private": o_private,
            "welcome": hex::encode(&welcome),
            "creator_account": hex::encode(identity(xl.seed)),
            "commits": Value::Object(c),
        })
    }

    pub fn emit() -> Value {
        // X creates; W (co-admin), Y (no admin) and the observer O join.
        let mut x = member(b"w24h-neg-x");
        let mut w = member(b"w24h-neg-w");
        let mut y = member(b"w24h-neg-y");
        let o = member(b"w24h-neg-observer");
        let (kp_w, _) = key_package(&w, 0);
        let (kp_y, _) = key_package(&y, 0);
        let (kp_o, o_private) = key_package(&o, 0);
        let admins = [identity(x.seed), identity(w.seed)];
        let config = MlsGroupCreateConfig::builder()
            .ciphersuite(CS)
            .capabilities(capabilities())
            .with_leaf_node_extensions(leaf_extensions(Some(proof(x.seed, &x.signer.to_public_vec(), false))))
            .expect("leaf extensions")
            .with_group_context_extensions(group_context(&admins))
            .wire_format_policy(openmls::prelude::PURE_PLAINTEXT_WIRE_FORMAT_POLICY)
            .use_ratchet_tree_extension(true)
            .build();
        let mut group = MlsGroup::new(&x.provider, &x.signer, &config, cwk(&x)).expect("group");
        let (_c, welcome, _gi) = group
            .add_members(&x.provider, &x.signer, &[kp_w, kp_y, kp_o.clone()])
            .expect("add members");
        group.merge_pending_commit(&x.provider).expect("merge");
        x.group = Some(group);
        let welcome = welcome.tls_serialize_detached().expect("welcome");
        join(&mut w, &welcome);
        join(&mut y, &welcome);
        let leaf = |m: &Member| -> u32 {
            let g = x.group.as_ref().unwrap();
            g.members()
                .find(|mem| mem.credential.serialized_content() == identity(m.seed).as_slice())
                .map(|mem| mem.index.u32())
                .expect("leaf")
        };
        let w_leaf = leaf(&w);
        let y_leaf = leaf(&y);
        let o_leaf = leaf(&o);

        let only_x = admin_policy(vec![identity(x.seed)]);
        let fresh = member(b"w24h-neg-new");
        let (kp_ok, _) = key_package(&fresh, 0);
        let bad = member(b"w24h-neg-badproof");
        let (kp_bad, _) = key_package(&bad, 1);
        let none = member(b"w24h-neg-noproof");
        let (kp_none, _) = key_package(&none, 2);
        let mut c = serde_json::Map::new();
        let mut put = |name: &str, by: &str, bytes: Vec<u8>| {
            c.insert(name.into(), json!({ "by": by, "message": hex::encode(bytes) }));
        };

        // From the joined epoch (1).
        put("ok_rename", "x", commit(&mut x, vec![], vec![], vec![(0x8001, Some(profile("renamed by X")))], None, false));
        put("ok_unknown_component", "x", commit(&mut x, vec![], vec![], vec![(0x9000, Some(vec![1, 2, 3]))], None, false));
        put("ok_nonadmin_self_update", "y", commit(&mut y, vec![], vec![], vec![], None, false));
        put("ok_remove_admin_coupled", "x", commit(&mut x, vec![w_leaf], vec![], vec![(0x8003, Some(only_x.clone()))], None, false));
        put("ok_add", "x", commit(&mut x, vec![], vec![kp_ok], vec![], None, false));
        put("nonadmin_rename", "y", commit(&mut y, vec![], vec![], vec![(0x8001, Some(profile("renamed by Y")))], None, false));
        let (kp_ok2, _) = key_package(&member(b"w24h-neg-new2"), 0);
        put("nonadmin_add", "y", commit(&mut y, vec![], vec![kp_ok2], vec![], None, false));
        put("nonadmin_remove", "y", commit(&mut y, vec![w_leaf], vec![], vec![], None, false));
        put("drop_required_routing", "x", commit(&mut x, vec![], vec![], vec![(0x8004, None)], None, false));
        put("remove_admin_policy", "x", commit(&mut x, vec![], vec![], vec![(0x8003, None)], None, false));
        put("remove_lifecycle", "x", commit(&mut x, vec![], vec![], vec![(0x800c, None)], None, false));
        put("empty_admins", "x", commit(&mut x, vec![], vec![], vec![(0x8003, Some(vec![0x00]))], None, false));
        put("admin_not_member", "x", commit(&mut x, vec![], vec![], vec![(0x8003, Some(admin_policy(vec![[0x77; 32]])))], None, false));
        put("malformed_profile", "x", commit(&mut x, vec![], vec![], vec![(0x8001, Some(vec![0x02, 0xc3, 0x28, 0x00]))], None, false));
        put("malformed_routing", "x", commit(&mut x, vec![], vec![], vec![(0x8004, Some(vec![0x00; 32]))], None, false));
        put("remove_admin_uncoupled", "x", commit(&mut x, vec![w_leaf], vec![], vec![], None, false));
        put("add_bad_proof", "x", commit(&mut x, vec![], vec![kp_bad], vec![], None, false));
        put("add_without_proof", "x", commit(&mut x, vec![], vec![kp_none], vec![], None, false));
        put("disband", "x", commit(&mut x, vec![], vec![], vec![(0x800c, Some(vec![1]))], None, false));
        put("malformed_media_v2", "x", commit(&mut x, vec![], vec![], vec![(0x800b, Some(vec![0x00]))], None, false));
        put("proof_in_group_context", "x", commit(&mut x, vec![], vec![], vec![(0x8009, Some(vec![0x00; 104]))], None, false));
        let gce = x.group.as_ref().unwrap().extensions().clone();
        put("group_context_extensions", "x", commit(&mut x, vec![], vec![], vec![], Some(gce), false));

        // Slice H review M1: lifecycle (0x800c) transitions MDK refuses
        // (validate_group_lifecycle_transition), and the control.
        let no_lifecycle = components(&[0x8001, 0x8003, 0x8004, 0x8009]);
        put("lifecycle_unrequire", "x", commit(&mut x, vec![], vec![], vec![(0x0001, Some(no_lifecycle.clone()))], None, false));
        put("lifecycle_redundant", "x", commit(&mut x, vec![], vec![], vec![(0x800c, Some(vec![0]))], None, false));
        put("lifecycle_unrequire_rename", "x", commit(&mut x, vec![], vec![], vec![(0x0001, Some(no_lifecycle)), (0x8001, Some(profile("renamed while un-requiring")))], None, false));
        put("ok_unrequire_profile", "x", commit(&mut x, vec![], vec![], vec![(0x0001, Some(components(&[0x8003, 0x8004, 0x8009, 0x800c])))], None, false));
        // MC (slice I's validator): White Noise components updated.
        put("ok_media_v2", "x", commit(&mut x, vec![], vec![], vec![(0x800b, Some(media_v2("https://blossom.example.com")))], None, false));
        put("ok_agent_stream", "x", commit(&mut x, vec![], vec![], vec![(0x8006, Some(agent_stream_receive()))], None, false));
        let mut send_required = agent_stream_receive();
        send_required[0] = 0x03; // receive | send
        put("agent_stream_send_required", "x", commit(&mut x, vec![], vec![], vec![(0x8006, Some(send_required))], None, false));
        // L4: 0x8002, 0x8005 and 0x8007 on the Commit path.
        put("ok_image", "x", commit(&mut x, vec![], vec![], vec![(0x8002, Some(blossom_image()))], None, false));
        put("ok_avatar", "x", commit(&mut x, vec![], vec![], vec![(0x8007, Some(avatar_url("https://example.com/avatar.png")))], None, false));
        put("ok_retention", "x", commit(&mut x, vec![], vec![], vec![(0x8005, Some(vec![0, 0, 0, 0, 0, 0, 0x0e, 0x10]))], None, false));
        let mut bad_image = blossom_image();
        bad_image.truncate(bad_image.len() - 1);
        put("malformed_image", "x", commit(&mut x, vec![], vec![], vec![(0x8002, Some(bad_image))], None, false));
        let mut not_url = vec![9u8];
        not_url.extend_from_slice(b"not a url");
        not_url.extend_from_slice(&[0, 0]);
        put("avatar_not_url", "x", commit(&mut x, vec![], vec![], vec![(0x8007, Some(not_url))], None, false));
        put("retention_7_bytes", "x", commit(&mut x, vec![], vec![], vec![(0x8005, Some(vec![0, 0, 0, 0, 0, 0x0e, 0x10]))], None, false));
        // L3: what MDK and its OpenMLS accept.
        put("ok_remove_absent", "x", commit(&mut x, vec![], vec![], vec![(0x9001, None)], None, false));
        let many = (0x9100u16..0x9114).map(|id| (id, Some(vec![id as u8]))).collect::<Vec<_>>();
        put("ok_twenty_updates", "x", commit(&mut x, vec![], vec![], many, None, false));
        // L1: X removes the observer, with a malformed component riding
        // along (every member refuses it), and the valid control.
        put("remove_observer_malformed", "x", commit(&mut x, vec![o_leaf], vec![], vec![(0x8002, Some(vec![0x01]))], None, false));
        put("ok_remove_observer", "x", commit(&mut x, vec![o_leaf], vec![], vec![], None, false));
        let lc = lifecycle_less(&o);

        // By reference: X's standalone rename, then X's Commit of it.
        let px = propose(&mut x, 0x8001, profile("renamed by reference"));
        let by_ref = commit(&mut x, vec![], vec![], vec![], None, true);
        x.group
            .as_mut()
            .unwrap()
            .clear_pending_proposals(x.provider.storage())
            .expect("clear proposals");
        // Y's (no admin) standalone rename, and X's Commit of it.
        let py = propose(&mut y, 0x8001, profile("renamed by Y's proposal"));
        receive_proposal(&mut x, &py);
        let by_ref_y = commit(&mut x, vec![], vec![], vec![], None, true);
        x.group
            .as_mut()
            .unwrap()
            .clear_pending_proposals(x.provider.storage())
            .expect("clear proposals");
        // W's standalone rename while an admin (epoch 1).
        let pw1 = propose(&mut w, 0x8001, profile("renamed by W at epoch 1"));
        // X demotes W (epoch 1 -> 2), merged; W and Y follow.  W then
        // proposes again, no longer an admin in its source epoch.
        let demote = commit_keep(&mut x, vec![(0x8003, Some(only_x.clone()))]);
        x.group.as_mut().unwrap().merge_pending_commit(&x.provider).expect("merge demote");
        apply(&mut w, &demote);
        apply(&mut y, &demote);
        let _ = y_leaf;
        let pw2 = propose(&mut w, 0x8001, profile("renamed by W at epoch 2"));
        receive_proposal(&mut x, &pw2);
        let by_ref_old_admin = commit(&mut x, vec![], vec![], vec![], None, true);

        json!({
            "observer_key_package": hex::encode(kp_o.tls_serialize_detached().unwrap()),
            "observer_private": o_private,
            "welcome": hex::encode(&welcome),
            "creator_account": hex::encode(identity(x.seed)),
            "w_account": hex::encode(identity(w.seed)),
            "y_account": hex::encode(identity(y.seed)),
            "w_leaf": w_leaf,
            "y_leaf": y_leaf,
            "o_leaf": o_leaf,
            "lifecycle_less": lc,
            "commits": Value::Object(c),
            "by_ref": {
                "proposal_x": hex::encode(px),
                "commit_x": hex::encode(by_ref),
                "proposal_y": hex::encode(py),
                "commit_x_of_y": hex::encode(by_ref_y),
                "proposal_w_epoch1": hex::encode(pw1),
                "demote_w": hex::encode(demote),
                "proposal_w_epoch2": hex::encode(pw2),
                "commit_x_of_w_epoch2": hex::encode(by_ref_old_admin),
            },
        })
    }

    /// commit() for one AppDataUpdate list, left pending (to be merged).
    fn commit_keep(m: &mut Member, ops: Vec<(u16, Option<Vec<u8>>)>) -> Vec<u8> {
        let group = m.group.as_mut().unwrap();
        let own_ops = ops.clone();
        let proposals = ops
            .into_iter()
            .map(|(id, data)| {
                Proposal::AppDataUpdate(Box::new(match data {
                    Some(d) => AppDataUpdateProposal::update(id, d),
                    None => AppDataUpdateProposal::remove(id),
                }))
            })
            .collect::<Vec<_>>();
        let mut builder = group
            .commit_builder()
            .consume_proposal_store(false)
            .force_self_update(true)
            .add_proposals(proposals)
            .load_psks(m.provider.storage())
            .expect("psks");
        let mut app_data = builder.app_data_dictionary_updater();
        for (id, data) in own_ops {
            if let Some(d) = data {
                app_data.set(ComponentData::from_parts(id, d.into()));
            }
        }
        let changes = app_data.changes();
        builder.with_app_data_dictionary_updates(changes);
        let bundle = builder
            .build(m.provider.rand(), m.provider.crypto(), &m.signer, |_| true)
            .expect("build")
            .stage_commit(&m.provider)
            .expect("stage");
        let (msg, _w, _gi) = bundle.into_messages();
        msg.tls_serialize_detached().expect("commit bytes")
    }
}

async fn emit(out: &str) {
    let fixture = json!({
        "provenance": {
            "generator": "nostrc W24b-H throwaway emitter (libmarmot/tests/vectors/mdk-0.11/commits-emitter)",
            "mdk_tag": "v0.11.0",
            "mdk_commit": "946e0547485c9a2c393c2048ec3a968fd50fb441",
            "openmls": "erskingardner/openmls@59e7d3b27a7e95237879dd5478de1fd90eff7ada (MDK workspace pin)",
            "lockfile": "MDK v0.11.0 Cargo.lock, unchanged except for this crate",
            "secrets": "test-only keys derived as in cgka-engine/tests/support/mod.rs; MLS keys random at capture",
        },
        "mdk_engine": mdk_engine().await,
        "openmls_forgeries": forgeries::emit(),
    });
    std::fs::write(out, serde_json::to_string_pretty(&fixture).unwrap()).expect("write");
    eprintln!("wrote {out}");
}

#[tokio::main(flavor = "multi_thread")]
async fn main() {
    let args: Vec<String> = std::env::args().collect();
    match args.get(1).map(String::as_str) {
        Some("emit") if args.len() == 3 => emit(&args[2]).await,
        _ => {
            eprintln!("usage: w24h-vectors emit <out.json>");
            std::process::exit(64);
        }
    }
}
