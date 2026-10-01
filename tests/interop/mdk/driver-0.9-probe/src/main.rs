//! MDK 0.9.0 expected-incompatible probe (nostrc-a5u5): KeyPackages made by
//! MDK v0.9.0 (a102b196, the first cgka-engine cohort, whose leaves carry the
//! superseded 0xF2F1 v1 account proof instead of the adopted 0x8009 v2),
//! exactly as MDK 0.9's marmot-app makes and publishes them, behind the
//! JSON-lines contract of ../driver-0.11. Commands: hello, peer_new,
//! publish_key_package; anything else answers `unsupported` (this is a probe,
//! not a peer: no group operation is expected to interoperate). Requests are
//! never logged; no secret is returned. See ../README.md.

use std::collections::HashMap;
use std::sync::Arc;
use std::time::Duration;

use cgka_engine::account_identity_proof::{
    ACCOUNT_IDENTITY_PROOF_EXTENSION_TYPE, AccountIdentityProofRequest, AccountIdentityProofSigner,
};
use cgka_engine::key_package::key_package_metadata;
use cgka_session::{AccountDeviceSession, SessionConfig};
use cgka_traits::app_components::{
    AGENT_TEXT_STREAM_QUIC_COMPONENT_ID, GROUP_ENCRYPTED_MEDIA_COMPONENT_ID,
    NOSTR_ROUTING_COMPONENT_ID, default_group_components,
};
use cgka_traits::{MemberId, TransportEndpoint};
use futures_util::{SinkExt, StreamExt};
use nostr::prelude::*;
use serde_json::{Value, json};
use storage_sqlite::SqlCipherKey;
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio_tungstenite::tungstenite::Message as WsMessage;
use transport_nostr_adapter::NostrKeyPackagePublication;

const MDK_REV: &str = "a102b1966267c5bfcbe3a822212c0e343ac109ef";
const PROFILE: &str = "marmot-dictionary-proof-v1";

struct Failure {
    class: &'static str,
    error: String,
}

type Res<T> = Result<T, Failure>;

fn fail(class: &'static str, error: impl Into<String>) -> Failure {
    Failure { class, error: error.into() }
}

fn as_fail<'a, E: std::fmt::Display>(class: &'static str, what: &'a str) -> impl FnOnce(E) -> Failure + 'a {
    move |e| fail(class, format!("{what}: {e}"))
}

fn deadline() -> Duration {
    Duration::from_secs(std::env::var("MDK_DRIVER_DEADLINE_S").ok().and_then(|s| s.parse().ok()).unwrap_or(30))
}

fn dial_url(url: &str) -> String {
    match std::env::var("MDK_DRIVER_DIAL_HOST") {
        Ok(host) if !host.is_empty() => url
            .replacen("://127.0.0.1:", &format!("://{host}:"), 1)
            .replacen("://localhost:", &format!("://{host}:"), 1),
        _ => url.to_string(),
    }
}

/// Publishes `event` to `url`, answering one NIP-42 challenge with a fresh
/// key if the relay asks; the relay's verdict.
async fn publish(url: &str, event: &Event) -> Res<(bool, String)> {
    let (mut ws, _) = tokio::time::timeout(deadline(), tokio_tungstenite::connect_async(dial_url(url)))
        .await
        .map_err(|_| fail("transport", format!("{url}: connect timed out")))?
        .map_err(as_fail("transport", url))?;
    let frame = json!(["EVENT", event]).to_string();
    ws.send(WsMessage::Text(frame.clone().into())).await.map_err(as_fail("transport", url))?;
    let mut challenge: Option<String> = None;
    let mut authed = false;
    loop {
        let msg = tokio::time::timeout(deadline(), ws.next())
            .await
            .map_err(|_| fail("transport", format!("{url}: no answer before the deadline")))?
            .ok_or_else(|| fail("transport", format!("{url}: connection closed")))?
            .map_err(as_fail("transport", url))?;
        let WsMessage::Text(text) = msg else { continue };
        let frame_in: Value = serde_json::from_str(text.as_str()).map_err(as_fail("transport", url))?;
        match frame_in.get(0).and_then(Value::as_str) {
            Some("AUTH") => challenge = frame_in.get(1).and_then(Value::as_str).map(str::to_string),
            Some("OK") if frame_in.get(1).and_then(Value::as_str) == Some(event.id.to_hex().as_str()) => {
                let ok = frame_in.get(2).and_then(Value::as_bool).unwrap_or(false);
                let message = frame_in.get(3).and_then(Value::as_str).unwrap_or("").to_string();
                if ok || authed || !message.starts_with("auth-required") {
                    return Ok((ok, message));
                }
                let Some(c) = challenge.clone() else {
                    return Err(fail("transport", format!("{url}: auth-required without a challenge")));
                };
                let relay_url = RelayUrl::parse(url).map_err(as_fail("transport", url))?;
                let auth = EventBuilder::auth(c, relay_url)
                    .sign_with_keys(&Keys::generate())
                    .map_err(as_fail("internal", "auth"))?;
                ws.send(WsMessage::Text(json!(["AUTH", auth]).to_string().into()))
                    .await
                    .map_err(as_fail("transport", url))?;
                ws.send(WsMessage::Text(frame.clone().into())).await.map_err(as_fail("transport", url))?;
                authed = true;
            }
            _ => {}
        }
    }
}

/// MDK 0.9's marmot-app proof signer: BIP-340 over the v1 signing digest.
struct ProofSigner {
    keys: Keys,
}

impl AccountIdentityProofSigner for ProofSigner {
    fn sign_account_identity_proof(&self, request: &AccountIdentityProofRequest) -> Result<[u8; 64], String> {
        if self.keys.public_key().to_bytes().as_slice() != request.account_identity.as_slice() {
            return Err("proof request for another account".into());
        }
        let message = nostr::secp256k1::Message::from_digest(request.signing_digest());
        Ok(self.keys.sign_schnorr(&message).serialize())
    }
}

/// MDK 0.9's marmot-app supported component set (supported_app_component_ids):
/// advertised by its session and listed in its KeyPackage app_components tag.
fn app_components() -> Vec<u16> {
    let mut components = default_group_components();
    components.insert(NOSTR_ROUTING_COMPONENT_ID);
    components.insert(AGENT_TEXT_STREAM_QUIC_COMPONENT_ID);
    components.insert(GROUP_ENCRYPTED_MEDIA_COMPONENT_ID);
    components.into_iter().collect()
}

struct Peer {
    keys: Keys,
    session: AccountDeviceSession,
    slot: String,
}

struct Driver {
    dir: tempfile::TempDir,
    peers: HashMap<String, Peer>,
}

fn str_arg<'a>(req: &'a Value, key: &str) -> Res<&'a str> {
    req.get(key).and_then(Value::as_str).ok_or_else(|| fail("internal", format!("missing string '{key}'")))
}

impl Driver {
    async fn handle(&mut self, req: &Value) -> Res<Value> {
        match str_arg(req, "cmd")? {
            "hello" => {
                if let Some(wanted) = req.get("profiles").and_then(Value::as_array) {
                    if !wanted.iter().any(|p| p.as_str() == Some(PROFILE)) {
                        return Err(fail("unsupported", format!("this probe speaks only {PROFILE}, not {wanted:?}")));
                    }
                }
                Ok(json!({
                    "contract": 2,
                    "mdk": "mdk 0.9.0 (cgka-engine; KeyPackage probe)",
                    "mdk_rev": MDK_REV,
                    "profile": PROFILE,
                    "proof_extension": format!("0x{ACCOUNT_IDENTITY_PROOF_EXTENSION_TYPE:04x}"),
                    "commands": ["hello", "peer_new", "publish_key_package"],
                }))
            }
            "peer_new" => {
                let name = str_arg(req, "peer")?.to_string();
                let keys = Keys::parse(str_arg(req, "secret")?).map_err(as_fail("internal", "secret"))?;
                let db_key = hex::encode(Keys::generate().secret_key().to_secret_bytes());
                let config = SessionConfig::new(
                    self.dir.path().join(format!("{name}.sqlite")),
                    SqlCipherKey::new(&db_key).map_err(as_fail("internal", "sqlcipher key"))?,
                    keys.public_key().to_bytes().to_vec(),
                    Box::new(transport_nostr_peeler::NostrMlsPeeler::new().with_welcome_signer(keys.clone())),
                )
                .account_identity_proof_signer(Arc::new(ProofSigner { keys: keys.clone() }))
                .supported_app_components(app_components());
                let session = AccountDeviceSession::open(config).map_err(as_fail("internal", "session open"))?;
                let pubkey = keys.public_key().to_hex();
                let slot = hex::encode(Keys::generate().secret_key().to_secret_bytes());
                self.peers.insert(name, Peer { keys, session, slot });
                Ok(json!({ "pubkey": pubkey }))
            }
            "publish_key_package" => {
                let targets: Vec<String> = req
                    .get("to")
                    .and_then(Value::as_array)
                    .map(|a| a.iter().filter_map(|v| v.as_str().map(str::to_string)).collect())
                    .unwrap_or_default();
                let name = str_arg(req, "peer")?;
                let peer = self.peers.get_mut(name).ok_or_else(|| fail("internal", format!("no peer '{name}'")))?;
                let key_package = peer.session.fresh_key_package().await.map_err(as_fail("internal", "fresh_key_package"))?;
                let metadata = key_package_metadata(&key_package).map_err(as_fail("internal", "metadata"))?;
                // As MDK 0.9's marmot-app fills the publication.
                let publication = NostrKeyPackagePublication {
                    account_id: MemberId::new(peer.keys.public_key().to_bytes().to_vec()),
                    key_package,
                    key_package_slot_id: peer.slot.clone(),
                    key_package_ref: metadata.key_package_ref_hex.clone(),
                    mls_ciphersuite: "0x0001".into(),
                    mls_extensions: vec![
                        "0x0006".into(),
                        format!("0x{ACCOUNT_IDENTITY_PROOF_EXTENSION_TYPE:04x}"),
                        "0x000a".into(),
                    ],
                    mls_proposals: vec!["0x0008".into(), "0x000a".into()],
                    app_components: app_components().iter().map(|id| format!("0x{id:04x}")).collect(),
                    publish_endpoints: vec![TransportEndpoint("ws://127.0.0.1:1".into())],
                };
                let unsigned = publication.to_event().map_err(as_fail("internal", "KeyPackage event"))?;
                let tags: Vec<Tag> = unsigned
                    .tags
                    .iter()
                    .map(|t| Tag::parse(t.clone()).map_err(as_fail("internal", "tag")))
                    .collect::<Res<_>>()?;
                let event = EventBuilder::new(Kind::Custom(30443), unsigned.content.clone())
                    .tags(tags)
                    .custom_created_at(Timestamp::from_secs(unsigned.created_at))
                    .sign_with_keys(&peer.keys)
                    .map_err(as_fail("internal", "sign"))?;
                let mut accepted = targets.is_empty();
                let mut answers = serde_json::Map::new();
                for url in &targets {
                    let answer = match publish(url, &event).await {
                        Ok((ok, message)) => {
                            accepted |= ok;
                            json!({ "ok": ok, "message": message })
                        }
                        Err(e) => json!({ "ok": false, "message": e.error }),
                    };
                    answers.insert(url.clone(), answer);
                }
                if !accepted {
                    return Err(fail("transport", format!("no relay accepted the KeyPackage: {answers:?}")));
                }
                Ok(json!({ "event": event.as_json(), "event_id": event.id.to_hex(),
                           "key_package_ref": metadata.key_package_ref_hex }))
            }
            other => Err(fail("unsupported", format!("'{other}': the MDK 0.9.0 probe makes KeyPackages only"))),
        }
    }
}

#[tokio::main(flavor = "current_thread")]
async fn main() {
    let filter = tracing_subscriber::EnvFilter::try_from_env("MDK_DRIVER_LOG")
        .unwrap_or_else(|_| tracing_subscriber::EnvFilter::new("warn"));
    tracing_subscriber::fmt().with_env_filter(filter).with_writer(std::io::stderr).with_ansi(false).init();
    let dir = tempfile::tempdir().expect("a temporary directory");
    let mut driver = Driver { dir, peers: HashMap::new() };
    let mut lines = BufReader::new(tokio::io::stdin()).lines();
    let mut stdout = tokio::io::stdout();
    while let Ok(Some(line)) = lines.next_line().await {
        if line.trim().is_empty() {
            continue;
        }
        let Ok(req) = serde_json::from_str::<Value>(&line) else {
            eprintln!("mdk09-probe: bad request line");
            continue;
        };
        let id = req.get("id").cloned().unwrap_or(Value::Null);
        let answer = match driver.handle(&req).await {
            Ok(mut value) => {
                value["id"] = id;
                value["ok"] = json!(true);
                value
            }
            Err(e) => json!({ "id": id, "ok": false, "class": e.class, "error": e.error }),
        };
        let text = format!("{answer}\n");
        if stdout.write_all(text.as_bytes()).await.is_err() || stdout.flush().await.is_err() {
            break;
        }
    }
}
