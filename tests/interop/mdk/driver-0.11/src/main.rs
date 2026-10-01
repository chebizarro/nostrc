//! MDK 0.11 interop driver: one or more MDK v0.11.0 peers (the adopted Marmot
//! profile: cgka-engine, app_data_dictionary components, 0x8009 account
//! proof) behind the JSON-lines protocol of ../driver (MDK 0.8), talking to
//! real Nostr relays. nostrc's C interop test
//! (gnome/groundhog/tests/mls/test_mdk011_interop.c) starts it, usually as a
//! Docker container, and drives it on stdin/stdout; the relays are the test's
//! local relays.
//!
//! Each peer is the account-device stack White Noise 0.11 runs, with
//! marmot-app's session configuration: cgka-session's AccountDeviceSession
//! (cgka-engine on SQLCipher storage, ProtocolProfile::Current, the pinned v1
//! convergence policy, and marmot-app's own feature registry and supported
//! component set, copied verbatim and checked against the pinned source by the
//! `parity` tests), the transport-nostr-peeler (kind 445 sealing, NIP-59
//! Welcomes) and the transport-nostr-adapter's kind 30443 KeyPackage
//! publication, with White Noise Android's `client` tag. KeyPackages come from
//! `fresh_key_package()`, which builds them with the same engine function
//! (`build_fresh_key_package`) as marmot-app's lifecycle staging. The relay
//! I/O is this driver's own (as in the 0.8 driver), not marmot-app's relay
//! plane: one WebSocket per operation, every wait bounded.
//!
//! Protocol: one JSON object per line on stdin, `{"id": N, "cmd": "...", ...}`;
//! exactly one answer line per request on stdout, `{"id": N, "ok": true, ...}`
//! or `{"id": N, "ok": false, "class": "...", "error": "..."}`, in order. The
//! failure class is one of `unsupported` (a profile, framing or capability
//! this peer does not speak), `crypto/auth failure` (a signature, proof,
//! credential or decryption check failed), `transport` (relays), `state`
//! (the group's or the request's state forbids it) or `internal`. Logs go to
//! stderr. Every relay wait is bounded (MDK_DRIVER_DEADLINE_S, default 30 s)
//! and turns a hang into an error. The only timed wait is the engine's own
//! settlement window (the adopted convergence policy's 1 s quiescence), which
//! the engine reports and the driver waits out before advancing convergence.
//!
//! Vectors: with MDK_DRIVER_ARTIFACT_DIR set, the wire objects the peers make
//! or see (signed KeyPackage events, unwrapped Welcome rumors, kind 445
//! events) and the member-visible GroupContext (component bytes, key-bearing
//! components redacted to length and digest) are appended to `vectors.jsonl`
//! there. Secrets (account keys, the SQLCipher key, MLS or exporter secrets,
//! group image keys) are never written: no command returns them, and every
//! artifact line is checked against the peers' secret keys before it is
//! written. Requests are never logged (peer_new carries a secret).
//!
//! Relays: a relay that asks for NIP-42 AUTH gets it, as the peer's account
//! for its inbox (gift wraps are served only to their recipient), as a fresh
//! key everywhere else. MDK_DRIVER_DIAL_HOST, when set, is the host dialled
//! instead of 127.0.0.1/localhost (Docker Desktop: host.docker.internal); the
//! URL itself, and so the AUTH `relay` tag, is unchanged.

use std::collections::{BTreeMap, HashMap, HashSet};
use std::io::Write as _;
use std::sync::Arc;
use std::time::Duration;

use base64::Engine as _;
use cgka_engine::account_identity_proof::{AccountIdentityProofRequest, AccountIdentityProofSigner};
use cgka_engine::feature_registry::FeatureRegistry;
use cgka_engine::key_package::{KeyPackageMetadata, key_package_metadata};
use cgka_session::{AccountDeviceSession, PublishWork, SessionConfig, SessionEffects};
use cgka_traits::agent_text_stream::{
    AGENT_TEXT_STREAM_QUIC_FANOUT_CAPABILITY, AGENT_TEXT_STREAM_QUIC_FANOUT_FEATURE,
    AGENT_TEXT_STREAM_QUIC_RECEIVE_CAPABILITY, AGENT_TEXT_STREAM_QUIC_RECEIVE_FEATURE,
    AGENT_TEXT_STREAM_QUIC_SEND_CAPABILITY, AGENT_TEXT_STREAM_QUIC_SEND_FEATURE,
    AgentTextStreamQuicPolicyV1,
};
use cgka_traits::app_components::{
    AGENT_TEXT_STREAM_QUIC_COMPONENT_ID, APP_COMPONENTS_COMPONENT_ID, AppComponentData,
    GROUP_ADMIN_POLICY_COMPONENT_ID, GROUP_AVATAR_URL_COMPONENT_ID,
    GROUP_BLOSSOM_IMAGE_COMPONENT_ID, GROUP_ENCRYPTED_MEDIA_V1_COMPONENT_ID,
    GROUP_ENCRYPTED_MEDIA_V2_COMPONENT_ID, GROUP_LIFECYCLE_COMPONENT_ID,
    GROUP_MESSAGE_RETENTION_COMPONENT_ID, GROUP_PROFILE_COMPONENT_ID, NOSTR_ROUTING_COMPONENT_ID,
    EncryptedMediaPolicyV2, NostrRoutingV1, PRIVATE_USE_APP_COMPONENT_ID_START,
    SAFE_AAD_COMPONENT_ID, decode_nostr_routing_v1, default_group_components,
    encode_encrypted_media_policy_v2, encode_nostr_routing_v1,
};
use cgka_traits::app_event::{MARMOT_APP_EVENT_KIND_CHAT, MarmotAppEvent};
use cgka_traits::app_components::GROUP_ENCRYPTED_MEDIA_EXPORTER_CACHE_KEY;
use chacha20poly1305::aead::{AeadInPlace, KeyInit};
use chacha20poly1305::{ChaCha20Poly1305, Nonce};
use cgka_traits::capabilities::{Capability, CapabilityRequirement, Feature, RequirementLevel};
use cgka_traits::engine::{CreateGroupRequest, GroupEvent, KeyPackage, SendIntent};
use cgka_traits::group::ProtocolProfile;
use cgka_traits::ingest::IngestOutcome;
use cgka_traits::{GroupId, MemberId, MessageId, PendingStateRef, TransportEndpoint, TransportMessage};
use futures_util::{SinkExt, StreamExt};
use nostr::prelude::*;
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use storage_sqlite::SqlCipherKey;
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio::net::TcpStream;
use tokio_tungstenite::tungstenite::Message as WsMessage;
use tokio_tungstenite::{MaybeTlsStream, WebSocketStream};
use transport_nostr_adapter::NostrKeyPackagePublication;
use transport_nostr_peeler::NostrTransportEvent;

const MDK_VERSION: &str = "0.11.0";
const MDK_REV: &str = "946e0547485c9a2c393c2048ec3a968fd50fb441";
const OPENMLS_REV: &str = "59e7d3b27a7e95237879dd5478de1fd90eff7ada";
const NOSTR_REV: &str = "a9c7a6423d104c603de6ea8244265ea17f0f9d89";
/// The wire profile this peer speaks (see the README's profile names).
const PROFILE: &str = "marmot-adopted";
const KIND_KEY_PACKAGE: u16 = 30443;
const KIND_GIFT_WRAP: u16 = 1059;
const KIND_GROUP_MESSAGE: u16 = 445;

// ---- failures ---------------------------------------------------------------------

/// A failed request: its class (see the module doc) and message.
#[derive(Debug)]
struct Failure {
    class: &'static str,
    error: String,
}

type Res<T> = Result<T, Failure>;

const UNSUPPORTED: &str = "unsupported";
const CRYPTO: &str = "crypto/auth failure";
const TRANSPORT: &str = "transport";
const STATE: &str = "state";
const INTERNAL: &str = "internal";

fn fail(class: &'static str, error: impl Into<String>) -> Failure {
    Failure { class, error: error.into() }
}

/// Maps a displayable error into a failure of a fixed class.
fn as_fail<'a, E: std::fmt::Display>(class: &'static str, what: &'a str) -> impl FnOnce(E) -> Failure + 'a {
    move |e| fail(class, format!("{what}: {e}"))
}

/// The class of an MDK engine/session error, by its variant (Debug form);
/// the message is the error's own text.
fn engine_fail<E: std::fmt::Debug + std::fmt::Display>(what: &str) -> impl FnOnce(E) -> Failure + '_ {
    move |e| {
        let debug = format!("{e:?}");
        fail(classify(&debug), format!("{what}: {e} [{}]", variant_name(&debug)))
    }
}

fn variant_name(debug: &str) -> String {
    debug.chars().take_while(|c| c.is_alphanumeric() || *c == '_').collect()
}

/// MDK 0.11's account-proof errors that name a profile mismatch, not a
/// failed check (cgka-engine/src/account_identity_proof.rs).
const PROFILE_PROOF_ERRORS: &[&str] = &[
    "unsupported proof version",
    "neither legacy proof extension",
    "mixes legacy proof",
    "requires neither legacy proof",
    "cannot classify proof profile",
    "is not advertised in leaf app_components",
    " proof in a ",
];

fn classify(debug: &str) -> &'static str {
    // Session errors wrap the engine's: Engine(InvalidWelcome) etc.
    let inner = debug
        .strip_prefix("Engine(")
        .or_else(|| debug.strip_prefix("Storage("))
        .unwrap_or(debug);
    let name = variant_name(inner);
    let lower = debug.to_ascii_lowercase();
    match name.as_str() {
        "MissingRequiredCapabilities"
        | "InvalidKeyPackageCapabilities"
        | "UnsupportedCiphersuite"
        | "DisbandingUnsupportedMembers"
        | "DisbandingNotEnabled" => UNSUPPORTED,
        // A proof of another profile (v1 0xF2F1, none, both, or one the group
        // does not use) is unsupported; a proof that fails its checks
        // (signature, identity, key, length) is a crypto/auth failure.
        "InvalidAccountIdentityProof"
            if PROFILE_PROOF_ERRORS.iter().any(|m| debug.contains(m)) => UNSUPPORTED,
        "InvalidAccountIdentityProof" | "InvalidCredentialIdentity" | "InvalidWelcome" => CRYPTO,
        "NotAMember" | "NotGroupAdmin" | "UnknownGroup" | "UnknownMember" | "UnknownPending"
        | "AdminCannotSelfRemove" | "AdminDepletion" | "LeaveAlreadyRequested"
        | "GroupNotHydrated" | "AppMessageEpochUnsettled" | "AppMessageEpochMismatch"
        | "ForkedEpoch" | "QueuedOutboundAtCapacity" | "InvalidTransition" => STATE,
        _ if lower.contains("signature") || lower.contains("decrypt") || lower.contains("proof")
            || lower.contains("credential") || lower.contains("aead") => CRYPTO,
        _ if lower.contains("unsupported") || lower.contains("profile")
            || lower.contains("capabilit") || lower.contains("legacy") => UNSUPPORTED,
        _ => INTERNAL,
    }
}

fn deadline() -> Duration {
    let secs = std::env::var("MDK_DRIVER_DEADLINE_S")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(30);
    Duration::from_secs(secs)
}

// ---- relay client (as in ../driver) --------------------------------------------

/* ---- MIP-04 encrypted-media-v2 (marmot-app 0.11 media/, parity tested) ---------- */

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum EncryptedMediaVersion {
    V2,
}

impl EncryptedMediaVersion {
    fn as_str(self) -> &'static str {
        match self {
            Self::V2 => "encrypted-media-v2",
        }
    }
}

struct MediaLocator {
    kind: String,
    value: String,
}

/// The fields of marmot-app's `MediaAttachmentReference` its `imeta_tag`
/// writes (the source epoch is the message's, not a tag).
struct MediaAttachmentReference {
    locators: Vec<MediaLocator>,
    ciphertext_sha256: String,
    plaintext_sha256: String,
    nonce_hex: String,
    file_name: String,
    media_type: String,
    version: String,
    dim: Option<String>,
    thumbhash: Option<String>,
}

impl MediaAttachmentReference {
    pub(crate) fn imeta_tag(&self) -> Vec<String> {
        let mut tag = vec!["imeta".to_owned(), format!("v {}", self.version)];
        tag.extend(
            self.locators
                .iter()
                .map(|locator| format!("locator {} {}", locator.kind, locator.value)),
        );
        tag.extend([
            format!("ciphertext_sha256 {}", self.ciphertext_sha256),
            format!("plaintext_sha256 {}", self.plaintext_sha256),
            format!("nonce {}", self.nonce_hex),
            format!("m {}", self.media_type),
            format!("filename {}", self.file_name),
        ]);
        if let Some(dim) = self.dim.as_deref() {
            tag.push(format!("dim {}", dim));
        }
        if let Some(thumbhash) = self.thumbhash.as_deref() {
            tag.push(format!("thumbhash {}", thumbhash));
        }
        tag
    }
}

fn media_key_info(
    version: EncryptedMediaVersion,
    file_hash: &[u8; 32],
    media_type: &str,
    file_name: &str,
) -> Vec<u8> {
    let version = version.as_str();
    let mut info =
        Vec::with_capacity(version.len() + 1 + 32 + 1 + media_type.len() + 1 + file_name.len() + 4);
    info.extend_from_slice(version.as_bytes());
    info.push(0);
    info.extend_from_slice(file_hash);
    info.push(0);
    info.extend_from_slice(media_type.as_bytes());
    info.push(0);
    info.extend_from_slice(file_name.as_bytes());
    info.push(0);
    info.extend_from_slice(b"key");
    info
}

fn media_aad(
    version: EncryptedMediaVersion,
    file_hash: &[u8; 32],
    media_type: &str,
    file_name: &str,
) -> Vec<u8> {
    let version = version.as_str();
    let mut aad =
        Vec::with_capacity(version.len() + 1 + 32 + 1 + media_type.len() + 1 + file_name.len());
    aad.extend_from_slice(version.as_bytes());
    aad.push(0);
    aad.extend_from_slice(file_hash);
    aad.push(0);
    aad.extend_from_slice(media_type.as_bytes());
    aad.push(0);
    aad.extend_from_slice(file_name.as_bytes());
    aad
}

/// A minimal HTTP/1.1 exchange with a loopback `http://` server (the test's
/// Blossom fixture), dialled like the relays (MDK_DRIVER_DIAL_HOST): the
/// status and body; `Connection: close`, bounded by the deadline.
async fn http_exchange(url: &str, method: &str, headers: &[(&str, String)], body: &[u8]) -> Res<(u16, Vec<u8>)> {
    let dial = dial_url(url);
    let rest = dial.strip_prefix("http://").ok_or_else(|| fail(UNSUPPORTED, format!("{url}: only http:// test servers")))?;
    let (authority, path) = match rest.find('/') {
        Some(i) => (&rest[..i], &rest[i..]),
        None => (rest, "/"),
    };
    let host_header = url.strip_prefix("http://").map(|r| r.split('/').next().unwrap_or(r)).unwrap_or(authority);
    let exchange = async {
        let mut stream = TcpStream::connect(authority).await.map_err(as_fail(TRANSPORT, url))?;
        let mut head = format!("{method} {path} HTTP/1.1\r\nHost: {host_header}\r\nConnection: close\r\nContent-Length: {}\r\n", body.len());
        for (k, v) in headers {
            head.push_str(&format!("{k}: {v}\r\n"));
        }
        head.push_str("\r\n");
        stream.write_all(head.as_bytes()).await.map_err(as_fail(TRANSPORT, url))?;
        stream.write_all(body).await.map_err(as_fail(TRANSPORT, url))?;
        let mut answer = Vec::new();
        tokio::io::AsyncReadExt::read_to_end(&mut stream, &mut answer).await.map_err(as_fail(TRANSPORT, url))?;
        let split = answer.windows(4).position(|w| w == b"\r\n\r\n")
            .ok_or_else(|| fail(TRANSPORT, format!("{url}: no HTTP header")))?;
        let header = String::from_utf8_lossy(&answer[..split]).to_string();
        let status: u16 = header.split_whitespace().nth(1).and_then(|s| s.parse().ok())
            .ok_or_else(|| fail(TRANSPORT, format!("{url}: no HTTP status")))?;
        let mut payload = answer[split + 4..].to_vec();
        if header.to_ascii_lowercase().contains("transfer-encoding: chunked") {
            payload = dechunk(&payload).ok_or_else(|| fail(TRANSPORT, format!("{url}: bad chunked body")))?;
        }
        Ok::<_, Failure>((status, payload))
    };
    tokio::time::timeout(deadline(), exchange).await.map_err(|_| fail(TRANSPORT, format!("{url}: timed out")))?
}

fn dechunk(mut data: &[u8]) -> Option<Vec<u8>> {
    let mut out = Vec::new();
    loop {
        let line_end = data.windows(2).position(|w| w == b"\r\n")?;
        let size = usize::from_str_radix(std::str::from_utf8(&data[..line_end]).ok()?.split(';').next()?.trim(), 16).ok()?;
        data = &data[line_end + 2..];
        if size == 0 {
            return Some(out);
        }
        out.extend_from_slice(data.get(..size)?);
        data = data.get(size + 2..)?;
    }
}

async fn http_get(url: &str) -> Res<Vec<u8>> {
    let (status, body) = http_exchange(url, "GET", &[], &[]).await?;
    if status != 200 {
        return Err(fail(TRANSPORT, format!("{url}: HTTP {status}")));
    }
    Ok(body)
}

/// BUD-02 PUT /upload with the kind-24242 authorization marmot-app's Blossom
/// client signs (t upload, x, expiration), signed by the account's keys.
async fn blossom_upload(server: &str, blob: &[u8], sha256: &str, keys: &Keys) -> Res<String> {
    let expiration = Timestamp::now().as_secs() + 300;
    let auth = EventBuilder::new(Kind::Custom(24242), "Upload blob")
        .tags([
            Tag::parse(["t", "upload"]).map_err(as_fail(INTERNAL, "tag"))?,
            Tag::parse(["x", sha256]).map_err(as_fail(INTERNAL, "tag"))?,
            Tag::parse(["expiration", &expiration.to_string()]).map_err(as_fail(INTERNAL, "tag"))?,
        ])
        .finalize(keys)
        .map_err(as_fail(INTERNAL, "blossom auth"))?;
    let header = format!("Nostr {}", base64::engine::general_purpose::STANDARD.encode(auth.as_json()));
    let url = format!("{server}/upload");
    let (status, body) = http_exchange(&url, "PUT", &[
        ("Authorization", header),
        ("Content-Type", "application/octet-stream".to_string()),
    ], blob).await?;
    if status != 200 && status != 201 {
        return Err(fail(TRANSPORT, format!("{url}: HTTP {status}: {}", String::from_utf8_lossy(&body))));
    }
    let descriptor: Value = serde_json::from_slice(&body).map_err(as_fail(TRANSPORT, "blossom descriptor"))?;
    if descriptor["sha256"].as_str() != Some(sha256) {
        return Err(fail(TRANSPORT, "the Blossom descriptor names another blob"));
    }
    Ok(format!("{server}/{sha256}"))
}

struct Relay {
    url: String,
    ws: WebSocketStream<MaybeTlsStream<TcpStream>>,
    challenge: Option<String>,
    authed: bool,
    queued: Vec<Value>,
}

fn dial_url(url: &str) -> String {
    match std::env::var("MDK_DRIVER_DIAL_HOST") {
        Ok(host) if !host.is_empty() => url
            .replacen("://127.0.0.1:", &format!("://{host}:"), 1)
            .replacen("://localhost:", &format!("://{host}:"), 1),
        _ => url.to_string(),
    }
}

impl Relay {
    async fn open(url: &str) -> Res<Self> {
        let dial = dial_url(url);
        let (ws, _) = tokio::time::timeout(deadline(), tokio_tungstenite::connect_async(&dial))
            .await
            .map_err(|_| fail(TRANSPORT, format!("{url}: connect timed out")))?
            .map_err(as_fail(TRANSPORT, url))?;
        Ok(Self { url: url.to_string(), ws, challenge: None, authed: false, queued: Vec::new() })
    }

    async fn send(&mut self, frame: Value) -> Res<()> {
        self.ws
            .send(WsMessage::Text(frame.to_string().into()))
            .await
            .map_err(as_fail(TRANSPORT, &self.url))
    }

    /// The next relay frame other than AUTH, which only records the challenge.
    async fn next(&mut self) -> Res<Value> {
        if !self.queued.is_empty() {
            return Ok(self.queued.remove(0));
        }
        loop {
            let msg = tokio::time::timeout(deadline(), self.ws.next())
                .await
                .map_err(|_| fail(TRANSPORT, format!("{}: no answer before the deadline", self.url)))?
                .ok_or_else(|| fail(TRANSPORT, format!("{}: connection closed", self.url)))?
                .map_err(as_fail(TRANSPORT, &self.url))?;
            let WsMessage::Text(text) = msg else { continue };
            let frame: Value =
                serde_json::from_str(text.as_str()).map_err(as_fail(TRANSPORT, &self.url))?;
            if frame.get(0).and_then(Value::as_str) == Some("AUTH") {
                self.challenge = frame.get(1).and_then(Value::as_str).map(str::to_string);
                continue;
            }
            return Ok(frame);
        }
    }

    async fn authenticate(&mut self, keys: &Keys) -> Res<()> {
        while self.challenge.is_none() {
            let frame = self.next().await?;
            self.queued.push(frame);
        }
        let challenge = self.challenge.clone().unwrap_or_default();
        // NIP-42 (the pinned nostr fork has no builder for it): kind 22242
        // with the relay URL as the test dialled it, and the challenge.
        let tags = [
            Tag::parse(["relay", self.url.as_str()]).map_err(as_fail(INTERNAL, "auth"))?,
            Tag::parse(["challenge", challenge.as_str()]).map_err(as_fail(INTERNAL, "auth"))?,
        ];
        let auth = EventBuilder::new(Kind::Custom(22242), "")
            .tags(tags)
            .finalize(keys)
            .map_err(as_fail(INTERNAL, "auth"))?;
        let id = auth.id.to_hex();
        self.send(json!(["AUTH", serde_json::to_value(&auth).map_err(as_fail(INTERNAL, "auth"))?]))
            .await?;
        let (ok, message) = self.wait_ok(&id).await?;
        if !ok {
            return Err(fail(TRANSPORT, format!("{}: AUTH refused: {message}", self.url)));
        }
        self.authed = true;
        Ok(())
    }

    async fn wait_ok(&mut self, id: &str) -> Res<(bool, String)> {
        loop {
            let frame = self.next().await?;
            if frame.get(0).and_then(Value::as_str) == Some("OK")
                && frame.get(1).and_then(Value::as_str) == Some(id)
            {
                let ok = frame.get(2).and_then(Value::as_bool).unwrap_or(false);
                let message = frame.get(3).and_then(Value::as_str).unwrap_or("").to_string();
                return Ok((ok, message));
            }
        }
    }

    async fn publish(&mut self, event: &Event, auth: &Keys) -> Res<(bool, String)> {
        let id = event.id.to_hex();
        let frame = json!(["EVENT", serde_json::to_value(event).map_err(as_fail(INTERNAL, "event"))?]);
        self.send(frame.clone()).await?;
        let (ok, message) = self.wait_ok(&id).await?;
        if ok || !message.starts_with("auth-required") || self.authed {
            return Ok((ok, message));
        }
        self.authenticate(auth).await?;
        self.send(frame).await?;
        self.wait_ok(&id).await
    }

    /// Every stored event matching filter, up to EOSE; the REQ is then closed.
    async fn fetch(&mut self, filter: &Value, auth: &Keys) -> Res<Vec<Event>> {
        let mut attempt = 0;
        loop {
            attempt += 1;
            let sub = format!("mdk-{}", hex::encode(&Keys::generate().secret_key().to_secret_bytes()[..6]));
            self.send(json!(["REQ", sub, filter])).await?;
            let mut events = Vec::new();
            loop {
                let frame = self.next().await?;
                let kind = frame.get(0).and_then(Value::as_str).unwrap_or("");
                if frame.get(1).and_then(Value::as_str) != Some(sub.as_str()) {
                    continue;
                }
                match kind {
                    "EVENT" => {
                        let raw = frame.get(2).cloned().unwrap_or(Value::Null);
                        let event: Event =
                            serde_json::from_value(raw).map_err(as_fail(TRANSPORT, "event"))?;
                        if event.verify().is_ok() {
                            events.push(event);
                        }
                    }
                    "EOSE" => {
                        self.send(json!(["CLOSE", sub])).await?;
                        return Ok(events);
                    }
                    "CLOSED" => {
                        let message = frame.get(2).and_then(Value::as_str).unwrap_or("");
                        if message.starts_with("auth-required") && !self.authed && attempt == 1 {
                            self.authenticate(auth).await?;
                            break;
                        }
                        return Err(fail(TRANSPORT, format!("{}: REQ closed: {message}", self.url)));
                    }
                    _ => {}
                }
            }
        }
    }
}

/// Publishes to every relay; the per-relay answers, and whether one accepted.
async fn publish_all(relays: &[String], event: &Event, auth: &Keys) -> (bool, Value) {
    let mut accepted = false;
    let mut answers = serde_json::Map::new();
    for url in relays {
        let answer = match Relay::open(url).await {
            Ok(mut relay) => relay.publish(event, auth).await,
            Err(e) => Err(e),
        };
        let value = match answer {
            Ok((ok, message)) => {
                accepted |= ok;
                json!({ "ok": ok, "message": message })
            }
            Err(e) => json!({ "ok": false, "message": e.error }),
        };
        answers.insert(url.clone(), value);
    }
    (accepted, Value::Object(answers))
}

async fn fetch_all(relays: &[String], filter: &Value, auth: &Keys) -> Res<Vec<Event>> {
    let mut seen = HashSet::new();
    let mut out = Vec::new();
    for url in relays {
        let mut relay = Relay::open(url).await?;
        for event in relay.fetch(filter, auth).await? {
            if seen.insert(event.id) {
                out.push(event);
            }
        }
    }
    Ok(out)
}

// ---- vectors -----------------------------------------------------------------------

/// The artifact sink: vectors.jsonl in MDK_DRIVER_ARTIFACT_DIR, if set.
struct Vectors {
    file: Option<std::fs::File>,
    /// Hex of every peer's secret key: no artifact line may contain one.
    secrets: Vec<String>,
    /// This driver process's id, on every line (several processes, one per
    /// case, append to one file in a run; CTest empties it first).
    run: String,
}

impl Vectors {
    fn open() -> Self {
        let file = std::env::var("MDK_DRIVER_ARTIFACT_DIR").ok().filter(|d| !d.is_empty()).and_then(|dir| {
            let _ = std::fs::create_dir_all(&dir);
            let path = std::path::Path::new(&dir).join("vectors.jsonl");
            match std::fs::OpenOptions::new().create(true).append(true).open(&path) {
                Ok(f) => Some(f),
                Err(e) => {
                    eprintln!("mdk011-driver: no vectors file {}: {e}", path.display());
                    None
                }
            }
        });
        let run = hex::encode(&Keys::generate().public_key().to_bytes()[..8]);
        Self { file, secrets: Vec::new(), run }
    }

    fn record(&mut self, kind: &str, peer: &str, mut value: Value) {
        let Some(file) = self.file.as_mut() else { return };
        value["vector"] = json!(kind);
        value["peer"] = json!(peer);
        value["mdk_rev"] = json!(MDK_REV);
        value["driver_run"] = json!(self.run);
        let line = value.to_string();
        if self.secrets.iter().any(|s| line.contains(s.as_str())) {
            // Redaction guard: a secret must never reach the artifact.
            eprintln!("mdk011-driver: a {kind} vector would carry a secret; dropped");
            return;
        }
        let _ = writeln!(file, "{line}");
    }
}

// ---- peers ------------------------------------------------------------------------

struct ProofSigner {
    keys: Keys,
}

impl AccountIdentityProofSigner for ProofSigner {
    fn sign_account_identity_proof(
        &self,
        request: &AccountIdentityProofRequest,
    ) -> Result<[u8; 64], String> {
        if self.keys.public_key().to_bytes().as_slice() != request.account_identity.as_slice() {
            return Err("proof request for another account".into());
        }
        let event = request
            .proof_event()
            .and_then(|event| event.finalize(&self.keys).map_err(|err| err.to_string()))?;
        request.signature_from_signed_event(event)
    }
}

// White Noise 0.11's session configuration. The two functions below are
// verbatim copies of marmot-app's at MDK_REV (crates/marmot-app/src/lib.rs:
// `app_feature_registry` and `MarmotApp::supported_app_component_ids`), and
// the `parity` tests (run by `cargo test`, and so by the image build) fail if
// their bodies differ from the pinned source or if marmot-app configures its
// session with anything else. They decide what a leaf and a KeyPackage
// advertise on the wire: SelfRemove (proposal 0x000a), the three
// agent-text-stream-QUIC roles as Optional private-use extensions
// (0xF2D1/0xF2D2/0xF2D4; advertising them needs no QUIC transport) and the
// component set including 0x8006. The bodies keep marmot-app's comments.

fn app_feature_registry() -> FeatureRegistry {
    let mut registry = FeatureRegistry::new();
    registry.register(
        Feature("self-remove"),
        CapabilityRequirement {
            requires: Capability::Proposal(10),
            level: RequirementLevel::Required,
            description: "MIP-03 SelfRemove group departure",
        },
    );
    // Each agent-text-stream-QUIC role maps to its own distinct backing
    // capability (a private-use MLS extension type), so a member advertises
    // `receive`/`send`/`fanout` independently and a group's
    // `required_member_roles` mask is enforceable per role (#177,
    // agent-text-stream-quic-v1.md). The capability/feature/bit mapping is the
    // shared `AGENT_TEXT_STREAM_QUIC_ROLES` table so the engine enforcement and
    // this registration cannot drift.
    for (feature, capability, description) in [
        (
            AGENT_TEXT_STREAM_QUIC_RECEIVE_FEATURE.clone(),
            AGENT_TEXT_STREAM_QUIC_RECEIVE_CAPABILITY,
            "receive QUIC-backed agent text stream previews",
        ),
        (
            AGENT_TEXT_STREAM_QUIC_SEND_FEATURE.clone(),
            AGENT_TEXT_STREAM_QUIC_SEND_CAPABILITY,
            "send QUIC-backed agent text stream frames",
        ),
        (
            AGENT_TEXT_STREAM_QUIC_FANOUT_FEATURE.clone(),
            AGENT_TEXT_STREAM_QUIC_FANOUT_CAPABILITY,
            "fan out QUIC-backed agent text stream frames",
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

/// cgka-engine's default supported components (profile 0x8001, admin policy
/// 0x8003, lifecycle 0x800c) plus Nostr routing 0x8004, which every
/// Nostr-routed group requires: an MDK 0.11 engine as configured by default.
fn engine_default_component_ids() -> Vec<u16> {
    vec![0x8001, 0x8003, 0x8004, 0x800c]
}

fn supported_app_component_ids() -> Vec<u16> {
    let mut components = default_group_components();
    components.insert(GROUP_BLOSSOM_IMAGE_COMPONENT_ID);
    components.insert(NOSTR_ROUTING_COMPONENT_ID);
    components.insert(GROUP_MESSAGE_RETENTION_COMPONENT_ID);
    components.insert(AGENT_TEXT_STREAM_QUIC_COMPONENT_ID);
    components.insert(GROUP_AVATAR_URL_COMPONENT_ID);
    // Existing legacy groups continue to require V1, while fresh
    // current-profile groups require V2. Advertising both is support, not
    // negotiation: each group's required component id selects exactly one.
    components.insert(GROUP_ENCRYPTED_MEDIA_V1_COMPONENT_ID);
    components.insert(GROUP_ENCRYPTED_MEDIA_V2_COMPONENT_ID);
    components.into_iter().collect()
}

/// The KeyPackage `client` tag White Noise Android 0.11 publishes
/// (whitenoise-android 6186a253, MarmotClient.kt: `CLIENT_NAME`, passed to
/// marmot-app's `key_package_client_name`); a peer may override it (null:
/// none, as for a generic MDK consumer).
const WHITE_NOISE_CLIENT_NAME: &str = "White Noise Android";

struct Peer {
    name: String,
    keys: Keys,
    session: AccountDeviceSession,
    /// The KeyPackage slot (`d` tag), stable across replacements.
    slot: String,
    /// The KeyPackage `client` tag, if any.
    client: Option<String>,
    /// Kind 445 event ids this peer published or applied.
    seen: HashSet<EventId>,
    /// Gift wraps already opened: their outcome, by wrapper id.
    wraps: HashMap<String, Value>,
    /// Groups joined from a Welcome, by wrapper id.
    joined: HashMap<String, GroupId>,
}

struct Driver {
    dir: tempfile::TempDir,
    peers: HashMap<String, Peer>,
    vectors: Vectors,
}

fn str_arg<'a>(req: &'a Value, key: &str) -> Res<&'a str> {
    req.get(key).and_then(Value::as_str).ok_or_else(|| fail(INTERNAL, format!("missing string '{key}'")))
}

fn strs_arg(req: &Value, key: &str) -> Res<Vec<String>> {
    Ok(req
        .get(key)
        .and_then(Value::as_array)
        .ok_or_else(|| fail(INTERNAL, format!("missing array '{key}'")))?
        .iter()
        .filter_map(|v| v.as_str().map(str::to_string))
        .collect())
}

fn member_id(hex_key: &str) -> Res<MemberId> {
    let key = PublicKey::from_hex(hex_key).map_err(as_fail(INTERNAL, hex_key))?;
    Ok(MemberId::new(key.to_bytes().to_vec()))
}

fn group_id_arg(req: &Value) -> Res<GroupId> {
    let bytes = hex::decode(str_arg(req, "group")?).map_err(as_fail(INTERNAL, "group"))?;
    Ok(GroupId::new(bytes))
}

fn event_arg(value: &Value) -> Res<Event> {
    let event: Event = match value {
        Value::String(s) => Event::from_json(s).map_err(as_fail(INTERNAL, "event json"))?,
        other => serde_json::from_value(other.clone()).map_err(as_fail(INTERNAL, "event"))?,
    };
    event.verify().map_err(as_fail(CRYPTO, "event signature"))?;
    Ok(event)
}

fn hex_ids(ids: impl IntoIterator<Item = u16>) -> Vec<String> {
    ids.into_iter().map(|id| format!("0x{id:04x}")).collect()
}

fn tag_value<'a>(event: &'a Event, name: &str) -> Vec<&'a [String]> {
    event
        .tags
        .iter()
        .map(|t| t.as_slice())
        .filter(|t| t.first().map(String::as_str) == Some(name))
        .collect()
}

/// A kind 30443 event as MDK 0.11's relay-fetch path admits it
/// (marmot-app key_package_records: strict cutover, current profile only):
/// tag cardinality and values, MLSMessage(KeyPackage) framing, leaf
/// signature, the 0x8009 account proof, credential = author, `i` = KeyPackageRef.
fn admit_key_package(event: &Event) -> Res<(KeyPackage, KeyPackageMetadata)> {
    if event.kind != Kind::Custom(KIND_KEY_PACKAGE) {
        return Err(fail(UNSUPPORTED, format!("kind {} is not a Marmot KeyPackage (30443)", event.kind.as_u16())));
    }
    let single = |name: &str| -> Res<String> {
        let tags = tag_value(event, name);
        match tags.as_slice() {
            [tag] if tag.len() == 2 && !tag[1].is_empty() => Ok(tag[1].clone()),
            [] => Err(fail(UNSUPPORTED, format!("missing '{name}' tag"))),
            _ => Err(fail(UNSUPPORTED, format!("'{name}' tag must appear once with one value"))),
        }
    };
    let version = single("mls_protocol_version")?;
    if version != "1.0" {
        return Err(fail(UNSUPPORTED, format!("mls_protocol_version {version}")));
    }
    single("d")?;
    let reference = single("i")?;
    let bytes = base64::engine::general_purpose::STANDARD
        .decode(event.content.as_bytes())
        .map_err(as_fail(UNSUPPORTED, "content is not base64"))?;
    let event_id = MessageId::new(event.id.to_bytes().to_vec());
    let key_package = KeyPackage::with_source_event_id(bytes, event_id)
        .with_protocol_profile(ProtocolProfile::Current);
    let metadata = key_package_metadata(&key_package).map_err(|e| {
        let debug = format!("{e:?}");
        let mut f = engine_fail("KeyPackage")(e);
        // MDK's own verdict. A KeyPackage that is not MLSMessage(KeyPackage)
        // framed is another wire profile, not a broken one; the legacy
        // MIP-00 shape (bare KeyPackage, `encoding` tag) is named as such.
        if debug.starts_with("Serialize(") {
            f.class = UNSUPPORTED;
            if !tag_value(event, "encoding").is_empty() {
                f.error.push_str(" (a legacy MIP-00 KeyPackage event: `encoding` tag, no MLSMessage framing)");
            }
        }
        f
    })?;
    if metadata.protocol_profile != ProtocolProfile::Current {
        return Err(fail(UNSUPPORTED, format!("{:?} profile KeyPackage (strict cutover admits Current only)", metadata.protocol_profile)));
    }
    let suite = single("mls_ciphersuite")?;
    if suite != format!("0x{:04x}", metadata.ciphersuite) {
        return Err(fail(UNSUPPORTED, "mls_ciphersuite tag does not match the KeyPackage"));
    }
    let multi = |name: &str, expect: Vec<String>| -> Res<()> {
        let tags = tag_value(event, name);
        let [tag] = tags.as_slice() else {
            return Err(fail(UNSUPPORTED, format!("'{name}' tag must appear once")));
        };
        let mut got: Vec<String> = tag[1..].to_vec();
        let mut want = expect;
        got.sort();
        want.sort();
        if got != want {
            return Err(fail(UNSUPPORTED, format!("'{name}' tag {got:?} does not match the KeyPackage {want:?}")));
        }
        Ok(())
    };
    multi("mls_extensions", hex_ids(metadata.mls_extensions.iter().copied()))?;
    multi("mls_proposals", hex_ids(metadata.mls_proposals.iter().copied()))?;
    multi(
        "app_components",
        hex_ids(metadata.app_components.iter().copied().filter(|id| *id >= PRIVATE_USE_APP_COMPONENT_ID_START)),
    )?;
    if metadata.credential_identity_hex != event.pubkey.to_hex() {
        return Err(fail(CRYPTO, "event author does not match the KeyPackage credential"));
    }
    if metadata.key_package_ref_hex != reference {
        return Err(fail(CRYPTO, "'i' tag does not match the KeyPackageRef"));
    }
    let key_package = key_package.with_protocol_profile(metadata.protocol_profile);
    Ok((key_package, metadata))
}

/// The KeyPackage an inviter picks from an author's kind 30443 events, as
/// MDK 0.11's marmot-app does (key_package_records.rs,
/// preferred_fresh_key_package_from_records, at the pinned commit): newest
/// first (created_at, then event id, both descending); the newest event of
/// each `d` slot supersedes its slot even when it is not admitted (never
/// fall back to an older one, whose key may be retired); a slot whose
/// newest event is another profile -- e.g. the MDK 0.8 KeyPackage a
/// dual-format client keeps in its own slot -- or fails admission is passed
/// over; among the admitted, marmot-app's `client` ranking (White Noise
/// first, Amethyst last) picks, newest first within a rank. With nothing
/// admitted, the newest slot's failure. Also the number of slots seen.
fn select_key_package(author: &str, events: Vec<Event>) -> Res<(Event, usize)> {
    let mut events: Vec<Event> = events
        .into_iter()
        .filter(|e| e.kind == Kind::Custom(KIND_KEY_PACKAGE) && e.pubkey.to_hex() == author)
        .collect();
    events.sort_by(|a, b| a.created_at.cmp(&b.created_at).then_with(|| a.id.cmp(&b.id)));
    let mut slots = HashSet::new();
    let mut seen = 0usize;
    let mut newest_error = None;
    let mut selected: Option<(Event, u8)> = None;
    for event in events.into_iter().rev() {
        let slot = tag_value(&event, "d").first().and_then(|t| t.get(1)).filter(|d| !d.is_empty()).cloned();
        if let Some(slot) = slot
            && !slots.insert(slot)
        {
            continue;
        }
        seen += 1;
        let priority = client_priority(&event);
        if let Err(error) = admit_key_package(&event) {
            newest_error.get_or_insert(error);
            continue;
        }
        if selected.as_ref().is_none_or(|(_, best)| priority > *best) {
            let top = priority == 2;
            selected = Some((event, priority));
            if top {
                break;
            }
        }
    }
    match selected {
        Some((event, _)) => Ok((event, seen)),
        None => Err(newest_error.unwrap_or_else(|| fail(STATE, "no KeyPackage found"))),
    }
}

/// marmot-app's key_package_client_priority (pinned commit): one `client`
/// tag naming "whitenoise" ranks first, "amethyst" last, anything else (or
/// no single tag) between.
fn client_priority(event: &Event) -> u8 {
    let clients = tag_value(event, "client");
    match clients.as_slice() {
        [tag] if tag.len() >= 2 => {
            let name = tag[1].trim();
            if name.eq_ignore_ascii_case("whitenoise") {
                2
            } else if name.eq_ignore_ascii_case("amethyst") {
                0
            } else {
                1
            }
        }
        _ => 1,
    }
}

/// The author's NIP-65 write relays: the newest kind 10002 on `discover`,
/// its `r` entries marked "write" or unmarked (never "read"), in order.
/// Never kind 10051 (the adopted transport removed it).
async fn write_relays(discover: &[String], author: &str) -> Res<Vec<String>> {
    let filter = json!({ "kinds": [10002], "authors": [author] });
    let lists = fetch_all(discover, &filter, &Keys::generate()).await?;
    let newest = lists
        .into_iter()
        .filter(|e| e.pubkey.to_hex() == author)
        .max_by(|a, b| a.created_at.cmp(&b.created_at).then_with(|| b.id.cmp(&a.id)))
        .ok_or_else(|| fail(STATE, "no relay list (kind 10002) found"))?;
    let mut out: Vec<String> = Vec::new();
    for tag in tag_value(&newest, "r") {
        let write = match tag.get(2).map(String::as_str) {
            None | Some("") | Some("write") => true,
            _ => false,
        };
        if write && tag.len() >= 2 && !out.contains(&tag[1]) {
            out.push(tag[1].clone());
        }
    }
    if out.is_empty() {
        return Err(fail(STATE, "the relay list names no write relay"));
    }
    Ok(out)
}

fn describe_metadata(metadata: &KeyPackageMetadata) -> Value {
    json!({
        "profile": format!("{:?}", metadata.protocol_profile),
        "key_package_ref": metadata.key_package_ref_hex,
        "ciphersuite": format!("0x{:04x}", metadata.ciphersuite),
        "mls_extensions": hex_ids(metadata.mls_extensions.iter().copied()),
        "mls_proposals": hex_ids(metadata.mls_proposals.iter().copied()),
        "app_components": hex_ids(metadata.app_components.iter().copied()),
        "not_before": metadata.not_before,
        "not_after": metadata.not_after,
    })
}

/// What MDK 0.11 makes of a kind 30443 event: admitted (with the decoded
/// metadata) or refused, with the failure class.
fn describe_key_package(event: &Event) -> Value {
    match admit_key_package(event) {
        Ok((_, metadata)) => {
            let mut view = describe_metadata(&metadata);
            view["parsed"] = json!(true);
            view
        }
        Err(e) => json!({ "parsed": false, "class": e.class, "error": e.error }),
    }
}

/// The component ids probed in a group's app_data_dictionary.
const PROBED_COMPONENTS: std::ops::RangeInclusive<u16> = 0x8001..=0x8010;

/// Components whose bytes hold no key material, so they may be dumped: the
/// component list, safe AAD, profile, admin policy, routing, retention, the
/// agent-text-stream policy, URL avatar, both media policies and lifecycle.
/// Everything else is redacted to its length and SHA-256: 0x8002 (Blossom
/// image: image key, nonce and upload key, which MDK itself prints as
/// <redacted>) and any id not known to be key-free.
const DUMPABLE_COMPONENTS: &[u16] = &[
    APP_COMPONENTS_COMPONENT_ID,
    SAFE_AAD_COMPONENT_ID,
    GROUP_PROFILE_COMPONENT_ID,
    GROUP_ADMIN_POLICY_COMPONENT_ID,
    NOSTR_ROUTING_COMPONENT_ID,
    GROUP_MESSAGE_RETENTION_COMPONENT_ID,
    AGENT_TEXT_STREAM_QUIC_COMPONENT_ID,
    GROUP_AVATAR_URL_COMPONENT_ID,
    GROUP_ENCRYPTED_MEDIA_V1_COMPONENT_ID,
    GROUP_ENCRYPTED_MEDIA_V2_COMPONENT_ID,
    GROUP_LIFECYCLE_COMPONENT_ID,
];

/// A component's bytes as they may leave the driver (answers, vectors).
fn component_value(id: u16, bytes: &[u8]) -> Value {
    if DUMPABLE_COMPONENTS.contains(&id) {
        json!(hex::encode(bytes))
    } else {
        json!({ "redacted": true, "len": bytes.len(), "sha256": sha256_hex(bytes) })
    }
}

/// The member-visible GroupContext: the components present in the group's
/// app_data_dictionary (key-bearing ones redacted, see DUMPABLE_COMPONENTS)
/// and the required capabilities.
fn group_context(peer: &Peer, group_id: &GroupId) -> Res<Value> {
    let ids: Vec<u16> = (0x0001..=0x000f).chain(PROBED_COMPONENTS).collect();
    let values = peer.session.app_components(group_id, &ids).map_err(engine_fail("app_components"))?;
    let mut components = serde_json::Map::new();
    for (id, value) in ids.iter().zip(values) {
        if let Some(bytes) = value {
            components.insert(format!("0x{id:04x}"), component_value(*id, &bytes));
        }
    }
    let record = peer.session.group_record(group_id).map_err(engine_fail("group_record"))?;
    let required = &record.required_capabilities;
    Ok(json!({
        "components": components,
        "required_proposals": hex_ids(required.proposals.iter().copied()),
        "required_extensions": hex_ids(required.extensions.iter().copied()),
        "required_components": hex_ids(required.app_components.ids.iter().copied()),
    }))
}

fn routing(peer: &Peer, group_id: &GroupId) -> Res<NostrRoutingV1> {
    let bytes = peer
        .session
        .app_component(group_id, NOSTR_ROUTING_COMPONENT_ID)
        .map_err(engine_fail("routing component"))?
        .ok_or_else(|| fail(UNSUPPORTED, "the group has no Nostr routing component (0x8004)"))?;
    decode_nostr_routing_v1(&bytes).map_err(|e| fail(UNSUPPORTED, format!("routing component: {e}")))
}

fn group_state(peer: &Peer, group_id: &GroupId) -> Res<Value> {
    let record = peer.session.group_record(group_id).map_err(engine_fail("group_record"))?;
    let mut members: Vec<String> = peer
        .session
        .members(group_id)
        .map_err(engine_fail("members"))?
        .iter()
        .map(|m| hex::encode(m.id.as_slice()))
        .collect();
    members.sort();
    let mut admins: Vec<String> = peer
        .session
        .admin_pubkeys(group_id)
        .map_err(engine_fail("admin_pubkeys"))?
        .iter()
        .map(hex::encode)
        .collect();
    admins.sort();
    let route = routing(peer, group_id).ok();
    let context = group_context(peer, group_id)?;
    Ok(json!({
        "group": hex::encode(group_id.as_slice()),
        "nostr_group_id": route.as_ref().map(|r| hex::encode(r.nostr_group_id)),
        "relays": route.map(|r| r.relays).unwrap_or_default(),
        "name": record.name,
        "description": record.description,
        "epoch": peer.session.epoch(group_id).map_err(engine_fail("epoch"))?.0,
        "profile": format!("{:?}", record.protocol_profile),
        "removed": record.removed,
        "leave_in_progress": peer.session.leave_in_progress(group_id).unwrap_or(false),
        "members": members,
        "admins": admins,
        "components": context["components"].as_object().map(|c| c.keys().cloned().collect::<Vec<_>>()).unwrap_or_default(),
    }))
}

fn transport_event(msg: &TransportMessage) -> Res<Event> {
    NostrTransportEvent::from_transport_message(msg)
        .and_then(|e| e.to_verified_nostr_event())
        .map_err(as_fail(INTERNAL, "transport event"))
}

fn transport_message(event: &Event) -> Res<TransportMessage> {
    NostrTransportEvent::from_nostr_event(event)
        .and_then(|e| e.to_transport_message())
        .map_err(|e| fail(UNSUPPORTED, format!("not a Marmot transport event: {e}")))
}

fn describe_event(event: &GroupEvent) -> Value {
    match event {
        GroupEvent::MessageReceived { message_id, sender, epoch, payload, .. } => {
            match MarmotAppEvent::decode(payload) {
                Ok(app) => json!({ "type": "application", "id": hex::encode(message_id.as_slice()),
                    "author": hex::encode(sender.as_slice()), "inner_pubkey": app.pubkey,
                    "kind": app.kind, "content": app.content, "tags": app.tags,
                    "epoch": epoch.0 }),
                Err(e) => json!({ "type": "application", "author": hex::encode(sender.as_slice()),
                    "undecodable": e.to_string(), "epoch": epoch.0 }),
            }
        }
        GroupEvent::EpochChanged { .. } => json!({ "type": "commit", "debug": format!("{event:?}") }),
        GroupEvent::GroupJoined { group_id, welcomer, .. } => json!({ "type": "joined",
            "group": hex::encode(group_id.as_slice()),
            "welcomer": welcomer.as_ref().map(|w| hex::encode(w.as_slice())) }),
        other => {
            let debug = format!("{other:?}");
            json!({ "type": variant_name(&debug), "debug": debug })
        }
    }
}

impl Driver {
    fn peer(&mut self, req: &Value) -> Res<&mut Peer> {
        let name = str_arg(req, "peer")?;
        self.peers.get_mut(name).ok_or_else(|| fail(INTERNAL, format!("no peer '{name}'")))
    }

    async fn handle(&mut self, req: &Value) -> Res<Value> {
        let cmd = str_arg(req, "cmd")?;
        match cmd {
            "hello" => {
                // Profile negotiation: "profiles" lists what the caller wants,
                // best first; the answer is the one this peer speaks, or an
                // `unsupported` failure. No list: this peer's profile.
                if let Some(wanted) = req.get("profiles").and_then(Value::as_array) {
                    if !wanted.iter().any(|p| p.as_str() == Some(PROFILE)) {
                        return Err(fail(UNSUPPORTED, format!("this peer speaks only {PROFILE}, not {wanted:?}")));
                    }
                }
                Ok(json!({
                    "contract": 2,
                    "mdk": format!("mdk {MDK_VERSION} (cgka-engine, cgka-session)"),
                    "mdk_rev": MDK_REV,
                    "openmls_rev": OPENMLS_REV,
                    "nostr_rev": NOSTR_REV,
                    "profile": PROFILE,
                    "protocol_profile": "Current",
                    "commands": ["hello", "peer_new", "publish_key_package", "fetch_key_package",
                        "parse_key_package", "create_group", "add_members", "remove_members",
                        "update_group_data", "self_update", "leave", "send", "send_media",
                        "open_media", "sync",
                        "fetch_welcomes", "accept_welcome", "state", "group_context"],
                    "configured_as": "marmot-app app_feature_registry() + supported_app_component_ids() at mdk_rev",
                }))
            }
            "peer_new" => {
                let name = str_arg(req, "peer")?.to_string();
                let keys = Keys::parse(str_arg(req, "secret")?).map_err(as_fail(INTERNAL, "secret"))?;
                self.vectors.secrets.push(keys.secret_key().to_secret_hex());
                let pubkey = keys.public_key().to_hex();
                let db_key = hex::encode(Keys::generate().secret_key().to_secret_bytes());
                let config = SessionConfig::new(
                    self.dir.path().join(format!("{name}.sqlite")),
                    SqlCipherKey::new(&db_key).map_err(engine_fail("sqlcipher key"))?,
                    keys.public_key().to_bytes().to_vec(),
                    Box::new(transport_nostr_peeler::NostrMlsPeeler::new().with_welcome_signer(keys.clone())),
                )
                .account_identity_proof_signer(Arc::new(ProofSigner { keys: keys.clone() }));
                // "config": "engine-default" (nostrc-qp24.5.1.3): cgka-engine's
                // own defaults -- the empty feature registry and the default
                // components (profile, admin policy, lifecycle) plus Nostr
                // routing -- instead of White Noise's marmot-app configuration.
                // Its groups require no SelfRemove, agent stream or media v2.
                let engine_default = match req.get("config") {
                    None => false,
                    Some(v) => match v.as_str() {
                        Some("white-noise") => false,
                        Some("engine-default") => true,
                        _ => return Err(fail(INTERNAL, "'config' must be \"white-noise\" or \"engine-default\"")),
                    },
                };
                let config = if engine_default {
                    config
                        .feature_registry(FeatureRegistry::new())
                        .supported_app_components(engine_default_component_ids())
                } else {
                    config
                        .feature_registry(app_feature_registry())
                        .supported_app_components(supported_app_component_ids())
                };
                let session = AccountDeviceSession::open(config).map_err(engine_fail("session open"))?;
                let slot = hex::encode(Keys::generate().secret_key().to_secret_bytes());
                let client = match req.get("client") {
                    None => Some(WHITE_NOISE_CLIENT_NAME.to_string()),
                    Some(Value::Null) => None,
                    Some(v) => Some(v.as_str().ok_or_else(|| fail(INTERNAL, "'client' must be a string or null"))?.to_string()),
                };
                let peer = Peer {
                    name: name.clone(),
                    keys,
                    session,
                    slot,
                    client,
                    seen: HashSet::new(),
                    wraps: HashMap::new(),
                    joined: HashMap::new(),
                };
                self.peers.insert(name, peer);
                Ok(json!({ "pubkey": pubkey }))
            }
            "publish_key_package" => self.publish_key_package(req).await,
            "fetch_key_package" => {
                let author = str_arg(req, "author")?;
                // Where to look: the author's kind 10002 write relays, found on
                // `discover` (the adopted transport, nostrc-8u53), or `from`.
                let (from, write_relays) = if req.get("discover").is_some() {
                    let write = write_relays(&strs_arg(req, "discover")?, author).await?;
                    (write.clone(), json!(write))
                } else {
                    (strs_arg(req, "from")?, Value::Null)
                };
                let filter = json!({ "kinds": [KIND_KEY_PACKAGE], "authors": [author] });
                let events = fetch_all(&from, &filter, &Keys::generate()).await?;
                let (selected, slots) = select_key_package(author, events)?;
                let view = describe_key_package(&selected);
                let name = str_arg(req, "peer")?.to_string();
                self.vectors.record("key_package_event_fetched", &name, json!({ "event": selected, "mdk": view }));
                Ok(json!({ "event": selected.as_json(), "mdk": view, "slots": slots,
                           "write_relays": write_relays }))
            }
            "parse_key_package" => {
                let event = event_arg(req.get("event").ok_or_else(|| fail(INTERNAL, "missing 'event'"))?)?;
                Ok(describe_key_package(&event))
            }
            "create_group" => self.create_group(req).await,
            "add_members" => {
                let group_id = group_id_arg(req)?;
                let (kps, authors) = admitted_key_packages(req)?;
                let welcome_relays = strs_arg(req, "welcome_relays")?;
                let intent = SendIntent::Invite { group_id: group_id.clone(), key_packages: kps, initial_admins: vec![] };
                self.evolve(req, group_id, intent, &welcome_relays, &authors).await
            }
            "remove_members" => {
                let group_id = group_id_arg(req)?;
                let members = strs_arg(req, "members")?.iter().map(|m| member_id(m)).collect::<Res<Vec<_>>>()?;
                let intent = SendIntent::RemoveMembers { group_id: group_id.clone(), members };
                self.evolve(req, group_id, intent, &[], &[]).await
            }
            "update_group_data" => {
                let group_id = group_id_arg(req)?;
                let intent = SendIntent::UpdateGroupData {
                    group_id: group_id.clone(),
                    name: req.get("name").and_then(Value::as_str).map(str::to_string),
                    description: req.get("description").and_then(Value::as_str).map(str::to_string),
                };
                self.evolve(req, group_id, intent, &[], &[]).await
            }
            "update_routing" => {
                // nostrc-ms4d: an admin's change of the group's Nostr routing
                // (0x8004) through the engine's generic UpdateAppComponents
                // (marmot-app sets routing only at creation). "relays": the
                // new signed list; "rotate": true also moves the group to a
                // fresh random nostr_group_id (a routing rotation), else it
                // keeps its address. The Commit goes to the prior routing's
                // relays (evolve() reads it before the merge).
                let group_id = group_id_arg(req)?;
                let relays = strs_arg(req, "relays")?;
                let rotate = req.get("rotate").and_then(Value::as_bool).unwrap_or(false);
                let previous = routing(self.peer(req)?, &group_id)?.nostr_group_id;
                let nostr_group_id: [u8; 32] =
                    if rotate { Keys::generate().secret_key().to_secret_bytes() } else { previous };
                let route = NostrRoutingV1::new(nostr_group_id, relays)
                    .map_err(|e| fail(INTERNAL, format!("routing: {e}")))?;
                let data = encode_nostr_routing_v1(&route).map_err(|e| fail(INTERNAL, format!("routing: {e}")))?;
                let intent = SendIntent::UpdateAppComponents {
                    group_id: group_id.clone(),
                    updates: vec![AppComponentData { component_id: NOSTR_ROUTING_COMPONENT_ID, data }],
                };
                let mut state = self.evolve(req, group_id, intent, &[], &[]).await?;
                state["previous_nostr_group_id"] = json!(hex::encode(previous));
                Ok(state)
            }
            "self_update" => {
                let group_id = group_id_arg(req)?;
                let intent = SendIntent::SelfUpdate { group_id: group_id.clone() };
                self.evolve(req, group_id, intent, &[], &[]).await
            }
            "leave" => self.leave(req).await,
            "send" => self.send(req).await,
            "send_media" => self.send_media(req).await,
            "open_media" => self.open_media(req).await,
            "sync" => self.sync(req).await,
            "fetch_welcomes" => self.fetch_welcomes(req).await,
            "accept_welcome" => {
                // A Welcome is joined when it is ingested (fetch_welcomes);
                // this answers the joined group's state, or why there is none.
                let wrapper = str_arg(req, "wrapper_id")?.to_string();
                let peer = self.peer(req)?;
                if let Some(group_id) = peer.joined.get(&wrapper).cloned() {
                    return group_state(peer, &group_id);
                }
                match peer.wraps.get(&wrapper) {
                    Some(outcome) => Err(fail(
                        match outcome.get("class").and_then(Value::as_str) {
                            Some(UNSUPPORTED) => UNSUPPORTED,
                            Some(CRYPTO) => CRYPTO,
                            _ => STATE,
                        },
                        format!("the Welcome was not joined: {outcome}"),
                    )),
                    None => Err(fail(STATE, "no such Welcome")),
                }
            }
            "state" => {
                let group_id = group_id_arg(req)?;
                let peer = self.peer(req)?;
                group_state(peer, &group_id)
            }
            "group_context" => {
                let group_id = group_id_arg(req)?;
                let peer = self.peer(req)?;
                let mut context = group_context(peer, &group_id)?;
                context["epoch"] = json!(peer.session.epoch(&group_id).map_err(engine_fail("epoch"))?.0);
                let name = peer.name.clone();
                self.vectors.record("group_context", &name, json!({ "group": hex::encode(group_id.as_slice()), "context": context }));
                Ok(context)
            }
            "export_secret" | "group_extension" | "welcome_bytes" => Err(fail(
                UNSUPPORTED,
                format!("'{cmd}' is a 0.8-driver vector command; the 0.11 driver returns no secrets and has no 0xF2EE extension"),
            )),
            other => Err(fail(UNSUPPORTED, format!("unknown command '{other}'"))),
        }
    }

    async fn publish_key_package(&mut self, req: &Value) -> Res<Value> {
        // "relays" is accepted for the 0.8 contract and unused: adopted
        // KeyPackages carry no relays tag (KeyPackage relays are the
        // account's NIP-65 list; the caller names the targets in "to").
        let targets = strs_arg(req, "to")?;
        let peer = self.peer(req)?;
        let key_package = peer.session.fresh_key_package().await.map_err(engine_fail("fresh_key_package"))?;
        let metadata = key_package_metadata(&key_package).map_err(engine_fail("KeyPackage metadata"))?;
        let publication = NostrKeyPackagePublication {
            client_name: peer.client.clone(),
            account_id: MemberId::new(peer.keys.public_key().to_bytes().to_vec()),
            key_package,
            key_package_slot_id: peer.slot.clone(),
            key_package_ref: metadata.key_package_ref_hex.clone(),
            mls_ciphersuite: format!("0x{:04x}", metadata.ciphersuite),
            mls_extensions: hex_ids(metadata.mls_extensions.iter().copied()),
            mls_proposals: hex_ids(metadata.mls_proposals.iter().copied()),
            app_components: hex_ids(
                metadata.app_components.iter().copied().filter(|id| *id >= PRIVATE_USE_APP_COMPONENT_ID_START),
            ),
            publish_endpoints: if targets.is_empty() {
                vec![TransportEndpoint("ws://127.0.0.1:1".into())]
            } else {
                targets.iter().cloned().map(TransportEndpoint).collect()
            },
        };
        let unsigned = publication.to_event().map_err(as_fail(INTERNAL, "KeyPackage event"))?;
        let tags: Vec<Tag> = unsigned
            .tags
            .iter()
            .map(|t| Tag::parse(t.clone()).map_err(as_fail(INTERNAL, "tag")))
            .collect::<Res<_>>()?;
        let event = EventBuilder::new(Kind::Custom(KIND_KEY_PACKAGE), unsigned.content.clone())
            .tags(tags)
            .custom_created_at(Timestamp::from_secs(unsigned.created_at))
            .finalize(&peer.keys)
            .map_err(as_fail(INTERNAL, "sign KeyPackage"))?;
        if event.id.to_hex() != unsigned.id {
            return Err(fail(INTERNAL, "signed KeyPackage event differs from the adapter's"));
        }
        // "to": [] makes the KeyPackage without publishing it (vectors).
        let (accepted, relays) = publish_all(&targets, &event, &Keys::generate()).await;
        if !accepted && !targets.is_empty() {
            return Err(fail(TRANSPORT, format!("no relay accepted the KeyPackage: {relays}")));
        }
        let name = peer.name.clone();
        let view = describe_metadata(&metadata);
        self.vectors.record("key_package_event", &name, json!({ "event": event, "mdk": view }));
        Ok(json!({ "event": event.as_json(), "event_id": event.id.to_hex(), "relays": relays, "mdk": view }))
    }

    async fn create_group(&mut self, req: &Value) -> Res<Value> {
        let name = str_arg(req, "name")?.to_string();
        let description = req.get("description").and_then(Value::as_str).unwrap_or("").to_string();
        let relays = strs_arg(req, "relays")?;
        let admins = strs_arg(req, "admins")?;
        let (kps, authors) = admitted_key_packages(req)?;
        let welcome_relays = strs_arg(req, "welcome_relays")?;
        let peer = self.peer(req)?;
        let me = peer.keys.public_key().to_hex();
        // The creator is always an admin; the others must be invitees.
        let initial_admins = admins.iter().filter(|a| **a != me).map(|a| member_id(a)).collect::<Res<Vec<_>>>()?;
        let nostr_group_id: [u8; 32] = Keys::generate().secret_key().to_secret_bytes();
        let route = NostrRoutingV1::new(nostr_group_id, relays.clone())
            .map_err(|e| fail(INTERNAL, format!("routing: {e}")))?;
        let routing = AppComponentData {
            component_id: NOSTR_ROUTING_COMPONENT_ID,
            data: encode_nostr_routing_v1(&route).map_err(|e| fail(INTERNAL, format!("routing: {e}")))?,
        };
        let mut app_components = vec![routing];
        // "white_noise": the group components marmot-app's
        // create_group_with_initial_source adds to every group (946e0547,
        // crates/marmot-app/src/client/mod.rs): the agent text stream's
        // user_to_agent_default (0x8006, required role receive) and
        // encrypted media v2 (0x800b; EncryptedMediaPolicyV2::blossom_default
        // of "media_endpoints", which marmot-app fills with its default Blossom
        // servers -- the test passes its own, never contacted).
        if req.get("white_noise").and_then(Value::as_bool) == Some(true) {
            app_components.push(
                AgentTextStreamQuicPolicyV1::user_to_agent_default()
                    .to_app_component_data()
                    .map_err(|e| fail(INTERNAL, format!("agent text stream: {e}")))?,
            );
            let endpoints = strs_arg(req, "media_endpoints")?;
            let policy = EncryptedMediaPolicyV2::blossom_default(endpoints)
                .map_err(|e| fail(INTERNAL, format!("encrypted media: {e}")))?;
            app_components.push(AppComponentData {
                component_id: GROUP_ENCRYPTED_MEDIA_V2_COMPONENT_ID,
                data: encode_encrypted_media_policy_v2(&policy)
                    .map_err(|e| fail(INTERNAL, format!("encrypted media: {e}")))?,
            });
        }
        let request = CreateGroupRequest {
            name,
            description,
            members: kps,
            required_features: vec![],
            app_components,
            initial_admins,
        };
        // marmot-app's invite precheck: every KeyPackage must meet the
        // creation requirements (profile, ciphersuite, required capabilities,
        // mandatory components and the agent-stream roles) before any MLS
        // state is made (resolve_compatible_member_key_packages).
        let requirements = peer
            .session
            .create_key_package_requirements(&request)
            .map_err(engine_fail("create_key_package_requirements"))?;
        for kp in &request.members {
            requirements.validate(kp).map_err(engine_fail("invite precheck"))?;
        }
        let created = peer
            .session
            .create_group(request)
            .await
            .map_err(engine_fail("create_group"))?;
        let group_id = created.group_id.clone();
        let sent = match created.effects.publish.as_slice() {
            // Current profile: the founding group is canonical at once; each
            // Welcome is delivered, and acknowledged, on its own.
            [PublishWork::FoundingGroupCreated { welcomes }] => {
                let sent = send_welcomes(welcomes, &authors, &welcome_relays).await?;
                if !welcome_relays.is_empty() {
                    for msg in welcomes {
                        peer.session.mark_sent_welcome_delivered(&msg.id).map_err(engine_fail("mark_sent_welcome_delivered"))?;
                    }
                }
                sent
            }
            // Legacy lifecycle: publish-before-apply, confirmed once out.
            [PublishWork::GroupCreated { pending, welcomes }] => {
                let pending = *pending;
                match send_welcomes(welcomes, &authors, &welcome_relays).await {
                    Ok(sent) => {
                        peer.session.confirm_published(pending).await.map_err(engine_fail("confirm_published"))?;
                        sent
                    }
                    Err(e) => {
                        let _ = peer.session.publish_failed(pending).await;
                        return Err(e);
                    }
                }
            }
            other => return Err(fail(INTERNAL, format!("unexpected create_group work: {}", work_names(other)))),
        };
        let mut state = group_state(peer, &group_id)?;
        state["welcomes"] = sent;
        let context = group_context(peer, &group_id)?;
        let name = peer.name.clone();
        self.vectors.record("group_context", &name, json!({ "group": hex::encode(group_id.as_slice()), "epoch": state["epoch"], "context": context }));
        Ok(state)
    }

    /// A Commit (invite, removal, profile update, self-update): published to
    /// the group relays first and merged only once one accepted it (else
    /// rolled back); the Welcomes it makes are sent after the Commit.
    async fn evolve(
        &mut self,
        req: &Value,
        group_id: GroupId,
        intent: SendIntent,
        welcome_relays: &[String],
        invitees: &[String],
    ) -> Res<Value> {
        let peer = self.peer(req)?;
        let effects = peer.session.send(intent).await.map_err(engine_fail("send"))?;
        let (msg, welcomes, pending) = match effects.publish.as_slice() {
            [PublishWork::GroupEvolution { msg, welcomes, pending }] => (msg.clone(), welcomes.clone(), *pending),
            [] if !effects.queued.is_empty() => {
                return Err(fail(STATE, "the change was queued: the group's epoch is not settled"));
            }
            other => return Err(fail(INTERNAL, format!("unexpected work: {}", work_names(other)))),
        };
        let commit = transport_event(&msg)?;
        let relays = routing(peer, &group_id)?.relays;
        let (accepted, answers) = publish_all(&relays, &commit, &Keys::generate()).await;
        if !accepted {
            let _ = peer.session.publish_failed(pending).await;
            return Err(fail(TRANSPORT, format!("no group relay accepted the Commit: {answers}")));
        }
        peer.seen.insert(commit.id);
        let confirmed = peer.session.confirm_published(pending).await.map_err(engine_fail("confirm_published"))?;
        let sent = send_welcomes(&welcomes, invitees, welcome_relays).await?;
        let mut state = group_state(peer, &group_id)?;
        state["commit_id"] = json!(commit.id.to_hex());
        state["welcomes"] = sent;
        state["events"] = Value::Array(confirmed.events.iter().map(describe_event).collect());
        let name = peer.name.clone();
        let context = group_context(peer, &group_id)?;
        self.vectors.record("kind445_commit", &name, json!({ "event": commit, "epoch": state["epoch"] }));
        self.vectors.record("group_context", &name, json!({ "group": hex::encode(group_id.as_slice()), "epoch": state["epoch"], "context": context }));
        Ok(state)
    }

    /// SelfRemove (MIP-03 departure): a standalone proposal on the group
    /// relays; an admin's next convergence commits it.
    async fn leave(&mut self, req: &Value) -> Res<Value> {
        let group_id = group_id_arg(req)?;
        let peer = self.peer(req)?;
        let effects = peer
            .session
            .send(SendIntent::Leave { group_id: group_id.clone() })
            .await
            .map_err(engine_fail("leave"))?;
        let mut proposals = Vec::new();
        for work in &effects.publish {
            if let PublishWork::Proposal { msg, .. } = work {
                let event = transport_event(msg)?;
                let relays = routing(peer, &group_id)?.relays;
                let (accepted, answers) = publish_all(&relays, &event, &Keys::generate()).await;
                if !accepted {
                    return Err(fail(TRANSPORT, format!("no group relay accepted the SelfRemove: {answers}")));
                }
                peer.seen.insert(event.id);
                proposals.push(event);
            }
        }
        if proposals.is_empty() {
            return Err(fail(INTERNAL, format!("leave made no proposal: {}", work_names(&effects.publish))));
        }
        let mut state = group_state(peer, &group_id)?;
        state["proposal_id"] = json!(proposals[0].id.to_hex());
        let name = peer.name.clone();
        for p in &proposals {
            self.vectors.record("kind445_self_remove", &name, json!({ "event": p }));
        }
        Ok(state)
    }

    async fn send(&mut self, req: &Value) -> Res<Value> {
        let group_id = group_id_arg(req)?;
        let text = str_arg(req, "text")?.to_string();
        let publish = req.get("publish").and_then(Value::as_bool) != Some(false);
        let peer = self.peer(req)?;
        let now = Timestamp::now().as_secs();
        let payload = MarmotAppEvent::new(peer.keys.public_key().to_hex(), now, MARMOT_APP_EVENT_KIND_CHAT, vec![], text)
            .encode()
            .map_err(as_fail(INTERNAL, "app event"))?;
        let effects = peer
            .session
            .send(SendIntent::AppMessage { group_id: group_id.clone(), payload, expected_epoch: None })
            .await
            .map_err(engine_fail("send"))?;
        let msg = effects
            .publish
            .iter()
            .find_map(|w| match w {
                PublishWork::ApplicationMessage { msg, .. } => Some(msg.clone()),
                _ => None,
            })
            .ok_or_else(|| fail(STATE, format!("no message to publish (queued: {})", effects.queued.len())))?;
        let event = transport_event(&msg)?;
        peer.seen.insert(event.id);
        let name = peer.name.clone();
        if !publish {
            self.vectors.record("kind445_application", &name, json!({ "event": event }));
            return Ok(json!({ "event": event.as_json(), "event_id": event.id.to_hex() }));
        }
        let relays = routing(peer, &group_id)?.relays;
        let (accepted, answers) = publish_all(&relays, &event, &Keys::generate()).await;
        if !accepted {
            return Err(fail(TRANSPORT, format!("no group relay accepted the message: {answers}")));
        }
        self.vectors.record("kind445_application", &name, json!({ "event": event }));
        Ok(json!({ "event_id": event.id.to_hex() }))
    }

    /// MIP-04 encrypted-media-v2 as marmot-app 0.11 sends it
    /// (`upload_encrypted_media_attachment`, `AppMessageIntent::Media`): the
    /// file is sealed with the group's current-epoch media exporter
    /// (`MLS-Exporter("marmot", "encrypted-media", 32)`), its key and AAD
    /// from marmot-app's `media_key_info`/`media_aad` (verbatim, parity
    /// tested), ChaCha20-Poly1305 under a fresh nonce, uploaded to `blossom`
    /// (BUD-02, a kind-24242 authorization signed by the peer's account, as
    /// marmot-app's signer does), then one kind-9 app event whose tags are the
    /// ordered `imeta` (`imeta_tag`, verbatim) and whose content is the
    /// caption, sent only in that epoch (`expected_epoch`). The plaintext is
    /// test data, never a secret; the media secret and file key never leave.
    async fn send_media(&mut self, req: &Value) -> Res<Value> {
        let group_id = group_id_arg(req)?;
        let file = base64::engine::general_purpose::STANDARD
            .decode(str_arg(req, "file")?)
            .map_err(as_fail(INTERNAL, "file"))?;
        let media_type = str_arg(req, "mime")?.to_string();
        let file_name = str_arg(req, "filename")?.to_string();
        let blossom = str_arg(req, "blossom")?.trim_end_matches('/').to_string();
        let caption = req.get("caption").and_then(Value::as_str).map(str::to_string);
        let peer = self.peer(req)?;
        let (epoch, secret) = peer
            .session
            .exporter_secret_with_epoch(&group_id, GROUP_ENCRYPTED_MEDIA_EXPORTER_CACHE_KEY, 32)
            .map_err(engine_fail("media exporter"))?;
        let plaintext_hash: [u8; 32] = Sha256::digest(&file).into();
        let mut nonce = [0u8; 12];
        nonce.copy_from_slice(&Keys::generate().secret_key().to_secret_bytes()[..12]);
        let version = EncryptedMediaVersion::V2;
        let hkdf = hkdf::Hkdf::<Sha256>::from_prk(secret.as_ref())
            .map_err(|_| fail(INTERNAL, "media secret is not a PRK"))?;
        let mut key = [0u8; 32];
        hkdf.expand(&media_key_info(version, &plaintext_hash, &media_type, &file_name), &mut key)
            .map_err(|_| fail(INTERNAL, "media key derivation"))?;
        let aad = media_aad(version, &plaintext_hash, &media_type, &file_name);
        let mut encrypted = file;
        ChaCha20Poly1305::new_from_slice(&key)
            .map_err(|_| fail(INTERNAL, "media key length"))?
            .encrypt_in_place(Nonce::from_slice(&nonce), &aad, &mut encrypted)
            .map_err(|_| fail(INTERNAL, "media encryption"))?;
        key.iter_mut().for_each(|b| *b = 0);
        let ciphertext_sha256 = hex::encode(Sha256::digest(&encrypted));
        let url = blossom_upload(&blossom, &encrypted, &ciphertext_sha256, &peer.keys).await?;
        let reference = MediaAttachmentReference {
            locators: vec![MediaLocator { kind: "blossom-v1".to_string(), value: url.clone() }],
            ciphertext_sha256: ciphertext_sha256.clone(),
            plaintext_sha256: hex::encode(plaintext_hash),
            nonce_hex: hex::encode(nonce),
            file_name,
            media_type,
            version: version.as_str().to_string(),
            dim: req.get("dim").and_then(Value::as_str).map(str::to_string),
            thumbhash: None,
        };
        let imeta = reference.imeta_tag();
        let now = Timestamp::now().as_secs();
        let payload = MarmotAppEvent::new(peer.keys.public_key().to_hex(), now,
                                          MARMOT_APP_EVENT_KIND_CHAT, vec![imeta.clone()],
                                          caption.unwrap_or_default())
            .encode()
            .map_err(as_fail(INTERNAL, "app event"))?;
        let effects = peer
            .session
            .send(SendIntent::AppMessage { group_id: group_id.clone(), payload,
                                           expected_epoch: Some(epoch) })
            .await
            .map_err(engine_fail("send"))?;
        let msg = effects
            .publish
            .iter()
            .find_map(|w| match w {
                PublishWork::ApplicationMessage { msg, .. } => Some(msg.clone()),
                _ => None,
            })
            .ok_or_else(|| fail(STATE, format!("no message to publish (queued: {})", effects.queued.len())))?;
        let event = transport_event(&msg)?;
        peer.seen.insert(event.id);
        let name = peer.name.clone();
        let relays = routing(peer, &group_id)?.relays;
        let (accepted, answers) = publish_all(&relays, &event, &Keys::generate()).await;
        if !accepted {
            return Err(fail(TRANSPORT, format!("no group relay accepted the message: {answers}")));
        }
        self.vectors.record("kind445_application", &name, json!({ "event": event }));
        Ok(json!({ "event_id": event.id.to_hex(), "epoch": epoch.0, "imeta": imeta,
                   "url": url, "ciphertext_sha256": ciphertext_sha256 }))
    }

    /// Opens a received v2 attachment as marmot-app's download path does
    /// (`download_encrypted_media_with_transport`), but NOT through its imeta
    /// parser and validator (nostrc-qeyg): its blossom-v1 locator
    /// fetched, the body's SHA-256 checked first, then the AEAD with the
    /// source epoch's media key, then the plaintext hash. The driver keeps no
    /// per-epoch secret cache, so the source epoch must be the group's
    /// current one (`state`), else `unsupported`. Answers the plaintext
    /// (test data) in base64.
    async fn open_media(&mut self, req: &Value) -> Res<Value> {
        let group_id = group_id_arg(req)?;
        let source_epoch = req.get("epoch").and_then(Value::as_u64)
            .ok_or_else(|| fail(INTERNAL, "epoch"))?;
        let tag: Vec<String> = req.get("imeta").and_then(Value::as_array)
            .ok_or_else(|| fail(INTERNAL, "imeta"))?
            .iter()
            .map(|v| v.as_str().unwrap_or_default().to_string())
            .collect();
        // Not marmot-app's parser (nostrc-qeyg): only the fields the key and
        // AAD are derived from, each exactly once (a first-wins reader of a
        // duplicated m, filename or hash would derive another key).
        for name in ["v", "ciphertext_sha256", "plaintext_sha256", "nonce", "m", "filename"] {
            let prefix = format!("{name} ");
            if tag.iter().skip(1).filter(|f| f.starts_with(&prefix)).count() > 1 {
                return Err(fail(CRYPTO, format!("duplicate imeta field {name}")));
            }
        }
        let field = |name: &str| -> Option<String> {
            tag.iter().skip(1).find_map(|f| f.strip_prefix(&format!("{name} ")).map(str::to_string))
        };
        if tag.first().map(String::as_str) != Some("imeta") ||
            field("v").as_deref() != Some(EncryptedMediaVersion::V2.as_str()) {
            return Err(fail(UNSUPPORTED, "not an encrypted-media-v2 imeta"));
        }
        let locator = field("locator").ok_or_else(|| fail(CRYPTO, "no locator"))?;
        let url = locator.strip_prefix("blossom-v1 ").ok_or_else(|| fail(UNSUPPORTED, "locator kind"))?;
        let want_ct = field("ciphertext_sha256").ok_or_else(|| fail(CRYPTO, "ciphertext_sha256"))?;
        let want_pt = field("plaintext_sha256").ok_or_else(|| fail(CRYPTO, "plaintext_sha256"))?;
        let nonce = hex::decode(field("nonce").ok_or_else(|| fail(CRYPTO, "nonce"))?)
            .map_err(as_fail(CRYPTO, "nonce"))?;
        let media_type = field("m").ok_or_else(|| fail(CRYPTO, "m"))?;
        let file_name = field("filename").ok_or_else(|| fail(CRYPTO, "filename"))?;
        let peer = self.peer(req)?;
        let (epoch, secret) = peer
            .session
            .exporter_secret_with_epoch(&group_id, GROUP_ENCRYPTED_MEDIA_EXPORTER_CACHE_KEY, 32)
            .map_err(engine_fail("media exporter"))?;
        if epoch.0 != source_epoch {
            return Err(fail(UNSUPPORTED, format!(
                "the driver keeps only the current epoch's media secret ({} != {source_epoch})", epoch.0)));
        }
        let mut body = http_get(url).await?;
        if hex::encode(Sha256::digest(&body)) != want_ct.to_ascii_lowercase() {
            return Err(fail(CRYPTO, "ciphertext hash mismatch"));
        }
        let plaintext_hash: [u8; 32] = hex::decode(&want_pt).map_err(as_fail(CRYPTO, "plaintext_sha256"))?
            .try_into().map_err(|_| fail(CRYPTO, "plaintext_sha256 length"))?;
        let version = EncryptedMediaVersion::V2;
        let hkdf = hkdf::Hkdf::<Sha256>::from_prk(secret.as_ref())
            .map_err(|_| fail(INTERNAL, "media secret is not a PRK"))?;
        let mut key = [0u8; 32];
        hkdf.expand(&media_key_info(version, &plaintext_hash, &media_type, &file_name), &mut key)
            .map_err(|_| fail(INTERNAL, "media key derivation"))?;
        let aad = media_aad(version, &plaintext_hash, &media_type, &file_name);
        if nonce.len() != 12 {
            return Err(fail(CRYPTO, "nonce length"));
        }
        let opened = ChaCha20Poly1305::new_from_slice(&key)
            .map_err(|_| fail(INTERNAL, "media key length"))?
            .decrypt_in_place(Nonce::from_slice(&nonce), &aad, &mut body);
        key.iter_mut().for_each(|b| *b = 0);
        opened.map_err(|_| fail(CRYPTO, "media decryption failed"))?;
        if Sha256::digest(&body).as_slice() != plaintext_hash {
            return Err(fail(CRYPTO, "plaintext hash mismatch"));
        }
        Ok(json!({ "file": base64::engine::general_purpose::STANDARD.encode(&body),
                   "media_type": media_type, "filename": file_name }))
    }

    /// Every kind 445 of the group on its relays, ingested oldest first as a
    /// fixpoint (an input the engine defers for a later epoch is offered
    /// again once the epoch moved), with the engine's settlement window
    /// waited out and convergence advanced; publish work convergence makes
    /// (e.g. the admin's Commit of a SelfRemove) is published and confirmed.
    async fn sync(&mut self, req: &Value) -> Res<Value> {
        let group_id = group_id_arg(req)?;
        let peer = self.peer(req)?;
        let route = routing(peer, &group_id)?;
        let filter = json!({ "kinds": [KIND_GROUP_MESSAGE], "#h": [hex::encode(route.nostr_group_id)] });
        let mut pending: Vec<Event> = fetch_all(&route.relays, &filter, &Keys::generate())
            .await?
            .into_iter()
            .filter(|e| !peer.seen.contains(&e.id))
            .collect();
        pending.sort_by_key(|e| (e.created_at, e.id));
        let mut results = Vec::new();
        let mut inputs = Vec::new();
        let mut published = Vec::new();
        let mut last: BTreeMap<String, Value> = BTreeMap::new();
        let mut due = false;
        loop {
            let mut progress = false;
            let mut still = Vec::new();
            for event in pending {
                let id = event.id.to_hex();
                let msg = transport_message(&event)?;
                match peer.session.ingest(msg).await {
                    Ok(ingest) => {
                        let outcome = format!("{:?}", ingest.outcome);
                        let retry = matches!(ingest.outcome, IngestOutcome::TransportDeferred { .. })
                            || ingest.left_object_unpersisted && !matches!(ingest.outcome, IngestOutcome::Ignored { .. });
                        results.extend(ingest.effects.events.iter().map(describe_event));
                        due |= ingest.effects.pending_convergence.contains(&group_id);
                        published.extend(publish_effects(peer, &ingest.effects).await?);
                        if retry {
                            last.insert(id.clone(), json!({ "id": id, "outcome": outcome }));
                            still.push(event);
                        } else {
                            peer.seen.insert(event.id);
                            last.remove(&id);
                            inputs.push(json!({ "id": id, "outcome": outcome }));
                            progress = true;
                        }
                    }
                    Err(e) => {
                        let f = engine_fail("ingest")(e);
                        last.insert(id.clone(), json!({ "id": id, "class": f.class, "error": f.error }));
                        still.push(event);
                    }
                }
            }
            pending = still;
            let (events, work) = settle(peer, &group_id, std::mem::take(&mut due)).await?;
            progress |= !events.is_empty();
            results.extend(events);
            published.extend(work);
            if pending.is_empty() || !progress {
                break;
            }
        }
        let failed: Vec<Value> = pending.iter().filter_map(|e| last.get(&e.id.to_hex()).cloned()).collect();
        let state = group_state(peer, &group_id)?;
        Ok(json!({ "results": results, "inputs": inputs, "failed": failed, "published": published, "state": state }))
    }

    async fn fetch_welcomes(&mut self, req: &Value) -> Res<Value> {
        let from = strs_arg(req, "from")?;
        let peer = self.peer(req)?;
        let me = peer.keys.public_key().to_hex();
        let filter = json!({ "kinds": [KIND_GIFT_WRAP], "#p": [me] });
        let wraps = fetch_all(&from, &filter, &peer.keys).await?;
        let mut out = Vec::new();
        let mut rumors = Vec::new();
        for wrap in wraps {
            let wrapper_id = wrap.id.to_hex();
            if peer.wraps.contains_key(&wrapper_id) {
                continue;
            }
            // The rumor, for the answer and the vectors (diagnostics: the
            // session peels the wrap itself).
            let rumor = match nostr::nips::nip59::extract_rumor(&peer.keys, &wrap) {
                Ok(u) => u,
                Err(e) => {
                    let entry = json!({ "wrapper_id": wrapper_id, "ok": false, "class": CRYPTO, "error": format!("unwrap: {e}") });
                    peer.wraps.insert(wrapper_id, entry.clone());
                    out.push(entry);
                    continue;
                }
            };
            let mut entry = json!({
                "wrapper_id": wrapper_id,
                "sender": rumor.sender.to_hex(),
                "rumor_kind": rumor.rumor.kind.as_u16(),
                "rumor_tags": serde_json::to_value(&rumor.rumor.tags).unwrap_or(Value::Null),
            });
            rumors.push(json!({ "wrapper_id": wrapper_id, "sender": rumor.sender.to_hex(), "rumor": rumor.rumor }));
            let msg = transport_message(&wrap)?;
            match peer.session.ingest(msg).await {
                Ok(ingest) => {
                    let joined = ingest.effects.events.iter().find_map(|e| match e {
                        GroupEvent::GroupJoined { group_id, welcomer, .. } => Some((group_id.clone(), welcomer.clone())),
                        _ => None,
                    });
                    entry["outcome"] = json!(format!("{:?}", ingest.outcome));
                    match joined {
                        Some((group_id, welcomer)) => {
                            entry["ok"] = json!(true);
                            entry["group"] = json!(hex::encode(group_id.as_slice()));
                            entry["welcomer"] = json!(welcomer.map(|w| hex::encode(w.as_slice())));
                            entry["group_name"] = json!(peer.session.group_record(&group_id).map(|g| g.name).unwrap_or_default());
                            entry["member_count"] = json!(peer.session.members(&group_id).map(|m| m.len()).unwrap_or(0));
                            peer.joined.insert(wrapper_id.clone(), group_id);
                        }
                        None => {
                            entry["ok"] = json!(false);
                            entry["class"] = json!(match ingest.outcome {
                                IngestOutcome::Ignored { .. } => STATE,
                                _ => UNSUPPORTED,
                            });
                            entry["error"] = json!(format!("not joined: {:?}", ingest.outcome));
                        }
                    }
                }
                Err(e) => {
                    let f = engine_fail("Welcome")(e);
                    entry["ok"] = json!(false);
                    entry["class"] = json!(f.class);
                    entry["error"] = json!(f.error);
                }
            }
            peer.wraps.insert(wrapper_id, entry.clone());
            out.push(entry);
        }
        let name = peer.name.clone();
        for rumor in rumors {
            self.vectors.record("welcome_rumor", &name, rumor);
        }
        Ok(json!({ "welcomes": out }))
    }
}

/// The engine's settlement window, waited out, and convergence advanced
/// until nothing is due: the events it produced and the work it published.
/// `due`: an ingest reported the group pending convergence (e.g. a SelfRemove
/// proposal an admin must commit once the window closes). The wait is the
/// engine's own reported cutoff (the adopted v1 policy's quiescence), bounded
/// by the deadline.
async fn settle(peer: &mut Peer, group_id: &GroupId, mut due: bool) -> Res<(Vec<Value>, Vec<Value>)> {
    let mut events = Vec::new();
    let mut published = Vec::new();
    let started = std::time::Instant::now();
    loop {
        let pending = peer.session.has_pending_convergence_inputs(group_id).map_err(engine_fail("convergence"))?;
        let queued = peer.session.has_queued_outbound_intents(group_id).map_err(engine_fail("convergence"))?;
        if !due && !pending && !queued {
            break;
        }
        if started.elapsed() > deadline() {
            return Err(fail(STATE, "convergence did not settle before the deadline"));
        }
        let cutoff = peer.session.prepare_convergence_cutoff_delay_ms(group_id).map_err(engine_fail("convergence"))?;
        if let Some(ms) = cutoff {
            tokio::time::sleep(Duration::from_millis(ms.min(deadline().as_millis() as u64))).await;
        }
        let effects = peer.session.advance_convergence(group_id).await.map_err(engine_fail("advance_convergence"))?;
        let progressed = !effects.events.is_empty() || !effects.publish.is_empty();
        due = effects.pending_convergence.contains(group_id);
        events.extend(effects.events.iter().map(describe_event));
        published.extend(publish_effects(peer, &effects).await?);
        if !progressed && cutoff.is_none() && !due {
            break;
        }
    }
    Ok((events, published))
}

/// Publishes work an ingest or convergence step made: auto-published Commits
/// (confirmed once a group relay accepted them, else failed) and proposals.
async fn publish_effects(peer: &mut Peer, effects: &SessionEffects) -> Res<Vec<Value>> {
    let mut out = Vec::new();
    for work in &effects.publish {
        let (msg, pending): (&TransportMessage, Option<PendingStateRef>) = match work {
            PublishWork::AutoPublish { msg, pending } => (msg, Some(*pending)),
            PublishWork::GroupEvolution { msg, pending, .. } => (msg, Some(*pending)),
            PublishWork::Proposal { msg, .. } | PublishWork::ApplicationMessage { msg, .. } => (msg, None),
            _ => continue,
        };
        let event = transport_event(msg)?;
        let relays = relays_of_transport_group(peer, &event)?;
        let (accepted, answers) = publish_all(&relays, &event, &Keys::generate()).await;
        peer.seen.insert(event.id);
        if let Some(pending) = pending {
            if accepted {
                peer.session.confirm_published(pending).await.map_err(engine_fail("confirm_published"))?;
            } else {
                let _ = peer.session.publish_failed(pending).await;
            }
        }
        out.push(json!({ "id": event.id.to_hex(), "accepted": accepted, "answers": answers }));
    }
    Ok(out)
}

/// The relays of the group a kind 445 is routed to (its `h` tag).
fn relays_of_transport_group(peer: &Peer, event: &Event) -> Res<Vec<String>> {
    let h = tag_value(event, "h").first().and_then(|t| t.get(1).cloned()).unwrap_or_default();
    for group_id in peer.session.live_group_ids().map_err(engine_fail("groups"))? {
        if let Ok(route) = routing(peer, &group_id) {
            if hex::encode(route.nostr_group_id) == h {
                return Ok(route.relays);
            }
        }
    }
    Err(fail(STATE, "no group for the event's routing id"))
}

/// The KeyPackages of a request's "key_packages" (event JSON), as MDK 0.11
/// admits them, and their authors.
fn admitted_key_packages(req: &Value) -> Res<(Vec<KeyPackage>, Vec<String>)> {
    let mut kps = Vec::new();
    let mut authors = Vec::new();
    for value in req.get("key_packages").and_then(Value::as_array).ok_or_else(|| fail(INTERNAL, "missing 'key_packages'"))? {
        let event = event_arg(value)?;
        let (kp, _) = admit_key_package(&event)?;
        kps.push(kp);
        authors.push(event.pubkey.to_hex());
    }
    Ok((kps, authors))
}

/// Each Welcome (already a NIP-59 gift wrap to its invitee) published to
/// welcome_relays. None given: made, not sent (vectors).
async fn send_welcomes(welcomes: &[TransportMessage], invitees: &[String], welcome_relays: &[String]) -> Res<Value> {
    let mut out = Vec::new();
    for msg in welcomes {
        let wrap = transport_event(msg)?;
        let to = tag_value(&wrap, "p").first().and_then(|t| t.get(1).cloned()).unwrap_or_default();
        if !invitees.is_empty() && !invitees.contains(&to) {
            return Err(fail(INTERNAL, "a Welcome for someone not invited"));
        }
        let (accepted, answers) = publish_all(welcome_relays, &wrap, &Keys::generate()).await;
        if !accepted && !welcome_relays.is_empty() {
            return Err(fail(TRANSPORT, format!("no inbox relay accepted the Welcome: {answers}")));
        }
        out.push(json!({ "to": to, "wrapper_id": wrap.id.to_hex() }));
    }
    Ok(Value::Array(out))
}

/// The kinds of publish work, for errors (their Debug form is the bytes).
fn work_names(work: &[PublishWork]) -> String {
    let names: Vec<String> = work.iter().map(|w| variant_name(&format!("{w:?}"))).collect();
    format!("[{}]", names.join(", "))
}

fn sha256_hex(bytes: &[u8]) -> String {
    hex::encode(Sha256::digest(bytes))
}

#[tokio::main(flavor = "current_thread")]
async fn main() {
    let filter = tracing_subscriber::EnvFilter::try_from_env("MDK_DRIVER_LOG")
        .unwrap_or_else(|_| tracing_subscriber::EnvFilter::new("warn"));
    tracing_subscriber::fmt()
        .with_env_filter(filter)
        .with_writer(std::io::stderr)
        .with_ansi(false)
        .init();
    let dir = tempfile::tempdir().expect("a temporary directory for the peers' databases");
    let mut driver = Driver { dir, peers: HashMap::new(), vectors: Vectors::open() };
    let mut lines = BufReader::new(tokio::io::stdin()).lines();
    let mut stdout = tokio::io::stdout();
    while let Ok(Some(line)) = lines.next_line().await {
        if line.trim().is_empty() {
            continue;
        }
        let req: Value = match serde_json::from_str(&line) {
            Ok(v) => v,
            Err(e) => {
                eprintln!("mdk011-driver: bad request line: {e}");
                continue;
            }
        };
        let id = req.get("id").cloned().unwrap_or(Value::Null);
        let answer = match driver.handle(&req).await {
            Ok(mut value) => {
                if !value.is_object() {
                    value = json!({ "value": value });
                }
                value["id"] = id;
                value["ok"] = json!(true);
                value
            }
            Err(e) => {
                eprintln!("mdk011-driver: {} failed ({}): {}", req.get("cmd").unwrap_or(&Value::Null), e.class, e.error);
                json!({ "id": id, "ok": false, "class": e.class, "error": e.error })
            }
        };
        let mut text = answer.to_string();
        text.push('\n');
        if stdout.write_all(text.as_bytes()).await.is_err() || stdout.flush().await.is_err() {
            break;
        }
    }
}

/// The driver's session configuration is marmot-app's at MDK_REV: the two
/// functions that decide what leaves and KeyPackages advertise are
/// byte-for-byte (comments and whitespace aside) marmot-app's, and marmot-app
/// configures its SessionConfig with nothing else that reaches the wire.
#[cfg(test)]
mod parity {
    use super::MDK_REV;
    use std::path::PathBuf;

    const DRIVER: &str = include_str!("main.rs");

    /// marmot-app's lib.rs in Cargo's git checkout of MDK at MDK_REV
    /// (`$CARGO_HOME/git/checkouts/mdk-<hash>/<rev[..7]>/`: the whole
    /// repository at that commit, fetched for the build).
    fn marmot_app() -> String {
        let home = std::env::var_os("CARGO_HOME")
            .map(PathBuf::from)
            .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".cargo")))
            .expect("CARGO_HOME or HOME");
        let checkouts = home.join("git/checkouts");
        let short = &MDK_REV[..7];
        let found: Vec<PathBuf> = std::fs::read_dir(&checkouts)
            .unwrap_or_else(|e| panic!("{}: {e}", checkouts.display()))
            .filter_map(|e| e.ok())
            .filter(|e| e.file_name().to_string_lossy().starts_with("mdk-"))
            .map(|e| e.path().join(short).join("crates/marmot-app/src/lib.rs"))
            .filter(|p| p.is_file())
            .collect();
        let lib = found.first().unwrap_or_else(|| panic!("no MDK checkout at {short} in {}", checkouts.display()));
        std::fs::read_to_string(lib).expect("marmot-app/src/lib.rs at MDK_REV")
    }

    /// The body of the function declared by `signature` in `source`: from its
    /// opening brace to the matching one, `//` comments and whitespace removed.
    fn body(source: &str, signature: &str) -> String {
        let at = source.find(signature).unwrap_or_else(|| panic!("no `{signature}`"));
        let open = at + source[at..].find('{').unwrap();
        let mut depth = 0usize;
        let mut end = open;
        for (i, c) in source[open..].char_indices() {
            match c {
                '{' => depth += 1,
                '}' => {
                    depth -= 1;
                    if depth == 0 {
                        end = open + i;
                        break;
                    }
                }
                _ => {}
            }
        }
        source[open..=end]
            .lines()
            .map(|l| l.split("//").next().unwrap())
            .collect::<String>()
            .chars()
            .filter(|c| !c.is_whitespace())
            .collect()
    }

    fn mdk_file(relative: &str) -> String {
        let home = std::env::var_os("CARGO_HOME")
            .map(PathBuf::from)
            .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".cargo")))
            .expect("CARGO_HOME or HOME");
        let checkouts = home.join("git/checkouts");
        let short = &MDK_REV[..7];
        let found: Vec<PathBuf> = std::fs::read_dir(&checkouts)
            .unwrap_or_else(|e| panic!("{}: {e}", checkouts.display()))
            .filter_map(|e| e.ok())
            .filter(|e| e.file_name().to_string_lossy().starts_with("mdk-"))
            .map(|e| e.path().join(short).join(relative))
            .filter(|p| p.is_file())
            .collect();
        let path = found.first().unwrap_or_else(|| panic!("no {relative} at {short}"));
        std::fs::read_to_string(path).expect("MDK source at MDK_REV")
    }

    /// send_media/open_media seal and open with marmot-app's own v2 key
    /// info, AAD and imeta writer (media/crypto.rs, media/mod.rs at MDK_REV).
    #[test]
    fn media_v2_is_marmot_apps() {
        let crypto = mdk_file("crates/marmot-app/src/media/crypto.rs");
        let media = mdk_file("crates/marmot-app/src/media/mod.rs");
        for sig in ["fn media_key_info(", "fn media_aad("] {
            assert_eq!(body(DRIVER, sig), body(&crypto, sig), "{sig}");
        }
        let sig = "fn imeta_tag(&self) -> Vec<String>";
        assert_eq!(body(DRIVER, sig), body(&media, sig));
        // The key is HKDF-Expand(PRK = the media exporter, info, 32), the
        // cipher ChaCha20-Poly1305: marmot-app's derive_media_file_key.
        let derive = body(&crypto, "fn derive_media_file_key(");
        assert!(derive.contains("Hkdf::<Sha256>::from_prk(media_secret)"));
        assert!(derive.contains("hkdf.expand(&media_key_info(version,file_hash,media_type,file_name),&mutkey,)"));
        assert!(media.contains("ChaCha20Poly1305::new_from_slice(&file_key)"));
        assert!(media.contains("GROUP_ENCRYPTED_MEDIA_EXPORTER_CACHE_KEY") ||
                mdk_file("crates/marmot-app/src/client/mod.rs").contains("GROUP_ENCRYPTED_MEDIA_EXPORTER_CACHE_KEY,"));
    }

    #[test]
    fn feature_registry_is_marmot_apps() {
        let app = marmot_app();
        let sig = "fn app_feature_registry() -> FeatureRegistry";
        assert_eq!(body(DRIVER, sig), body(&app, sig));
    }

    #[test]
    fn component_set_is_marmot_apps() {
        let app = marmot_app();
        assert_eq!(
            body(DRIVER, "fn supported_app_component_ids() -> Vec<u16>"),
            body(&app, "fn supported_app_component_ids(&self) -> Vec<u16>")
        );
    }

    /// marmot-app builds its SessionConfig with exactly these calls; a new
    /// one (a protocol profile, a legacy switch, ...) fails here until the
    /// driver follows it.
    #[test]
    fn session_config_is_marmot_apps() {
        let app = marmot_app();
        let at = app.find("let mut session_config = SessionConfig::new(").expect("marmot-app's SessionConfig");
        let chain = &app[at..at + app[at..].find(';').unwrap()];
        assert_eq!(
            chain.matches("\n        .").map(|_| ()).count(),
            3,
            "marmot-app's SessionConfig chain changed:\n{chain}"
        );
        for call in [
            ".account_identity_proof_signer(",
            ".feature_registry(app_feature_registry())",
            ".supported_app_components(self.supported_app_component_ids())",
        ] {
            assert!(chain.contains(call), "marmot-app no longer calls {call}");
        }
        // Later reassignments: only wire-neutral ones (hydration timing, a
        // test-policy-only convergence override, the audit recorder).
        let mut later: Vec<&str> = app
            .match_indices("session_config = session_config.")
            .map(|(i, m)| {
                let rest = &app[i + m.len()..];
                &rest[..rest.find('(').unwrap()]
            })
            .collect();
        later.sort();
        later.dedup();
        assert_eq!(later, ["convergence_policy", "defer_group_hydration", "recorder"]);
        // The driver's own chain uses the same two functions.
        assert!(DRIVER.contains(".feature_registry(app_feature_registry())"));
        assert!(DRIVER.contains(".supported_app_components(supported_app_component_ids())"));
    }
}

#[cfg(test)]
mod redaction {
    use super::*;

    #[test]
    fn key_bearing_components_never_leave_raw() {
        let secret = [0xA5u8; 76];
        let value = component_value(GROUP_BLOSSOM_IMAGE_COMPONENT_ID, &secret);
        let text = value.to_string();
        assert!(!text.contains(&hex::encode(secret)), "0x8002 bytes leaked: {text}");
        assert_eq!(value["redacted"], json!(true));
        assert_eq!(value["len"], json!(76));
        // An id nobody vetted is redacted too.
        assert_eq!(component_value(0x80ff, &secret)["redacted"], json!(true));
        // Key-free components stay readable vectors.
        assert_eq!(component_value(GROUP_PROFILE_COMPONENT_ID, &[1, 2]), json!("0102"));
    }
}
