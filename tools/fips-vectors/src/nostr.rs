//! Immutable captures from the root fips-mesh crate, not fips-core.
//! The private cfg(test) fixture builders cannot be called by a dependency.
//! We verify/open their verbatim captures with nostr 0.44 and parse their
//! payloads with the real, pinned fips-mesh types instead of emulating them.
use crate::{Exporter, Result};
use fips::nostr::{OverlayAdvert, TraversalAnswer, TraversalOffer};
use nostr::prelude::{Event, JsonUtil, Keys, SecretKey, UnsignedEvent};
use serde_json::{Value, json};

const A: &str = "0000000000000000000000000000000000000000000000000000000000000003";
const B: &str = "b7e151628aed2a6abf7158809cf4f3c762e7160f38b4da56a784d9045190cfef";
const FIXTURES: [(&str, &str, &str, Option<&str>); 4] = [
    (
        "advert",
        include_str!("../fixtures/nostr/v044/advert.json"),
        A,
        None,
    ),
    (
        "inbox-list",
        include_str!("../fixtures/nostr/v044/inbox-list.json"),
        A,
        None,
    ),
    (
        "offer-wrap",
        include_str!("../fixtures/nostr/v044/offer-wrap.json"),
        A,
        Some(B),
    ),
    (
        "answer-wrap",
        include_str!("../fixtures/nostr/v044/answer-wrap.json"),
        B,
        Some(A),
    ),
];
fn keys(secret: &str) -> Keys {
    Keys::new(SecretKey::from_hex(secret).expect("fixture secret"))
}
pub fn export(ex: &Exporter<'_>) -> Result<()> {
    let mut cases = Vec::new();
    for (name, source, signing_key, recipient_key) in FIXTURES {
        let held: Value = serde_json::from_str(source)?;
        let event = Event::from_json(held["event"].to_string())?;
        event.verify()?;
        let event_json = event.as_json();
        // These exact bytes were serialized by the upstream capture, not by a
        // second builder with a new nonce/key/signature/clock reading.
        assert!(source.contains(&event_json));
        let mut case = json!({
            "name": name, "capture_unix_ms": held["captureUnixMs"],
            "created_at_secs": event.created_at.as_secs(),
            "kind": event.kind.as_u16(), "signing_key_hex": signing_key,
            "recipient_key_hex": recipient_key,
            "source": format!("src/nostr/fixtures/v044/{name}.json:1-4"),
            "contract": "src/nostr/tests/fixtures.rs:27-331",
            "capture": ex.bytes(&format!("{name}-capture"), source.as_bytes())?,
            "event": ex.bytes(name, event_json.as_bytes())?,
        });
        match recipient_key {
            Some(recipient) => {
                let to = keys(recipient);
                let seal = Event::from_json(nostr::nips::nip44::decrypt(
                    to.secret_key(),
                    &event.pubkey,
                    &event.content,
                )?)?;
                seal.verify()?;
                assert_eq!(seal.pubkey, keys(signing_key).public_key());
                let rumor = UnsignedEvent::from_json(nostr::nips::nip44::decrypt(
                    to.secret_key(),
                    &seal.pubkey,
                    &seal.content,
                )?)?;
                assert_eq!(rumor.pubkey, seal.pubkey);
                let canonical = if name == "offer-wrap" {
                    serde_json::to_string(&serde_json::from_str::<TraversalOffer>(&rumor.content)?)?
                } else {
                    serde_json::to_string(&serde_json::from_str::<TraversalAnswer>(
                        &rumor.content,
                    )?)?
                };
                assert_eq!(canonical, rumor.content);
                case["decoded"] = serde_json::from_str(&canonical)?;
                case["content"] = ex.bytes(&format!("{name}-content"), canonical.as_bytes())?;
                case["sender_pubkey_hex"] = json!(seal.pubkey.to_hex());
            }
            None if name == "advert" => {
                let typed: OverlayAdvert = serde_json::from_str(&event.content)?;
                let canonical = serde_json::to_string(&typed)?;
                assert_eq!(canonical, event.content);
                case["decoded"] = serde_json::to_value(typed)?;
                case["content"] = ex.bytes("advert-content", canonical.as_bytes())?;
            }
            None => {
                let relays: Vec<String> = held["event"]["tags"]
                    .as_array()
                    .unwrap()
                    .iter()
                    .filter(|t| t[0] == "r" || t[0] == "relay")
                    .map(|t| {
                        nostr::RelayUrl::parse(t[1].as_str().unwrap())
                            .unwrap()
                            .to_string()
                    })
                    .collect();
                case["decoded"] = json!({"relays": relays});
                case["relay_slash_equal"] = json!(
                    nostr::RelayUrl::parse("wss://example.com").unwrap()
                        == nostr::RelayUrl::parse("wss://example.com/").unwrap()
                );
                case["relay_normalization"] = json!(
                    [
                        "wss://EXAMPLE.com:443/",
                        "ws://EXAMPLE.com:80",
                        "wss://example.com/Path/",
                        "wss://example.com/?q=1",
                        "https://not-a-relay.example/",
                        "wss://[::1]:443/"
                    ]
                    .map(|input| json!({"input": input,
                    "expected": nostr::RelayUrl::parse(input).ok().map(|u| u.to_string())}))
                );
            }
        }
        cases.push(case);
    }
    ex.finish(json!(cases), &[
        "Verbatim signed captures: private cfg(test) builders are not callable; no recapture or signature regeneration is claimed.",
        "fips-mesh parses advert/offer/answer; nostr 0.44 verifies signatures and opens both NIP-44 layers.",
    ])
}
