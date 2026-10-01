//! nostrc W25 slice N throwaway fixture emitter for MDK v0.11.0
//! (git 946e0547485c9a2c393c2048ec3a968fd50fb441). NOT part of MDK; never vendored.
//!
//!   emit <out.json>
//!
//! Forks resolved by MDK's own convergence (nostrc-w1m0), for libmarmot's
//! branch selection: real MDK v0.11.0 `cgka-engine` instances (as the W24b
//! slice H emitter, `commits-emitter/`, whose helpers this copies), each
//! committing from the same epoch without seeing the others' Commits, then
//! each fed the others' and settled on the manual convergence clock.  MDK's
//! verdict (every party's epoch and group name after settling) and an
//! application message of the converged epoch are recorded; an observer
//! whose KeyPackage private keys are recorded is the member libmarmot joins.
//!
//! - `race`: two admins rename from one epoch (privileged both): the lower
//!   committer key wins.
//! - `depth2`: an admin's rename (privileged) against a member's two
//!   self-updates on its own branch: the deeper branch wins.
//! - `witnessed`: two admins rename from one epoch; the one whose key sorts
//!   last is witnessed by its committer's message at its epoch.

use std::sync::Arc;

use base64::Engine as _;
use base64::engine::general_purpose::STANDARD as B64;
use cgka_engine::account_identity_proof::{
    AccountIdentityProofRequest, AccountIdentityProofSigner, account_identity_proof_component,
};
use cgka_engine::feature_registry::FeatureRegistry;
use cgka_engine::key_package::key_package_metadata;
use cgka_engine::{Engine, EngineBuilder, ManualConvergenceClock};
use cgka_traits::GroupStorage;
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


async fn verdict(parties: &[&Party], gid: &GroupId) -> Value {
    let mut out = serde_json::Map::new();
    for p in parties {
        let g = p.storage.get_group(gid).expect("group");
        out.insert(p.name.to_string(), json!({ "epoch": g.epoch.0, "name": g.name }));
    }
    Value::Object(out)
}

async fn deliver_all(p: &mut Party, msgs: &[&TransportMessage], gid: &GroupId) {
    for m in msgs {
        let extra = deliver(p, m, gid).await;
        assert!(extra.is_empty(), "{}: no Commit of its own", p.name);
    }
}

fn agreed(v: &Value) -> (u64, String) {
    let obj = v.as_object().unwrap();
    let first = obj.values().next().unwrap();
    for (who, x) in obj {
        assert_eq!(x, first, "MDK parties disagree after settling: {who}");
    }
    (first["epoch"].as_u64().unwrap(), first["name"].as_str().unwrap().to_owned())
}

async fn forks() -> Value {
    let mut alice = party("alice", b"w25n-mdk-alice");
    let mut bob = party("bob", b"w25n-mdk-bob");
    let mut carol = party("carol", b"w25n-mdk-carol");
    let mut observer = party("observer", b"w25n-mdk-observer");

    let (kp_bob, _) = fresh_kp(&mut bob).await;
    let (kp_carol, _) = fresh_kp(&mut carol).await;
    let (kp_obs, ev_obs) = fresh_kp(&mut observer).await;
    let obs_private = party_private(&observer);
    let obs_meta = key_package_metadata(&kp_obs).expect("meta");

    let mut nostr_group_id = [0u8; 32];
    nostr_group_id.copy_from_slice(&Sha256::digest(b"w25n-nostr-group-id"));
    let (gid, result) = alice
        .engine
        .create_group(CreateGroupRequest {
            name: "W25-N forks".into(),
            description: "MDK v0.11.0 convergence fixture".into(),
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
    let mut verdicts = serde_json::Map::new();

    // Bob becomes a co-admin (0x8003): both can rename.
    let co_admins = admin_policy(vec![alice.pk, bob.pk]);
    let (msg, _) = evolve(&mut alice, SendIntent::UpdateAppComponents {
        group_id: gid.clone(),
        updates: vec![AppComponentData {
            component_id: GROUP_ADMIN_POLICY_COMPONENT_ID,
            data: co_admins,
        }],
    })
    .await;
    for p in [&mut bob, &mut carol] {
        deliver_all(p, &[&msg], &gid).await;
    }
    steps.push(step("admin_change", "commit", &alice, &msg, alice.engine.epoch(&gid).unwrap().0));

    // ── race: two privileged renames from one epoch ──
    let (ra, _) = evolve(&mut alice, SendIntent::UpdateGroupData {
        group_id: gid.clone(), name: Some("race: Alice's".into()), description: None,
    }).await;
    let (rb, _) = evolve(&mut bob, SendIntent::UpdateGroupData {
        group_id: gid.clone(), name: Some("race: Bob's".into()), description: None,
    }).await;
    steps.push(step("race_alice", "commit", &alice, &ra, alice.engine.epoch(&gid).unwrap().0));
    steps.push(step("race_bob", "commit", &bob, &rb, bob.engine.epoch(&gid).unwrap().0));
    deliver_all(&mut alice, &[&rb], &gid).await;
    deliver_all(&mut bob, &[&ra], &gid).await;
    deliver_all(&mut carol, &[&ra, &rb], &gid).await;
    let v = verdict(&[&alice, &bob, &carol], &gid).await;
    let (epoch, name) = agreed(&v);
    verdicts.insert("race".into(), json!({ "parties": v, "epoch": epoch, "name": name }));
    if let Some(app) = app_message(&mut carol, &gid, "after the race").await {
        steps.push(step("message_after_race", "application", &carol, &app, epoch));
    }

    // ── depth2: a privileged rename against a member's two self-updates ──
    let (r2, _) = evolve(&mut alice, SendIntent::UpdateGroupData {
        group_id: gid.clone(), name: Some("depth2: Alice's".into()), description: None,
    }).await;
    let (c1, _) = evolve(&mut carol, SendIntent::SelfUpdate { group_id: gid.clone() }).await;
    let (c2, _) = evolve(&mut carol, SendIntent::SelfUpdate { group_id: gid.clone() }).await;
    steps.push(step("depth2_rename", "commit", &alice, &r2, alice.engine.epoch(&gid).unwrap().0));
    steps.push(step("depth2_first", "commit", &carol, &c1, carol.engine.epoch(&gid).unwrap().0 - 1));
    steps.push(step("depth2_second", "commit", &carol, &c2, carol.engine.epoch(&gid).unwrap().0));
    deliver_all(&mut alice, &[&c1, &c2], &gid).await;
    deliver_all(&mut bob, &[&r2, &c1, &c2], &gid).await;
    deliver_all(&mut carol, &[&r2], &gid).await;
    let v = verdict(&[&alice, &bob, &carol], &gid).await;
    let (epoch, name) = agreed(&v);
    verdicts.insert("depth2".into(), json!({ "parties": v, "epoch": epoch, "name": name }));
    if let Some(app) = app_message(&mut alice, &gid, "after depth2").await {
        steps.push(step("message_after_depth2", "application", &alice, &app, epoch));
    }

    // ── witnessed: the rename whose committer's key sorts last is witnessed ──
    let (hi, lo) = if alice.pk > bob.pk { ("alice", "bob") } else { ("bob", "alice") };
    let (wh, wh_msg, wl) = {
        let (high, low) = if hi == "alice" { (&mut alice, &mut bob) } else { (&mut bob, &mut alice) };
        let (wh, _) = evolve(high, SendIntent::UpdateGroupData {
            group_id: gid.clone(), name: Some(format!("witnessed: {hi}'s")), description: None,
        }).await;
        let wh_msg = app_message(high, &gid, "witness of the higher key's rename").await.expect("witness");
        let (wl, _) = evolve(low, SendIntent::UpdateGroupData {
            group_id: gid.clone(), name: Some(format!("witnessed: {lo}'s")), description: None,
        }).await;
        (wh, wh_msg, wl)
    };
    let epoch_wh = alice.engine.epoch(&gid).unwrap().0;
    {
        let (high, low) = if hi == "alice" { (&alice, &bob) } else { (&bob, &alice) };
        steps.push(step("witnessed_high", "commit", high, &wh, epoch_wh));
        steps.push(step("witness_message", "application", high, &wh_msg, epoch_wh));
        steps.push(step("witnessed_low", "commit", low, &wl, epoch_wh));
    }
    {
        let (high, low) = if hi == "alice" { (&mut alice, &mut bob) } else { (&mut bob, &mut alice) };
        deliver_all(high, &[&wl], &gid).await;
        deliver_all(low, &[&wh, &wh_msg], &gid).await;
    }
    deliver_all(&mut carol, &[&wl, &wh, &wh_msg], &gid).await;
    let v = verdict(&[&alice, &bob, &carol], &gid).await;
    let (epoch, name) = agreed(&v);
    verdicts.insert("witnessed".into(), json!({ "parties": v, "epoch": epoch, "name": name,
                                                "higher_key": hi }));
    if let Some(app) = app_message(&mut carol, &gid, "after witnessed").await {
        steps.push(step("message_after_witnessed", "application", &carol, &app, epoch));
    }

    json!({
        "accounts": {
            "alice": hex::encode(alice.pk), "bob": hex::encode(bob.pk), "carol": hex::encode(carol.pk),
            "observer": hex::encode(observer.pk),
        },
        "observer_key_package_mls_message": hex::encode(kp_obs.bytes()),
        "observer_key_package_ref": obs_meta.key_package_ref_hex,
        "observer_key_package_event_json": ev_obs.as_json(),
        "observer_private": obs_private,
        "welcome_rumor_json": observer_rumor,
        "group_id": hex::encode(gid.as_slice()),
        "nostr_group_id": hex::encode(nostr_group_id),
        "relays": [RELAY_A, RELAY_B],
        "steps": steps,
        "verdicts": verdicts,
    })
}

async fn emit(out: &str) {
    let fixture = json!({
        "provenance": {
            "generator": "nostrc W25 N throwaway emitter (libmarmot/tests/vectors/mdk-0.11/forks-emitter)",
            "mdk_tag": "v0.11.0",
            "mdk_commit": "946e0547485c9a2c393c2048ec3a968fd50fb441",
            "openmls": "erskingardner/openmls@59e7d3b27a7e95237879dd5478de1fd90eff7ada (MDK workspace pin)",
            "lockfile": "MDK v0.11.0 Cargo.lock, unchanged except for this crate",
            "secrets": "test-only keys derived as in cgka-engine/tests/support/mod.rs; MLS keys random at capture",
        },
        "forks": forks().await,
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
            eprintln!("usage: w25n-vectors emit <out.json>");
            std::process::exit(64);
        }
    }
}
