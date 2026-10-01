//! nostrc W24-E throwaway fixture emitter for MDK v0.11.0
//! (git 946e0547485c9a2c393c2048ec3a968fd50fb441). NOT part of MDK; never vendored.
//!
//! Subcommands:
//!   emit <out.json>                 MDK creates adopted groups; joiner keys exported
//!   joiner <dir>                    MDK KeyPackage for a libmarmot-created group
//!   join <dir> <rumor.json> <sender_sk_hex>
//!                                   MDK joins the libmarmot Welcome (NIP-59 wrapped here)

use std::sync::Arc;

use base64::Engine as _;
use base64::engine::general_purpose::STANDARD as B64;
use cgka_engine::account_identity_proof::{
    AccountIdentityProofRequest, AccountIdentityProofSigner,
};
use cgka_engine::feature_registry::FeatureRegistry;
use cgka_engine::key_package::key_package_metadata;
use cgka_engine::{Engine, EngineBuilder};
use cgka_traits::TransportEndpoint;
use cgka_traits::agent_text_stream::{
    AGENT_TEXT_STREAM_QUIC_FANOUT_CAPABILITY, AGENT_TEXT_STREAM_QUIC_FANOUT_FEATURE,
    AGENT_TEXT_STREAM_QUIC_RECEIVE_CAPABILITY, AGENT_TEXT_STREAM_QUIC_RECEIVE_FEATURE,
    AGENT_TEXT_STREAM_QUIC_SEND_CAPABILITY, AGENT_TEXT_STREAM_QUIC_SEND_FEATURE,
    AgentTextStreamQuicPolicyV1,
};
use cgka_traits::app_components::{
    AGENT_TEXT_STREAM_QUIC_COMPONENT_ID, AppComponentData, EncryptedMediaPolicyV2,
    GROUP_ADMIN_POLICY_COMPONENT_ID, GROUP_AVATAR_URL_COMPONENT_ID,
    GROUP_BLOSSOM_IMAGE_COMPONENT_ID, GROUP_ENCRYPTED_MEDIA_V1_COMPONENT_ID,
    GROUP_ENCRYPTED_MEDIA_V2_COMPONENT_ID, GROUP_LIFECYCLE_COMPONENT_ID,
    GROUP_MESSAGE_RETENTION_COMPONENT_ID, GROUP_PROFILE_COMPONENT_ID,
    NOSTR_ROUTING_COMPONENT_ID, NostrRoutingV1, encode_encrypted_media_policy_v2,
    encode_nostr_routing_v1,
};
use cgka_traits::capabilities::{Capability, CapabilityRequirement, RequirementLevel};
use cgka_traits::engine::{CgkaEngine, CreateGroupRequest, KeyPackage, SendResult};
use cgka_traits::group::ProtocolProfile;
use cgka_traits::storage::{AccountDeviceSignerStorage, KeyPackageBundleStorage, StorageProvider};
use cgka_traits::types::{MemberId, MessageId};
use k256::schnorr::SigningKey;
use k256::schnorr::signature::hazmat::PrehashSigner;
use nostr::nips::nip59::{GiftWrapBuilder, extract_rumor_async};
use nostr::prelude::*;
use openmls_basic_credential::SignatureKeyPair;
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use storage_sqlite::{SqlCipherKey, SqliteAccountStorage};
use tls_codec::Serialize as _;
use transport_nostr_adapter::NostrKeyPackagePublication;
use transport_nostr_peeler::{NostrMlsPeeler, NostrTransportEvent, SdkSigner};

const RELAY_A: &str = "wss://relay-a.example.com";
const RELAY_B: &str = "wss://relay-b.example.com";

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

struct Party {
    engine: Engine<SqliteAccountStorage>,
    storage: SqliteAccountStorage,
    keys: Keys,
    sk: [u8; 32],
    pk: [u8; 32],
}

fn app_feature_registry() -> FeatureRegistry {
    // Copy of crates/marmot-app/src/lib.rs `app_feature_registry` (White Noise).
    let mut registry = FeatureRegistry::new();
    registry.register(
        cgka_traits::capabilities::Feature("self-remove"),
        CapabilityRequirement {
            requires: Capability::Proposal(10),
            level: RequirementLevel::Required,
            description: "MIP-03 SelfRemove group departure",
        },
    );
    for (feature, capability, description) in [
        (
            AGENT_TEXT_STREAM_QUIC_RECEIVE_FEATURE.clone(),
            AGENT_TEXT_STREAM_QUIC_RECEIVE_CAPABILITY,
            "receive",
        ),
        (
            AGENT_TEXT_STREAM_QUIC_SEND_FEATURE.clone(),
            AGENT_TEXT_STREAM_QUIC_SEND_CAPABILITY,
            "send",
        ),
        (
            AGENT_TEXT_STREAM_QUIC_FANOUT_FEATURE.clone(),
            AGENT_TEXT_STREAM_QUIC_FANOUT_CAPABILITY,
            "fanout",
        ),
    ] {
        registry.register(
            feature,
            CapabilityRequirement {
                requires: capability,
                level: RequirementLevel::Optional,
                description,
            },
        );
    }
    registry
}

/// marmot-app `supported_app_component_ids` (White Noise).
fn app_components_supported() -> Vec<u16> {
    vec![
        GROUP_PROFILE_COMPONENT_ID,
        GROUP_BLOSSOM_IMAGE_COMPONENT_ID,
        GROUP_ADMIN_POLICY_COMPONENT_ID,
        NOSTR_ROUTING_COMPONENT_ID,
        GROUP_MESSAGE_RETENTION_COMPONENT_ID,
        AGENT_TEXT_STREAM_QUIC_COMPONENT_ID,
        GROUP_AVATAR_URL_COMPONENT_ID,
        GROUP_ENCRYPTED_MEDIA_V1_COMPONENT_ID,
        GROUP_ENCRYPTED_MEDIA_V2_COMPONENT_ID,
        GROUP_LIFECYCLE_COMPONENT_ID,
    ]
}

/// cgka-engine defaults (profile, admin policy, lifecycle) plus Nostr routing,
/// which every Nostr-routed group requires.
fn engine_components_supported() -> Vec<u16> {
    vec![
        GROUP_PROFILE_COMPONENT_ID,
        GROUP_ADMIN_POLICY_COMPONENT_ID,
        NOSTR_ROUTING_COMPONENT_ID,
        GROUP_LIFECYCLE_COMPONENT_ID,
    ]
}

fn party(
    seed: &[u8],
    storage: SqliteAccountStorage,
    components: Vec<u16>,
    registry: FeatureRegistry,
) -> Party {
    let signing = account_signing_key(seed);
    let sk: [u8; 32] = signing.to_bytes().into();
    let pk: [u8; 32] = signing.verifying_key().to_bytes().into();
    let keys = Keys::new(SecretKey::from_slice(&sk).expect("secret key"));
    let engine = EngineBuilder::new(storage.clone())
        .identity(pk.to_vec())
        .account_identity_proof_signer(Arc::new(ProofSigner(signing)))
        .supported_app_components(components)
        .feature_registry(registry)
        .protocol_profile(ProtocolProfile::Current)
        .peeler(Box::new(
            NostrMlsPeeler::new().with_welcome_signer(keys.clone()),
        ))
        .build()
        .expect("build current-profile engine");
    Party {
        engine,
        storage,
        keys,
        sk,
        pk,
    }
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

/// The joiner's private material, in the layout libmarmot stores under the
/// "kp_priv"/"kp_full" labels (init, encryption, Ed25519 seed + public key).
fn joiner_private(party: &Party) -> Value {
    let bundles = party
        .storage
        .stored_key_package_bundles()
        .expect("stored bundles");
    assert_eq!(bundles.len(), 1, "exactly one fresh KeyPackage bundle");
    let bundle: Value = serde_json::from_slice(&bundles[0].value).expect("bundle json");
    if std::env::var_os("W24E_DUMP_BUNDLE").is_some() {
        eprintln!("bundle keys: {}", serde_json::to_string(&bundle).unwrap().chars().take(600).collect::<String>());
    }
    let init = find_bytes(&bundle, "private_init_key").expect("private_init_key");
    let enc = find_bytes(&bundle, "private_encryption_key").expect("private_encryption_key");
    let binding = party
        .storage
        .account_device_signer(&MemberId::new(party.pk.to_vec()))
        .expect("signer binding")
        .expect("signer binding present");
    let signer = SignatureKeyPair::read(
        party.storage.mls_storage(),
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

/// A kind:30443 KeyPackage event exactly as marmot-app publishes it
/// (transport-nostr-adapter NostrKeyPackagePublication, tags from
/// key_package_metadata, app_components restricted to private-use ids).
fn key_package_event(party: &Party, kp: &KeyPackage, created_at: u64) -> Event {
    let metadata = key_package_metadata(kp).expect("kp metadata");
    let slot = {
        let mut h = Sha256::new();
        h.update(b"w24e-kp-slot");
        h.update(party.pk);
        hex::encode(h.finalize())
    };
    let publication = NostrKeyPackagePublication {
        client_name: None,
        account_id: MemberId::new(party.pk.to_vec()),
        key_package: kp.clone(),
        key_package_slot_id: slot,
        key_package_ref: metadata.key_package_ref_hex.clone(),
        mls_ciphersuite: format!("0x{:04x}", metadata.ciphersuite),
        mls_extensions: metadata
            .mls_extensions
            .iter()
            .map(|id| format!("0x{id:04x}"))
            .collect(),
        mls_proposals: metadata
            .mls_proposals
            .iter()
            .map(|id| format!("0x{id:04x}"))
            .collect(),
        app_components: metadata
            .app_components
            .iter()
            .filter(|id| **id >= cgka_traits::app_components::PRIVATE_USE_APP_COMPONENT_ID_START)
            .map(|id| format!("0x{id:04x}"))
            .collect(),
        publish_endpoints: vec![TransportEndpoint(RELAY_A.to_owned())],
    };
    let unsigned = publication.to_event_at(created_at).expect("kp event");
    sign_transport_event(&unsigned, &party.keys)
}

fn routing_component(nostr_group_id: [u8; 32]) -> AppComponentData {
    let routing = NostrRoutingV1::new(
        nostr_group_id,
        vec![RELAY_B.to_owned(), RELAY_A.to_owned()],
    )
    .expect("routing");
    AppComponentData {
        component_id: NOSTR_ROUTING_COMPONENT_ID,
        data: encode_nostr_routing_v1(&routing).expect("encode routing"),
    }
}

fn load_group(
    storage: &SqliteAccountStorage,
    gid: &cgka_traits::types::GroupId,
) -> openmls::group::MlsGroup {
    openmls::group::MlsGroup::load(
        storage.mls_storage(),
        &openmls::group::GroupId::from_slice(gid.as_slice()),
    )
    .expect("load group")
    .expect("group stored")
}

async fn unwrap_welcome(
    welcome: &cgka_traits::transport::TransportMessage,
    recipient: &Keys,
) -> (String, Vec<u8>) {
    let event = NostrTransportEvent::from_transport_message(welcome).expect("welcome event");
    let gift_wrap = event.to_verified_nostr_event().expect("verified gift wrap");
    let unwrapped = extract_rumor_async(&SdkSigner(Arc::new(recipient.clone())), &gift_wrap)
        .await
        .expect("unwrap welcome");
    let rumor_json = unwrapped.rumor.as_json();
    let bytes = B64
        .decode(unwrapped.rumor.content.as_bytes())
        .expect("base64");
    (rumor_json, bytes)
}

struct Scenario {
    creator_seed: &'static [u8],
    joiner_seed: &'static [u8],
    registry: fn() -> FeatureRegistry,
    components: fn() -> Vec<u16>,
    white_noise_app: bool,
}

async fn run_scenario(name: &str, s: &Scenario) -> Value {
    let mut creator = party(
        s.creator_seed,
        SqliteAccountStorage::in_memory().unwrap(),
        (s.components)(),
        (s.registry)(),
    );
    let mut joiner = party(
        s.joiner_seed,
        SqliteAccountStorage::in_memory().unwrap(),
        (s.components)(),
        (s.registry)(),
    );
    let created_at = 1_790_000_000u64;
    let kp = joiner.engine.fresh_key_package().await.expect("fresh kp");
    let kp_event = key_package_event(&joiner, &kp, created_at);
    let kp_private = joiner_private(&joiner);
    let kp_metadata = key_package_metadata(&kp).expect("metadata");

    let mut nostr_group_id = [0u8; 32];
    let mut h = Sha256::new();
    h.update(b"w24e-nostr-group-id");
    h.update(name.as_bytes());
    nostr_group_id.copy_from_slice(&h.finalize());
    let mut app_components = vec![routing_component(nostr_group_id)];
    if s.white_noise_app {
        // marmot-app client create_group_with_initial_source: routing, agent
        // text stream (user_to_agent_default), encrypted media v2.
        app_components.push(
            AgentTextStreamQuicPolicyV1::user_to_agent_default()
                .to_app_component_data()
                .expect("agent stream"),
        );
        app_components.push(AppComponentData {
            component_id: GROUP_ENCRYPTED_MEDIA_V2_COMPONENT_ID,
            data: encode_encrypted_media_policy_v2(
                &EncryptedMediaPolicyV2::blossom_default(["https://blossom.example.com".to_owned()])
                    .expect("media policy"),
            )
            .expect("encode media"),
        });
    }
    let event_id = MessageId::new(kp_event.id.to_bytes().to_vec());
    let kp_with_source = KeyPackage::with_source_event_id(kp.bytes().to_vec(), event_id)
        .with_protocol_profile(ProtocolProfile::Current);
    let (group_id, result) = creator
        .engine
        .create_group(CreateGroupRequest {
            name: format!("W24-E {name}"),
            description: "MDK v0.11.0 fixture".into(),
            members: vec![kp_with_source],
            required_features: vec![],
            app_components,
            initial_admins: vec![],
        })
        .await
        .expect("create group");
    let welcome = match result {
        SendResult::FoundingGroupCreated { mut welcomes } => welcomes.remove(0),
        other => panic!("expected FoundingGroupCreated, got {other:?}"),
    };
    let (rumor_json, welcome_bytes) = unwrap_welcome(&welcome, &joiner.keys).await;

    let group = load_group(&creator.storage, &group_id);
    let gc_extensions = group
        .extensions()
        .tls_serialize_detached()
        .expect("serialize extensions");
    let ratchet_tree = group
        .export_ratchet_tree()
        .tls_serialize_detached()
        .expect("serialize tree");
    let epoch = group.epoch().as_u64();

    // Control: MDK itself joins the Welcome it built.
    let joined = joiner.engine.join_welcome(welcome).await.expect("MDK joins");
    let joined_record = joiner.engine.group_record(&joined).expect("joined record");

    json!({
        "creator_account_public": hex::encode(creator.pk),
        "joiner_account_public": hex::encode(joiner.pk),
        "joiner_account_secret_test_only": hex::encode(joiner.sk),
        "joiner_key_package_mls_message": hex::encode(kp.bytes()),
        "joiner_key_package_ref": kp_metadata.key_package_ref_hex,
        "joiner_key_package_app_components": kp_metadata.app_components,
        "joiner_key_package_event_json": kp_event.as_json(),
        "joiner_private": kp_private,
        "welcome_rumor_json": rumor_json,
        "welcome_mls_message": hex::encode(&welcome_bytes),
        "group_id": hex::encode(group_id.as_slice()),
        "epoch": epoch,
        "group_context_extensions": hex::encode(gc_extensions),
        "ratchet_tree": hex::encode(ratchet_tree),
        "nostr_group_id": hex::encode(nostr_group_id),
        "relays": [RELAY_A, RELAY_B],
        "mdk_control_join_profile": format!("{:?}", joined_record.protocol_profile),
        "mdk_control_join_members": joined_record.members.len(),
        "creator_secret_test_only": hex::encode(creator.sk),
    })
}


// ── OpenMLS-direct negatives (same pinned OpenMLS as MDK v0.11.0) ─────────

mod negatives {
    use super::*;
    use cgka_engine::account_identity_proof::account_identity_proof_component;
    use cgka_traits::app_components::encode_components_list;
    use openmls::extensions::{AppDataDictionary, AppDataDictionaryExtension};
    use openmls::prelude::{
        BasicCredential, Capabilities, CredentialWithKey, Extension, ExtensionType, Extensions,
        KeyPackage as MlsKeyPackage, LeafNode, MlsGroup, MlsGroupCreateConfig, ProposalType,
        RequiredCapabilitiesExtension, UnknownExtension,
    };
    use openmls::group::GroupContext;
    use openmls_rust_crypto::OpenMlsRustCrypto;
    use openmls_traits::types::Ciphersuite;
    use std::collections::BTreeSet;

    const CS: Ciphersuite = Ciphersuite::MLS_128_DHKEMX25519_AES128GCM_SHA256_Ed25519;
    /// libmarmot's adopted leaf advertisement (MLS_ADOPTED_SUPPORTED_COMPONENTS + 0x0001).
    const LEAF_COMPONENTS: [u16; 6] = [0x0001, 0x8001, 0x8003, 0x8004, 0x8009, 0x800c];
    const REQUIRED_COMPONENTS: [u16; 5] = [0x8001, 0x8003, 0x8004, 0x8009, 0x800c];

    #[derive(Clone, Copy, Debug)]
    pub enum Variant {
        Control,
        BadProof,
        MissingProof,
        MixedGroup,
        MissingRequiredCapability,
        NonAdminInviter,
    }

    fn identity(seed: &[u8]) -> [u8; 32] {
        account_signing_key(seed).verifying_key().to_bytes().into()
    }

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
            Some(&[ExtensionType::AppDataDictionary, ExtensionType::Unknown(0xF2EE)]),
            Some(&[ProposalType::AppDataUpdate]),
            None,
        )
    }

    pub struct Joiner {
        pub key_package: MlsKeyPackage,
        pub private: Value,
    }

    pub fn joiner(provider: &OpenMlsRustCrypto) -> Joiner {
        let seed = b"w24e-neg-joiner";
        let signer = SignatureKeyPair::new(CS.signature_algorithm()).unwrap();
        let cwk = CredentialWithKey {
            credential: BasicCredential::new(identity(seed).to_vec()).into(),
            signature_key: signer.public().into(),
        };
        let bundle = MlsKeyPackage::builder()
            .leaf_node_capabilities(capabilities())
            .leaf_node_extensions(leaf_extensions(Some(proof(seed, &signer.to_public_vec(), false))))
            .build(CS, provider, &signer, cwk)
            .expect("joiner kp");
        let bundle_json = serde_json::to_value(&bundle).expect("bundle json");
        let signer_json = serde_json::to_value(&signer).expect("signer json");
        let private = json!({
            "init_key_private": hex::encode(find_bytes(&bundle_json, "private_init_key").unwrap()),
            "encryption_key_private": hex::encode(find_bytes(&bundle_json, "private_encryption_key").unwrap()),
            "signature_key_private_seed": hex::encode(&find_bytes(&signer_json, "private").unwrap()[..32]),
            "signature_key_public": hex::encode(signer.to_public_vec()),
        });
        Joiner { key_package: bundle.key_package().clone(), private }
    }

    fn group_context(v: Variant) -> Extensions<GroupContext> {
        let proposals: Vec<ProposalType> = match v {
            Variant::MissingRequiredCapability => vec![],
            _ => vec![ProposalType::AppDataUpdate],
        };
        let required = RequiredCapabilitiesExtension::new(
            &[ExtensionType::AppDataDictionary],
            &proposals,
            &[],
        );
        let admin = match v {
            Variant::NonAdminInviter => identity(b"w24e-neg-joiner"),
            _ => identity(b"w24e-neg-creator"),
        };
        let mut admin_state = vec![0x20u8];
        admin_state.extend_from_slice(&admin);
        let mut dict = AppDataDictionary::new();
        dict.insert(0x0001, encode_components_list(&REQUIRED_COMPONENTS.iter().copied().collect()));
        dict.insert(0x8001, cgka_traits::app_components::encode_group_profile_v1(
            &cgka_traits::app_components::GroupProfileV1 {
                name: format!("W24-E negative {v:?}"),
                description: String::new(),
            },
        ).unwrap());
        dict.insert(0x8003, admin_state);
        dict.insert(0x8004, routing_component([0x42; 32]).data);
        dict.insert(0x800c, vec![0]);
        let mut exts = vec![
            Extension::RequiredCapabilities(required),
            Extension::AppDataDictionary(AppDataDictionaryExtension::new(dict)),
        ];
        if matches!(v, Variant::MixedGroup) {
            // A legacy MIP-01 marmot_group_data next to the adopted state.
            exts.push(Extension::Unknown(0xF2EE, UnknownExtension(vec![0x00, 0x02])));
        }
        Extensions::from_vec(exts).expect("group context")
    }

    /// One real Welcome for the shared joiner from a group built for `v`.
    pub fn welcome(provider: &OpenMlsRustCrypto, joiner: &MlsKeyPackage, v: Variant) -> Vec<u8> {
        let seed = b"w24e-neg-creator";
        let signer = SignatureKeyPair::new(CS.signature_algorithm()).unwrap();
        let cwk = CredentialWithKey {
            credential: BasicCredential::new(identity(seed).to_vec()).into(),
            signature_key: signer.public().into(),
        };
        let leaf_proof = match v {
            Variant::MissingProof => None,
            Variant::BadProof => Some(proof(seed, &signer.to_public_vec(), true)),
            _ => Some(proof(seed, &signer.to_public_vec(), false)),
        };
        let config = MlsGroupCreateConfig::builder()
            .ciphersuite(CS)
            .capabilities(capabilities())
            .with_leaf_node_extensions(leaf_extensions(leaf_proof))
            .expect("leaf extensions")
            .with_group_context_extensions(group_context(v))
            .use_ratchet_tree_extension(true)
            .build();
        let mut group = MlsGroup::new(provider, &signer, &config, cwk).expect("group");
        let (_commit, welcome, _gi) = group
            .add_members(provider, &signer, &[joiner.clone()])
            .expect("add member");
        welcome.tls_serialize_detached().expect("welcome bytes")
    }
}

fn emit_negatives() -> Value {
    use negatives::Variant::*;
    let provider = openmls_rust_crypto::OpenMlsRustCrypto::default();
    let joiner = negatives::joiner(&provider);
    let kp_bytes = joiner.key_package.tls_serialize_detached().expect("kp bytes");
    let mut welcomes = serde_json::Map::new();
    for v in [Control, BadProof, MissingProof, MixedGroup, MissingRequiredCapability, NonAdminInviter] {
        let w = negatives::welcome(&provider, &joiner.key_package, v);
        welcomes.insert(format!("{v:?}"), Value::String(hex::encode(w)));
    }
    json!({
        "joiner_key_package": hex::encode(kp_bytes),
        "joiner_private": joiner.private,
        "welcomes": Value::Object(welcomes),
    })
}

async fn emit(out: &str) {
    let engine_default = Scenario {
        creator_seed: b"w24e-mdk-creator",
        joiner_seed: b"w24e-mdk-joiner",
        registry: FeatureRegistry::new,
        components: engine_components_supported,
        white_noise_app: false,
    };
    let white_noise = Scenario {
        creator_seed: b"w24e-wn-creator",
        joiner_seed: b"w24e-wn-joiner",
        registry: app_feature_registry,
        components: app_components_supported,
        white_noise_app: true,
    };
    let fixture = json!({
        "provenance": {
            "generator": "nostrc W24-E throwaway emitter (/tmp/w24e/mdk-v0.11.0/crates/w24e-vectors)",
            "mdk_tag": "v0.11.0",
            "mdk_commit": "946e0547485c9a2c393c2048ec3a968fd50fb441",
            "openmls": "erskingardner/openmls@59e7d3b27a7e95237879dd5478de1fd90eff7ada (MDK workspace pin)",
            "lockfile": "MDK v0.11.0 Cargo.lock, unchanged except for this crate",
            "secrets": "test-only keys derived as in cgka-engine/tests/support/mod.rs; MLS keys random at capture",
        },
        "engine_default": run_scenario("engine-default", &engine_default).await,
        "white_noise_app": run_scenario("white-noise-app", &white_noise).await,
        "openmls_negatives": emit_negatives(),
    });
    std::fs::write(out, serde_json::to_string_pretty(&fixture).unwrap()).expect("write");
    eprintln!("wrote {out}");
}

fn reverse_party(dir: &str) -> Party {
    let key = SqlCipherKey::new("w24e-throwaway-test-key").unwrap();
    let storage = SqliteAccountStorage::open_encrypted(format!("{dir}/mdk-joiner.db"), &key)
        .expect("open storage");
    party(
        b"w24e-reverse-joiner",
        storage,
        engine_components_supported(),
        FeatureRegistry::new(),
    )
}

async fn joiner(dir: &str) {
    std::fs::create_dir_all(dir).unwrap();
    let mut p = reverse_party(dir);
    let kp = p.engine.fresh_key_package().await.expect("fresh kp");
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_secs();
    let event = key_package_event(&p, &kp, now);
    std::fs::write(format!("{dir}/kp_event.json"), event.as_json()).unwrap();
    std::fs::write(format!("{dir}/joiner_pubkey.hex"), hex::encode(p.pk)).unwrap();
    eprintln!("joiner {} kp {}", hex::encode(p.pk), event.id);
}

async fn join(dir: &str, rumor_path: &str, sender_sk_hex: &str) {
    let mut p = reverse_party(dir);
    let rumor_json = std::fs::read_to_string(rumor_path).expect("rumor");
    let rumor = UnsignedEvent::from_json(&rumor_json).expect("rumor json");
    let sender = Keys::new(SecretKey::from_hex(sender_sk_hex).expect("sender sk"));
    let gift_wrap = GiftWrapBuilder::new(p.keys.public_key(), rumor)
        .finalize_async(&SdkSigner(Arc::new(sender)))
        .await
        .expect("gift wrap");
    let msg = NostrTransportEvent::from_nostr_event(&gift_wrap)
        .expect("transport event")
        .to_transport_message()
        .expect("transport message");
    match p.engine.join_welcome(msg).await {
        Ok(gid) => {
            let record = p.engine.group_record(&gid).expect("record");
            let group = load_group(&p.storage, &gid);
            println!(
                "{}",
                json!({
                    "result": "joined",
                    "group_id": hex::encode(gid.as_slice()),
                    "protocol_profile": format!("{:?}", record.protocol_profile),
                    "members": record.members.len(),
                    "epoch": group.epoch().as_u64(),
                    "admins": p.engine.admin_pubkeys(&gid).expect("admins").iter().map(hex::encode).collect::<Vec<_>>(),
                    "required_app_components": record.required_capabilities.app_components.ids,
                })
            );
        }
        Err(e) => {
            println!("{}", json!({"result": "refused", "error": format!("{e:?}")}));
            std::process::exit(2);
        }
    }
}

#[tokio::main(flavor = "multi_thread")]
async fn main() {
    let args: Vec<String> = std::env::args().collect();
    match args.get(1).map(String::as_str) {
        Some("emit") => emit(&args[2]).await,
        Some("joiner") => joiner(&args[2]).await,
        Some("join") => join(&args[2], &args[3], &args[4]).await,
        _ => {
            eprintln!(
                "usage: w24e-vectors emit <out.json> | joiner <dir> | join <dir> <rumor.json> <sender_sk_hex>"
            );
            std::process::exit(64);
        }
    }
}
