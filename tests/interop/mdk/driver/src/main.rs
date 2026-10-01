//! MDK interop driver: one or more MDK 0.8 peers (mdk-core, the legacy 0xF2EE
//! Marmot profile) behind a JSON-lines protocol, talking to real Nostr relays.
//! nostrc's C interop test (gnome/groundhog/tests/mls/test_mdk_interop.c)
//! starts it, usually as a Docker container, and drives it on stdin/stdout;
//! the relays are the test's local relays.
//!
//! Protocol: one JSON object per line on stdin, `{"id": N, "cmd": "...", ...}`;
//! exactly one answer line per request on stdout, `{"id": N, "ok": true, ...}`
//! or `{"id": N, "ok": false, "error": "..."}`, in order. Logs go to stderr.
//! Every relay wait is bounded (MDK_DRIVER_DEADLINE_S, default 30 s) and turns
//! a hang into an error; nothing sleeps. MDK's tracing goes to stderr, filtered
//! by MDK_DRIVER_LOG (an EnvFilter, default "warn"). See ../README.md for the commands.
//!
//! Relays: one WebSocket per operation. A relay that asks for NIP-42 AUTH
//! (CLOSED or OK false "auth-required:") gets it and the request is sent again:
//! as the peer's account for its inbox (NIP-17 serves gift wraps only to their
//! recipient), as a fresh key everywhere else (Marmot group relays see no
//! account). MDK_DRIVER_DIAL_HOST, when set, is the host dialled instead of
//! 127.0.0.1/localhost (Docker Desktop: host.docker.internal); the URL itself,
//! and so the AUTH `relay` tag, is unchanged.

use std::collections::{BTreeMap, HashMap, HashSet};
use std::time::Duration;

use base64::Engine as _;
use futures_util::{SinkExt, StreamExt};
use mdk_core::prelude::*;
use mdk_memory_storage::MdkMemoryStorage;
use nostr::nips::nip59::UnwrappedGift;
use nostr::prelude::*;
use serde_json::{Value, json};
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio::net::TcpStream;
use tokio_tungstenite::tungstenite::Message as WsMessage;
use tokio_tungstenite::{MaybeTlsStream, WebSocketStream};
use openmls::prelude::OpenMlsProvider as _;
use tls_codec::Deserialize as _;

type Res<T> = Result<T, String>;

fn err<E: std::fmt::Display>(what: &str) -> impl FnOnce(E) -> String + '_ {
    move |e| format!("{what}: {e}")
}

fn deadline() -> Duration {
    let secs = std::env::var("MDK_DRIVER_DEADLINE_S")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(30);
    Duration::from_secs(secs)
}

// ---- relay client ---------------------------------------------------------------

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
            .map_err(|_| format!("{url}: connect timed out"))?
            .map_err(err(url))?;
        Ok(Self { url: url.to_string(), ws, challenge: None, authed: false, queued: Vec::new() })
    }

    async fn send(&mut self, frame: Value) -> Res<()> {
        self.ws.send(WsMessage::Text(frame.to_string().into())).await.map_err(err(&self.url))
    }

    /// The next relay frame (a JSON array) other than AUTH, which only records
    /// the challenge.
    async fn next(&mut self) -> Res<Value> {
        if !self.queued.is_empty() {
            return Ok(self.queued.remove(0));
        }
        loop {
            let msg = tokio::time::timeout(deadline(), self.ws.next())
                .await
                .map_err(|_| format!("{}: no answer before the deadline", self.url))?
                .ok_or_else(|| format!("{}: connection closed", self.url))?
                .map_err(err(&self.url))?;
            let WsMessage::Text(text) = msg else { continue };
            let frame: Value = serde_json::from_str(text.as_str()).map_err(err(&self.url))?;
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
        let relay_url = RelayUrl::parse(&self.url).map_err(err(&self.url))?;
        let auth = EventBuilder::auth(challenge, relay_url).sign_with_keys(keys).map_err(err("auth"))?;
        let id = auth.id.to_hex();
        self.send(json!(["AUTH", serde_json::to_value(&auth).map_err(err("auth"))?])).await?;
        let (ok, message) = self.wait_ok(&id).await?;
        if !ok {
            return Err(format!("{}: AUTH refused: {message}", self.url));
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
        let frame = json!(["EVENT", serde_json::to_value(event).map_err(err("event"))?]);
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
            let sub = format!("mdk-{}", hex::encode(rand_bytes::<6>()));
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
                        let event: Event = serde_json::from_value(raw).map_err(err("event"))?;
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
                        return Err(format!("{}: REQ closed: {message}", self.url));
                    }
                    _ => {}
                }
            }
        }
    }
}

fn rand_bytes<const N: usize>() -> [u8; N] {
    let keys = Keys::generate();
    let mut out = [0u8; N];
    out.copy_from_slice(&keys.secret_key().to_secret_bytes()[..N]);
    out
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
            Err(e) => json!({ "ok": false, "message": e }),
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

// ---- peers ------------------------------------------------------------------------

struct Peer {
    keys: Keys,
    mdk: MDK<MdkMemoryStorage>,
    /// Group events already processed or published by this peer.
    seen: HashSet<EventId>,
    /// Welcomes processed, by gift-wrap id, until accepted.
    welcomes: HashMap<String, welcome_types::Welcome>,
    /// Wraps already opened (and their outcome), so a refetch is a no-op.
    wraps: HashSet<EventId>,
}

struct Driver {
    peers: HashMap<String, Peer>,
}

fn str_arg<'a>(req: &'a Value, key: &str) -> Res<&'a str> {
    req.get(key).and_then(Value::as_str).ok_or_else(|| format!("missing string '{key}'"))
}

fn strs_arg(req: &Value, key: &str) -> Res<Vec<String>> {
    Ok(req
        .get(key)
        .and_then(Value::as_array)
        .ok_or_else(|| format!("missing array '{key}'"))?
        .iter()
        .filter_map(|v| v.as_str().map(str::to_string))
        .collect())
}

fn opt_strs(req: &Value, key: &str) -> Option<Vec<String>> {
    req.get(key).and_then(Value::as_array).map(|a| {
        a.iter().filter_map(|v| v.as_str().map(str::to_string)).collect()
    })
}

fn relay_urls(urls: &[String]) -> Res<Vec<RelayUrl>> {
    urls.iter().map(|u| RelayUrl::parse(u).map_err(err(u))).collect()
}

fn pubkeys(hexes: &[String]) -> Res<Vec<PublicKey>> {
    hexes.iter().map(|h| PublicKey::from_hex(h).map_err(err(h))).collect()
}

fn hex_array<const N: usize>(obj: &Value, key: &str) -> Res<[u8; N]> {
    let bytes = hex::decode(str_arg(obj, key)?).map_err(err(key))?;
    bytes.try_into().map_err(|_| format!("'{key}' must be {N} bytes"))
}

fn group_id_arg(req: &Value) -> Res<GroupId> {
    let bytes = hex::decode(str_arg(req, "group")?).map_err(err("group"))?;
    Ok(GroupId::from_slice(&bytes))
}

fn event_arg(value: &Value) -> Res<Event> {
    let event: Event = match value {
        Value::String(s) => Event::from_json(s).map_err(err("event json"))?,
        other => serde_json::from_value(other.clone()).map_err(err("event"))?,
    };
    event.verify().map_err(err("event signature"))?;
    Ok(event)
}

fn ext_hex(t: openmls::prelude::ExtensionType) -> String {
    format!("0x{:04x}", u16::from(t))
}

/// What MDK makes of a kind:30443 event: parse_key_package's verdict and, when
/// it parses, the leaf's extensions and capabilities as MDK/OpenMLS read them.
fn describe_key_package(mdk: &MDK<MdkMemoryStorage>, event: &Event) -> Value {
    match mdk.parse_key_package(event) {
        Ok(kp) => {
            let leaf = kp.leaf_node();
            let leaf_extensions: Vec<String> =
                leaf.extensions().iter().map(|e| ext_hex(e.extension_type())).collect();
            let capability_extensions: Vec<String> =
                leaf.capabilities().extensions().iter().map(|t| ext_hex(*t)).collect();
            let proposals: Vec<String> = leaf
                .capabilities()
                .proposals()
                .iter()
                .map(|p| format!("0x{:04x}", u16::from(*p)))
                .collect();
            let kp_extensions: Vec<String> =
                kp.extensions().iter().map(|e| ext_hex(e.extension_type())).collect();
            json!({
                "parsed": true,
                "leaf_extensions": leaf_extensions,
                "capability_extensions": capability_extensions,
                "capability_proposals": proposals,
                "key_package_extensions": kp_extensions,
                "last_resort": kp.last_resort(),
            })
        }
        Err(e) => json!({ "parsed": false, "error": e.to_string(), "error_debug": format!("{e:?}") }),
    }
}

fn group_state(peer: &Peer, group_id: &GroupId) -> Res<Value> {
    let group = peer
        .mdk
        .get_group(group_id)
        .map_err(err("get_group"))?
        .ok_or("no such group")?;
    let mut members: Vec<String> = peer
        .mdk
        .get_members(group_id)
        .map_err(err("get_members"))?
        .iter()
        .map(|p| p.to_hex())
        .collect();
    members.sort();
    let mut admins: Vec<String> = group.admin_pubkeys.iter().map(|p| p.to_hex()).collect();
    admins.sort();
    let relays: Vec<String> = peer
        .mdk
        .get_relays(group_id)
        .map_err(err("get_relays"))?
        .iter()
        .map(|r| r.to_string())
        .collect();
    Ok(json!({
        "group": hex::encode(group.mls_group_id.as_slice()),
        "nostr_group_id": hex::encode(group.nostr_group_id),
        "name": group.name,
        "description": group.description,
        "epoch": group.epoch,
        "state": format!("{:?}", group.state),
        "members": members,
        "admins": admins,
        "relays": relays,
    }))
}

impl Driver {
    fn peer(&mut self, req: &Value) -> Res<&mut Peer> {
        let name = str_arg(req, "peer")?;
        self.peers.get_mut(name).ok_or_else(|| format!("no peer '{name}'"))
    }

    async fn handle(&mut self, req: &Value) -> Res<Value> {
        let cmd = str_arg(req, "cmd")?;
        match cmd {
            "hello" => Ok(json!({
                "mdk": "mdk-core 0.8.0",
                "mdk_rev": "575ae29d25d58494135058d9e46affec7174e79a",
                "openmls_rev": "04c50d7fb12d52f4f9aee26de5f5234f3df29fa8",
            })),
            "peer_new" => {
                let name = str_arg(req, "peer")?.to_string();
                let keys = Keys::parse(str_arg(req, "secret")?).map_err(err("secret"))?;
                let pubkey = keys.public_key().to_hex();
                let peer = Peer {
                    keys,
                    mdk: MDK::new(MdkMemoryStorage::default()),
                    seen: HashSet::new(),
                    welcomes: HashMap::new(),
                    wraps: HashSet::new(),
                };
                self.peers.insert(name, peer);
                Ok(json!({ "pubkey": pubkey }))
            }
            "publish_key_package" => {
                let tag_relays = strs_arg(req, "relays")?;
                let targets = strs_arg(req, "to")?;
                let peer = self.peer(req)?;
                let data = peer
                    .mdk
                    .create_key_package_for_event(&peer.keys.public_key(), relay_urls(&tag_relays)?)
                    .map_err(err("create_key_package_for_event"))?;
                let event = EventBuilder::new(Kind::Custom(30443), data.content)
                    .tags(data.tags_30443)
                    .sign_with_keys(&peer.keys)
                    .map_err(err("sign"))?;
                // "to": [] makes the KeyPackage without publishing it (vectors).
                let (accepted, relays) = publish_all(&targets, &event, &Keys::generate()).await;
                if !accepted && !targets.is_empty() {
                    return Err(format!("no relay accepted the KeyPackage: {relays}"));
                }
                Ok(json!({ "event": event.as_json(), "event_id": event.id.to_hex(), "relays": relays }))
            }
            "fetch_key_package" => {
                let author = str_arg(req, "author")?;
                let from = strs_arg(req, "from")?;
                let filter = json!({ "kinds": [30443], "authors": [author] });
                let events = fetch_all(&from, &filter, &Keys::generate()).await?;
                let newest = events
                    .into_iter()
                    .max_by_key(|e| (e.created_at, e.id))
                    .ok_or("no KeyPackage found")?;
                let peer = self.peer(req)?;
                let mdk_view = describe_key_package(&peer.mdk, &newest);
                Ok(json!({ "event": newest.as_json(), "mdk": mdk_view }))
            }
            "parse_key_package" => {
                let event = event_arg(req.get("event").ok_or("missing 'event'")?)?;
                let peer = self.peer(req)?;
                Ok(describe_key_package(&peer.mdk, &event))
            }
            "create_group" => self.create_group(req).await,
            "add_members" => {
                let group_id = group_id_arg(req)?;
                let kps: Vec<Event> = req
                    .get("key_packages")
                    .and_then(Value::as_array)
                    .ok_or("missing 'key_packages'")?
                    .iter()
                    .map(event_arg)
                    .collect::<Res<_>>()?;
                let welcome_relays = strs_arg(req, "welcome_relays")?;
                let peer = self.peer(req)?;
                let result = peer.mdk.add_members(&group_id, &kps).map_err(err("add_members"))?;
                let mut out = commit_and_merge(peer, &group_id, &result.evolution_event).await?;
                let rumors = result.welcome_rumors.unwrap_or_default();
                out["welcomes"] = send_welcomes(peer, rumors, &kps, &welcome_relays).await?;
                Ok(out)
            }
            "remove_members" => {
                let group_id = group_id_arg(req)?;
                let members = pubkeys(&strs_arg(req, "members")?)?;
                let peer = self.peer(req)?;
                let result =
                    peer.mdk.remove_members(&group_id, &members).map_err(err("remove_members"))?;
                commit_and_merge(peer, &group_id, &result.evolution_event).await
            }
            "update_group_data" => {
                let group_id = group_id_arg(req)?;
                let mut update = NostrGroupDataUpdate::new();
                update.name = req.get("name").and_then(Value::as_str).map(str::to_string);
                update.description =
                    req.get("description").and_then(Value::as_str).map(str::to_string);
                if let Some(admins) = opt_strs(req, "admins") {
                    update.admins = Some(pubkeys(&admins)?);
                }
                if let Some(image) = req.get("image") {
                    // {"hash","key","nonce","upload_key"}: hex (a vector with an image).
                    update.image_hash = Some(Some(hex_array(image, "hash")?));
                    update.image_key = Some(Some(hex_array(image, "key")?));
                    update.image_nonce = Some(Some(hex_array(image, "nonce")?));
                    update.image_upload_key = Some(Some(hex_array(image, "upload_key")?));
                }
                let peer = self.peer(req)?;
                let result =
                    peer.mdk.update_group_data(&group_id, update).map_err(err("update_group_data"))?;
                if req.get("publish").and_then(Value::as_bool) == Some(false) {
                    // Vector capture only: merged locally, never published.
                    peer.mdk.merge_pending_commit(&group_id).map_err(err("merge_pending_commit"))?;
                    return group_state(peer, &group_id);
                }
                commit_and_merge(peer, &group_id, &result.evolution_event).await
            }
            "self_update" => {
                let group_id = group_id_arg(req)?;
                let peer = self.peer(req)?;
                let result = peer.mdk.self_update(&group_id).map_err(err("self_update"))?;
                commit_and_merge(peer, &group_id, &result.evolution_event).await
            }
            "send" => {
                let group_id = group_id_arg(req)?;
                let text = str_arg(req, "text")?.to_string();
                let peer = self.peer(req)?;
                let rumor = EventBuilder::new(Kind::Custom(9), text).build(peer.keys.public_key());
                let event =
                    peer.mdk.create_message(&group_id, rumor, None).map_err(err("create_message"))?;
                peer.seen.insert(event.id);
                if req.get("publish").and_then(Value::as_bool) == Some(false) {
                    // Vector capture only: the kind:445 itself, never published.
                    return Ok(json!({ "event": event.as_json(), "event_id": event.id.to_hex() }));
                }
                let relays = group_relays(peer, &group_id)?;
                let (accepted, answers) = publish_all(&relays, &event, &Keys::generate()).await;
                if !accepted {
                    return Err(format!("no group relay accepted the message: {answers}"));
                }
                Ok(json!({ "event_id": event.id.to_hex() }))
            }
            "sync" => self.sync(req).await,
            "fetch_welcomes" => self.fetch_welcomes(req).await,
            "accept_welcome" => {
                let wrapper = str_arg(req, "wrapper_id")?.to_string();
                let peer = self.peer(req)?;
                let welcome = peer.welcomes.remove(&wrapper).ok_or("no such pending welcome")?;
                peer.mdk.accept_welcome(&welcome).map_err(err("accept_welcome"))?;
                group_state(peer, &welcome.mls_group_id)
            }
            "state" => {
                let group_id = group_id_arg(req)?;
                let peer = self.peer(req)?;
                group_state(peer, &group_id)
            }
            "leave_group" => {
                // MIP-03 "Leaving a group": MDK's leave_group() -- a SelfRemove
                // PublicMessage where the group requires SelfRemove, else a Remove of
                // itself -- published to the group relays for another member to
                // commit; a proposal, so nothing is merged. Also returns the
                // MLSMessage and OpenMLS's own ProposalRef for it (vector capture).
                let group_id = group_id_arg(req)?;
                let peer = self.peer(req)?;
                let result = peer.mdk.leave_group(&group_id).map_err(err("leave_group"))?;
                let event = result.evolution_event;
                peer.seen.insert(event.id);
                let group = peer
                    .mdk
                    .load_mls_group(&group_id)
                    .map_err(err("load_mls_group"))?
                    .ok_or("no such group")?;
                let message = open_current(peer, &group, &event)?;
                let refs: Vec<String> = group
                    .pending_proposals()
                    .map(|p| hex::encode(p.proposal_reference_ref().as_slice()))
                    .collect();
                let relays = group_relays(peer, &group_id)?;
                let (accepted, answers) = publish_all(&relays, &event, &Keys::generate()).await;
                if !accepted {
                    return Err(format!("no group relay accepted the leave: {answers}"));
                }
                let mut state = group_state(peer, &group_id)?;
                state["event_id"] = json!(event.id.to_hex());
                state["mls_message"] = json!(hex::encode(message));
                state["proposal_refs"] = json!(refs);
                Ok(state)
            }
            "export_secret" => {
                // MLS-Exporter(label, context, length) of the group's current epoch
                // (RFC 9420 section 8.5), e.g. MIP-03's ("marmot", "group-event", 32).
                let group_id = group_id_arg(req)?;
                let label = str_arg(req, "label")?.to_string();
                let context = str_arg(req, "context")?.as_bytes().to_vec();
                let length = req.get("length").and_then(Value::as_u64).unwrap_or(32) as usize;
                let peer = self.peer(req)?;
                let group = peer
                    .mdk
                    .load_mls_group(&group_id)
                    .map_err(err("load_mls_group"))?
                    .ok_or("no such group")?;
                let secret = group
                    .export_secret(peer.mdk.provider.crypto(), &label, &context, length)
                    .map_err(err("export_secret"))?;
                Ok(json!({ "hex": hex::encode(secret), "epoch": group.epoch().as_u64() }))
            }
            "group_extension" => {
                // The group's marmot_group_data (0xF2EE) exactly as MDK encodes it in
                // the GroupContext (MIP-01): a cross-implementation vector.
                let group_id = group_id_arg(req)?;
                let peer = self.peer(req)?;
                let group = peer
                    .mdk
                    .load_mls_group(&group_id)
                    .map_err(err("load_mls_group"))?
                    .ok_or("no such group")?;
                let data = group
                    .extensions()
                    .iter()
                    .find_map(|e| match e {
                        openmls::prelude::Extension::Unknown(0xF2EE, u) => Some(u.0.clone()),
                        _ => None,
                    })
                    .ok_or("no 0xF2EE extension")?;
                Ok(json!({ "hex": hex::encode(data) }))
            }
            "messages" => {
                let group_id = group_id_arg(req)?;
                let peer = self.peer(req)?;
                let messages = peer.mdk.get_messages(&group_id, None).map_err(err("get_messages"))?;
                let list: Vec<Value> = messages
                    .iter()
                    .map(|m| json!({ "id": m.id.to_hex(), "author": m.pubkey.to_hex(),
                                     "kind": m.kind.as_u16(), "content": m.content }))
                    .collect();
                Ok(json!({ "messages": list }))
            }
            "welcome_bytes" => {
                // Base64 TLS Welcome from a kind:444 rumor, decoded as OpenMLS
                // reads it (diagnostics only).
                let content = str_arg(req, "content")?;
                let bytes = base64::engine::general_purpose::STANDARD
                    .decode(content)
                    .map_err(err("base64"))?;
                let parsed = openmls::prelude::MlsMessageIn::tls_deserialize_exact(&bytes);
                Ok(json!({ "len": bytes.len(), "parses": parsed.is_ok(),
                           "error": parsed.err().map(|e| e.to_string()) }))
            }
            other => Err(format!("unknown command '{other}'")),
        }
    }

    async fn create_group(&mut self, req: &Value) -> Res<Value> {
        let name = str_arg(req, "name")?.to_string();
        let description = req.get("description").and_then(Value::as_str).unwrap_or("").to_string();
        let relays = strs_arg(req, "relays")?;
        let admins = pubkeys(&strs_arg(req, "admins")?)?;
        let kps: Vec<Event> = req
            .get("key_packages")
            .and_then(Value::as_array)
            .ok_or("missing 'key_packages'")?
            .iter()
            .map(event_arg)
            .collect::<Res<_>>()?;
        let welcome_relays = strs_arg(req, "welcome_relays")?;
        let (image_hash, image_key, image_nonce) = match req.get("image") {
            Some(image) => (
                Some(hex_array(image, "hash")?),
                Some(hex_array(image, "key")?),
                Some(hex_array(image, "nonce")?),
            ),
            None => (None, None, None),
        };
        let peer = self.peer(req)?;
        let config = NostrGroupConfigData::new(
            name,
            description,
            image_hash,
            image_key,
            image_nonce,
            relay_urls(&relays)?,
            admins,
        );
        let result = peer
            .mdk
            .create_group(&peer.keys.public_key(), kps.clone(), config)
            .map_err(err("create_group"))?;
        let group_id = result.group.mls_group_id.clone();
        let welcomes = send_welcomes(peer, result.welcome_rumors, &kps, &welcome_relays).await?;
        let mut state = group_state(peer, &group_id)?;
        state["welcomes"] = welcomes;
        Ok(state)
    }

    /// Every kind:445 of the group on its relays, through process_message, as a
    /// careful client feeds them: oldest first, and only events of the current
    /// epoch, until none applies; then the rest, for MDK's verdict. MDK 0.8
    /// fails an event of a later epoch for good (it never retries a Failed
    /// event id), and two Commits in one second have no created_at order: fed
    /// in id order, the later epoch's could come first and strand the peer.
    async fn sync(&mut self, req: &Value) -> Res<Value> {
        let group_id = group_id_arg(req)?;
        let peer = self.peer(req)?;
        let group = peer.mdk.get_group(&group_id).map_err(err("get_group"))?.ok_or("no such group")?;
        let relays = group_relays(peer, &group_id)?;
        let filter = json!({ "kinds": [445], "#h": [hex::encode(group.nostr_group_id)] });
        let mut pending: Vec<Event> = fetch_all(&relays, &filter, &Keys::generate())
            .await?
            .into_iter()
            .filter(|e| !peer.seen.contains(&e.id))
            .collect();
        pending.sort_by_key(|e| (e.created_at, e.id));
        let mut results = Vec::new();
        let mut last_error: BTreeMap<String, String> = BTreeMap::new();
        let mut last_pass = false;
        loop {
            let mut progress = false;
            let mut still = Vec::new();
            for event in pending {
                let id = event.id.to_hex();
                if !last_pass && !of_current_epoch(peer, &group_id, &event) {
                    still.push(event);
                    continue;
                }
                match peer.mdk.process_message(&event) {
                    Ok(MessageProcessingResult::Unprocessable { .. }) => {
                        last_error.insert(id, "Unprocessable".into());
                        still.push(event);
                    }
                    Ok(MessageProcessingResult::Proposal(update)) => {
                        // MDK auto-committed a member's leave (a SelfRemove, from any
                        // member; or, as an admin, a Remove the member sent for itself):
                        // a careful client publishes that Commit and merges it only once
                        // a relay took it (MIP-03), as commit_and_merge() does.
                        peer.seen.insert(event.id);
                        last_error.remove(&id);
                        progress = true;
                        let committed =
                            commit_and_merge(peer, &group_id, &update.evolution_event).await;
                        results.push(json!({
                            "id": id, "type": "proposal",
                            "auto_commit": update.evolution_event.id.to_hex(),
                            "auto_commit_error": committed.err(),
                        }));
                    }
                    Ok(result) => {
                        peer.seen.insert(event.id);
                        last_error.remove(&id);
                        progress = true;
                        results.push(describe_result(&id, result));
                    }
                    Err(e) => {
                        last_error.insert(id, e.to_string());
                        still.push(event);
                    }
                }
            }
            pending = still;
            if pending.is_empty() || (last_pass && !progress) {
                break;
            }
            if !progress {
                last_pass = true;   // the rest: late, foreign or undecryptable
            }
        }
        let failed: Vec<Value> = pending
            .iter()
            .map(|e| {
                let id = e.id.to_hex();
                json!({ "id": id, "error": last_error.get(&id).cloned().unwrap_or_default(),
                        "openmls": diagnose(peer, &group_id, e) })
            })
            .collect();
        let state = group_state(peer, &group_id)?;
        Ok(json!({ "results": results, "failed": failed, "state": state }))
    }

    async fn fetch_welcomes(&mut self, req: &Value) -> Res<Value> {
        let from = strs_arg(req, "from")?;
        let peer = self.peer(req)?;
        let me = peer.keys.public_key().to_hex();
        let filter = json!({ "kinds": [1059], "#p": [me] });
        let wraps = fetch_all(&from, &filter, &peer.keys).await?;
        let mut out = Vec::new();
        for wrap in wraps {
            if !peer.wraps.insert(wrap.id) {
                continue;
            }
            let wrapper_id = wrap.id.to_hex();
            let unwrapped = match UnwrappedGift::from_gift_wrap(&peer.keys, &wrap).await {
                Ok(u) => u,
                Err(e) => {
                    out.push(json!({ "wrapper_id": wrapper_id, "ok": false,
                                     "error": format!("unwrap: {e}") }));
                    continue;
                }
            };
            if unwrapped.rumor.kind != Kind::MlsWelcome {
                continue;
            }
            let sender = unwrapped.sender.to_hex();
            match peer.mdk.process_welcome(&wrap.id, &unwrapped.rumor) {
                Ok(welcome) => {
                    let entry = json!({
                        "wrapper_id": wrapper_id, "ok": true, "sender": sender,
                        "welcomer": welcome.welcomer.to_hex(),
                        "group": hex::encode(welcome.mls_group_id.as_slice()),
                        "group_name": welcome.group_name,
                        "member_count": welcome.member_count,
                    });
                    peer.welcomes.insert(wrapper_id, welcome);
                    out.push(entry);
                }
                Err(e) => out.push(json!({
                    "wrapper_id": wrapper_id, "ok": false, "sender": sender,
                    "error": e.to_string(), "error_debug": format!("{e:?}"),
                    // What MIP-02's tag rules judged (validate_welcome_event).
                    "rumor_kind": unwrapped.rumor.kind.as_u16(),
                    "rumor_tags": serde_json::to_value(&unwrapped.rumor.tags)
                        .unwrap_or(Value::Null),
                })),
            }
        }
        Ok(json!({ "welcomes": out }))
    }
}

/// Why an event MDK could not process is refused, in OpenMLS's words (MDK
/// logs only "Error processing MLS message"): its content opened with the
/// group's current MIP-03 key, MLS-Exporter("marmot", "group-event", 32),
/// and handed to OpenMLS's process_message on a freshly loaded copy of the
/// group. Diagnostics only: nothing is merged.
fn diagnose(peer: &Peer, group_id: &GroupId, event: &Event) -> String {
    let mut group = match peer.mdk.load_mls_group(group_id) {
        Ok(Some(group)) => group,
        _ => return "no MLS group".into(),
    };
    let plain = match open_current(peer, &group, event) {
        Ok(plain) => plain,
        Err(e) => return e,
    };
    let message = match openmls::prelude::MlsMessageIn::tls_deserialize_exact(&plain) {
        Ok(message) => message,
        Err(e) => return format!("MLSMessage: {e}"),
    };
    let protocol = match message.try_into_protocol_message() {
        Ok(protocol) => protocol,
        Err(e) => return format!("not a protocol message: {e:?}"),
    };
    let what = format!("epoch {} {:?}", protocol.epoch().as_u64(), protocol.content_type());
    match group.process_message(&peer.mdk.provider, protocol) {
        Ok(_) => format!("{what}: OpenMLS accepts it"),
        Err(e) => format!("{what}: OpenMLS: {e:?}"),
    }
}

/// The MLSMessage of a kind:445 sealed with `group`'s current epoch key
/// (MIP-03: base64(nonce || ChaCha20-Poly1305(MLS-Exporter("marmot",
/// "group-event", 32), nonce, msg, ""))), or why not.
fn open_current(
    peer: &Peer,
    group: &openmls::prelude::MlsGroup,
    event: &Event,
) -> Result<Vec<u8>, String> {
    use chacha20poly1305::aead::{Aead, KeyInit};
    use chacha20poly1305::{ChaCha20Poly1305, Nonce};
    let epoch = group.epoch().as_u64();
    let key = group
        .export_secret(peer.mdk.provider.crypto(), "marmot", b"group-event", 32)
        .map_err(|e| format!("export_secret: {e}"))?;
    let combined = match base64::engine::general_purpose::STANDARD.decode(&event.content) {
        Ok(bytes) if bytes.len() >= 28 => bytes,
        _ => return Err("content is not MIP-03 (base64 nonce || ciphertext)".into()),
    };
    let cipher = ChaCha20Poly1305::new_from_slice(&key).map_err(|_| "cipher".to_string())?;
    cipher
        .decrypt(Nonce::from_slice(&combined[..12]), &combined[12..])
        .map_err(|_| format!("not sealed with the key of epoch {epoch}"))
}

/// Whether a kind:445 belongs to the group's current epoch (see sync()).
fn of_current_epoch(peer: &Peer, group_id: &GroupId, event: &Event) -> bool {
    match peer.mdk.load_mls_group(group_id) {
        Ok(Some(group)) => open_current(peer, &group, event).is_ok(),
        _ => false,
    }
}

fn describe_result(id: &str, result: MessageProcessingResult) -> Value {
    match result {
        MessageProcessingResult::ApplicationMessage(m) => json!({
            "id": id, "type": "application", "author": m.pubkey.to_hex(),
            "kind": m.kind.as_u16(), "content": m.content,
        }),
        MessageProcessingResult::Commit { .. } => json!({ "id": id, "type": "commit" }),
        MessageProcessingResult::Proposal(_) => json!({ "id": id, "type": "proposal" }),
        MessageProcessingResult::PendingProposal { .. } => {
            json!({ "id": id, "type": "pending_proposal" })
        }
        MessageProcessingResult::IgnoredProposal { reason, .. } => {
            json!({ "id": id, "type": "ignored_proposal", "reason": reason })
        }
        MessageProcessingResult::ExternalJoinProposal { .. } => {
            json!({ "id": id, "type": "external_join_proposal" })
        }
        MessageProcessingResult::Unprocessable { .. } => json!({ "id": id, "type": "unprocessable" }),
        MessageProcessingResult::PreviouslyFailed => json!({ "id": id, "type": "previously_failed" }),
    }
}

fn group_relays(peer: &Peer, group_id: &GroupId) -> Res<Vec<String>> {
    Ok(peer
        .mdk
        .get_relays(group_id)
        .map_err(err("get_relays"))?
        .iter()
        .map(|r| r.to_string())
        .collect())
}

/// MIP-03: publish the Commit to the group relays, merge it only once one
/// accepted it (else clear it).
async fn commit_and_merge(peer: &mut Peer, group_id: &GroupId, commit: &Event) -> Res<Value> {
    peer.seen.insert(commit.id);
    let relays = group_relays(peer, group_id)?;
    let (accepted, answers) = publish_all(&relays, commit, &Keys::generate()).await;
    if !accepted {
        let _ = peer.mdk.clear_pending_commit(group_id);
        return Err(format!("no group relay accepted the Commit: {answers}"));
    }
    peer.mdk.merge_pending_commit(group_id).map_err(err("merge_pending_commit"))?;
    let mut state = group_state(peer, group_id)?;
    state["commit_id"] = json!(commit.id.to_hex());
    Ok(state)
}

/// MIP-02: each Welcome rumor gift-wrapped (NIP-59) to the author of the
/// KeyPackage it answers (same order), published to welcome_relays.
async fn send_welcomes(
    peer: &Peer,
    rumors: Vec<UnsignedEvent>,
    kps: &[Event],
    welcome_relays: &[String],
) -> Res<Value> {
    if rumors.len() != kps.len() {
        return Err(format!("{} Welcomes for {} KeyPackages", rumors.len(), kps.len()));
    }
    let mut out = Vec::new();
    for (rumor, kp) in rumors.into_iter().zip(kps) {
        let wrap = EventBuilder::gift_wrap(&peer.keys, &kp.pubkey, rumor, [])
            .await
            .map_err(err("gift_wrap"))?;
        // No welcome_relays: made, not sent (vectors).
        let (accepted, answers) = publish_all(welcome_relays, &wrap, &Keys::generate()).await;
        if !accepted && !welcome_relays.is_empty() {
            return Err(format!("no inbox relay accepted the Welcome: {answers}"));
        }
        out.push(json!({ "to": kp.pubkey.to_hex(), "wrapper_id": wrap.id.to_hex() }));
    }
    Ok(Value::Array(out))
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
    let mut driver = Driver { peers: HashMap::new() };
    let mut lines = BufReader::new(tokio::io::stdin()).lines();
    let mut stdout = tokio::io::stdout();
    while let Ok(Some(line)) = lines.next_line().await {
        if line.trim().is_empty() {
            continue;
        }
        let req: Value = match serde_json::from_str(&line) {
            Ok(v) => v,
            Err(e) => {
                eprintln!("mdk-driver: bad request line: {e}");
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
                eprintln!("mdk-driver: {} failed: {e}", req.get("cmd").unwrap_or(&Value::Null));
                json!({ "id": id, "ok": false, "error": e })
            }
        };
        let mut text = answer.to_string();
        text.push('\n');
        if stdout.write_all(text.as_bytes()).await.is_err() || stdout.flush().await.is_err() {
            break;
        }
    }
}
