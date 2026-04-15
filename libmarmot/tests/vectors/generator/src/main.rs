// Copyright (c) 2025 nostrc contributors
// SPDX-License-Identifier: MIT
//
// Marmot test vector generator — uses MDK (Rust reference implementation)
// to produce JSON test vectors for cross-implementation validation.
//
// Usage:
//   cargo run --release > ../mdk/protocol-vectors.json
//
// Bead: nostrc-mm51

use mdk_core::prelude::*;
use mdk_core::key_packages::KeyPackageEventData;
use mdk_memory_storage::MdkMemoryStorage;
use mdk_storage_traits::GroupId;
use nostr::event::builder::EventBuilder;
use nostr::{EventId, Keys, Kind, RelayUrl, TagKind};
use serde::Serialize;
use sha2::{Sha256, Digest};

/// Top-level output: all test vectors in one JSON file
#[derive(Serialize)]
struct TestVectors {
    description: String,
    mdk_version: String,
    ciphersuite: u16,
    key_package: KeyPackageVectors,
    group_lifecycle: GroupLifecycleVector,
}

#[derive(Serialize)]
struct KeyPackageVectors {
    description: String,
    cases: Vec<KeyPackageVector>,
}

#[derive(Serialize)]
struct KeyPackageVector {
    name: String,
    nostr_pubkey: String,
    relay_urls: Vec<String>,
    /// Hex-encoded TLS-serialized KeyPackage (content is hex of the base64-decoded bytes)
    serialized_key_package_hex: String,
    /// The base64 event content string as MDK produces it
    event_content_base64: String,
    /// Hex-encoded KeyPackageRef (SHA-256 of TLS serialization)
    key_package_ref: String,
    /// Event tags as array of string arrays
    event_tags: Vec<Vec<String>>,
}

#[derive(Serialize)]
struct GroupLifecycleVector {
    description: String,
    alice_pubkey: String,
    bob_pubkey: String,
    relay_url: String,
    steps: Vec<LifecycleStep>,
}

#[derive(Serialize)]
struct LifecycleStep {
    name: String,
    description: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    event_kind: Option<u16>,
    #[serde(skip_serializing_if = "Option::is_none")]
    event_content: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    event_tags: Option<Vec<Vec<String>>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    group_name: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    nostr_group_id: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    mls_group_id: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    member_count: Option<usize>,
    #[serde(skip_serializing_if = "Option::is_none")]
    message_content: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    message_kind: Option<u16>,
    #[serde(skip_serializing_if = "Option::is_none")]
    message_pubkey: Option<String>,
}

fn tags_to_vecs(tags: &[nostr::Tag]) -> Vec<Vec<String>> {
    tags.iter()
        .map(|t| t.as_slice().iter().map(|s| s.to_string()).collect())
        .collect()
}

fn decode_content_to_hex(content: &str) -> String {
    // MDK produces hex-encoded content for key packages
    // Try hex-decode first, then base64
    if let Ok(bytes) = hex::decode(content) {
        return hex::encode(bytes);
    }
    // Try base64
    use std::io::Read;
    let decoded = base64_decode(content);
    hex::encode(decoded)
}

fn base64_decode(input: &str) -> Vec<u8> {
    // Simple base64 decode
    use std::io::Read;
    let engine = base64_engine();
    base64::Engine::decode(&engine, input).unwrap_or_default()
}

fn base64_engine() -> base64::engine::general_purpose::GeneralPurpose {
    base64::engine::general_purpose::STANDARD
}

fn compute_sha256(data: &[u8]) -> String {
    let mut hasher = Sha256::new();
    hasher.update(data);
    hex::encode(hasher.finalize())
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let relay_url = RelayUrl::parse("wss://relay.example.com").unwrap();

    // ════════════════════════════════════════════════════════════════
    // 1. Key Package Vectors
    // ════════════════════════════════════════════════════════════════

    let mut kp_cases = Vec::new();

    // Generate a few key packages with different identities
    for i in 0..3 {
        let keys = Keys::generate();
        let mdk = MDK::new(MdkMemoryStorage::default());

        let KeyPackageEventData {
            content,
            tags_30443: tags,
            hash_ref,
            d_tag,
            ..
        } = mdk.create_key_package_for_event(&keys.public_key(), [relay_url.clone()])?;

        // Content is base64-encoded TLS serialization
        use base64::Engine;
        let kp_bytes = base64::engine::general_purpose::STANDARD.decode(&content)?;
        let _kp_ref_computed = compute_sha256(&kp_bytes);

        kp_cases.push(KeyPackageVector {
            name: format!("key_package_{}", i),
            nostr_pubkey: keys.public_key().to_string(),
            relay_urls: vec!["wss://relay.example.com".into()],
            serialized_key_package_hex: hex::encode(&kp_bytes),
            event_content_base64: content.clone(),
            key_package_ref: hex::encode(&hash_ref),
            event_tags: tags_to_vecs(&tags),
        });
    }

    // ════════════════════════════════════════════════════════════════
    // 2. Group Lifecycle Vector
    // ════════════════════════════════════════════════════════════════

    let alice_keys = Keys::generate();
    let bob_keys = Keys::generate();
    let alice_mdk = MDK::new(MdkMemoryStorage::default());
    let bob_mdk = MDK::new(MdkMemoryStorage::default());

    let mut steps = Vec::new();

    // Step 1: Bob creates key package
    let bob_kp_data = bob_mdk.create_key_package_for_event(
        &bob_keys.public_key(),
        [relay_url.clone()],
    )?;

    let bob_kp_event = EventBuilder::new(Kind::Custom(30443), bob_kp_data.content.clone())
        .tags(bob_kp_data.tags_30443.clone())
        .build(bob_keys.public_key())
        .sign(&bob_keys)
        .await?;

    steps.push(LifecycleStep {
        name: "bob_key_package".into(),
        description: "Bob publishes kind:30443 key package event".into(),
        event_kind: Some(30443),
        event_content: Some(bob_kp_data.content.clone()),
        event_tags: Some(tags_to_vecs(&bob_kp_data.tags_30443)),
        group_name: None,
        nostr_group_id: None,
        mls_group_id: None,
        member_count: None,
        message_content: None,
        message_kind: None,
        message_pubkey: None,
    });

    // Step 2: Alice creates group with Bob
    let config = NostrGroupConfigData::new(
        "Test Group".to_owned(),
        "A test group for vector generation".to_owned(),
        None, None, None,
        vec![relay_url.clone()],
        vec![alice_keys.public_key(), bob_keys.public_key()],
    );

    let group_result = alice_mdk.create_group(
        &alice_keys.public_key(),
        vec![bob_kp_event.clone()],
        config,
    )?;

    let alice_group = &group_result.group;
    let welcome_rumor = group_result.welcome_rumors.first()
        .expect("Should have welcome rumor for Bob");

    steps.push(LifecycleStep {
        name: "alice_creates_group".into(),
        description: "Alice creates group with Bob, producing welcome rumor".into(),
        event_kind: Some(444),
        event_content: Some(welcome_rumor.content.to_string()),
        event_tags: Some(tags_to_vecs(welcome_rumor.tags.as_slice())),
        group_name: Some("Test Group".into()),
        nostr_group_id: Some(hex::encode(&alice_group.nostr_group_id)),
        mls_group_id: Some(hex::encode(alice_group.mls_group_id.as_slice())),
        member_count: Some(2),
        message_content: None,
        message_kind: None,
        message_pubkey: None,
    });

    // Step 3: Bob processes welcome
    bob_mdk.process_welcome(&EventId::all_zeros(), welcome_rumor)?;
    let welcomes = bob_mdk.get_pending_welcomes(None)?;
    let welcome = welcomes.first().unwrap();

    steps.push(LifecycleStep {
        name: "bob_processes_welcome".into(),
        description: "Bob processes welcome, sees pending invitation".into(),
        event_kind: None,
        event_content: None,
        event_tags: None,
        group_name: Some(welcome.group_name.clone()),
        nostr_group_id: None,
        mls_group_id: None,
        member_count: Some(welcome.member_count as usize),
        message_content: None,
        message_kind: None,
        message_pubkey: None,
    });

    // Step 4: Bob accepts welcome
    bob_mdk.accept_welcome(welcome)?;
    let bobs_groups = bob_mdk.get_groups()?;
    let bobs_group = bobs_groups.first().unwrap();

    steps.push(LifecycleStep {
        name: "bob_accepts_welcome".into(),
        description: "Bob accepts welcome and joins group".into(),
        event_kind: None,
        event_content: None,
        event_tags: None,
        group_name: Some(bobs_group.name.clone()),
        nostr_group_id: Some(hex::encode(&bobs_group.nostr_group_id)),
        mls_group_id: Some(hex::encode(bobs_group.mls_group_id.as_slice())),
        member_count: Some(bob_mdk.get_members(&bobs_group.mls_group_id)?.len()),
        message_content: None,
        message_kind: None,
        message_pubkey: None,
    });

    // Step 5: Alice sends a message
    let rumor = EventBuilder::new(Kind::Custom(9), "Hello from Alice!")
        .build(alice_keys.public_key());
    let msg_event = alice_mdk.create_message(
        &alice_group.mls_group_id,
        rumor,
        None,
    )?;

    steps.push(LifecycleStep {
        name: "alice_sends_message".into(),
        description: "Alice encrypts and sends kind:445 group message".into(),
        event_kind: Some(msg_event.kind.as_u16()),
        event_content: Some(msg_event.content.to_string()),
        event_tags: Some(tags_to_vecs(msg_event.tags.as_slice())),
        group_name: None,
        nostr_group_id: None,
        mls_group_id: None,
        member_count: None,
        message_content: Some("Hello from Alice!".into()),
        message_kind: Some(9),
        message_pubkey: Some(alice_keys.public_key().to_string()),
    });

    // Step 6: Bob decrypts the message
    bob_mdk.process_message(&msg_event)?;
    let messages = bob_mdk.get_messages(&bobs_group.mls_group_id, None)
        .map_err(|e| format!("get_messages: {}", e))?;
    let message = messages.first().unwrap();

    steps.push(LifecycleStep {
        name: "bob_decrypts_message".into(),
        description: "Bob decrypts the message and verifies content".into(),
        event_kind: None,
        event_content: None,
        event_tags: None,
        group_name: None,
        nostr_group_id: None,
        mls_group_id: None,
        member_count: None,
        message_content: Some(message.content.clone()),
        message_kind: Some(message.kind.as_u16()),
        message_pubkey: Some(message.pubkey.to_string()),
    });

    // ════════════════════════════════════════════════════════════════
    // Assemble and output
    // ════════════════════════════════════════════════════════════════

    let vectors = TestVectors {
        description: "Marmot protocol test vectors generated from MDK reference implementation".into(),
        mdk_version: "0.7.1".into(),
        ciphersuite: 1,
        key_package: KeyPackageVectors {
            description: "MIP-00 KeyPackage creation and serialization vectors".into(),
            cases: kp_cases,
        },
        group_lifecycle: GroupLifecycleVector {
            description: "Full group lifecycle: create → welcome → accept → message → decrypt".into(),
            alice_pubkey: alice_keys.public_key().to_string(),
            bob_pubkey: bob_keys.public_key().to_string(),
            relay_url: "wss://relay.example.com".into(),
            steps,
        },
    };

    let json = serde_json::to_string_pretty(&vectors)?;
    println!("{}", json);

    Ok(())
}
