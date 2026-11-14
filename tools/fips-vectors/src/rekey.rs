use crate::{EPOCH, Exporter, Result, noise::handshake, support::*};
use fips_core::{
    identity::*,
    noise::{NoiseSession, seal},
    proto::{
        coord::TreeCoordinate,
        fsp::{rekey::FspProfile, wire::*},
        rekey::*,
    },
    time::Clock,
};
use serde_json::{Value, json};
use std::collections::VecDeque;

type P = FspProfile<SeededEntropy>;
type Machine = Rekey<P, TestClock>;
struct ZeroJitter;
impl JitterSource for ZeroJitter {
    fn draw(&mut self, _: u16) -> u16 {
        0
    }
}
struct Host {
    local: NodeKeys,
    tags: TagSource,
    indices: (),
    jitter: ZeroJitter,
    scratch: Vec<u8>,
}
impl Host {
    fn new(seed: u64) -> Self {
        Self {
            local: NodeKeys::new(seeded_keypair(seed), EPOCH),
            tags: TagSource::new(),
            indices: (),
            jitter: ZeroJitter,
            scratch: Vec::new(),
        }
    }
    fn env(&mut self) -> Env<'_, P> {
        Env {
            indices: &mut self.indices,
            transport: (),
            tags: &mut self.tags,
            workers: false,
            jitter: &mut self.jitter,
            local: &self.local,
            scratch: &mut self.scratch,
        }
    }
}
fn established(session: NoiseSession, mine: PeerKey, peer: PeerKey, host: &mut Host) -> Machine {
    let first = Completed::<P> {
        session: Box::new(NoiseKeys::new(session)),
        remote: peer,
        epoch: EPOCH,
        theirs: None,
    };
    let first = Bond::Key(peer).check(first).expect("bonded fixture peer");
    let facts = SetupFacts {
        peer_epoch: Some(EPOCH),
        k: false,
        link_since: TestInstant(0),
        started: TestInstant(0),
        peer_k: Some(false),
        last_rx: Some(TestInstant(0)),
        record: Record::new(),
    };
    let (machine, out) = Machine::established(
        Params::fsp(),
        Bond::Key(peer),
        mine.addr(),
        first,
        Ids::unindexed(()),
        facts,
        TestInstant(0),
        &mut host.env(),
    );
    assert!(out.is_empty());
    machine
}
fn epoch(v: Option<EpochView<'_, P, TestInstant>>) -> Value {
    match v {
        None => Value::Null,
        Some(v) => json!({"tag":v.tag.get(),"k":v.k,"role":format!("{:?}",v.role),
        "started_ms":v.started.0,"base_ms":v.base.map(|t|t.0),"recv":format!("{:?}",v.recv)}),
    }
}
fn state(m: &Machine) -> Value {
    let v = m.view();
    json!({"identity":format!("{:?}",v.identity),"peer_epoch_hex":v.peer_epoch.map(hex::encode),
        "last_rx_ms":v.last_rx.map(|t|t.0),"link_since_ms":v.link_since.0,"cycle":format!("{:?}",v.cycle),
        "current":epoch(v.current),"previous":epoch(v.previous),"pending":epoch(v.pending),"candidates":v.candidates,
        "next_wake_ms":m.next_wake().map(|t|t.0),"k":m.k(),"sent":m.sent()})
}
struct Packet {
    from: usize,
    kind: Option<MsgKind>,
    bytes: Vec<u8>,
    plaintext: Option<Vec<u8>>,
}
struct Trace<'a> {
    exporter: &'a Exporter<'a>,
    events: Vec<Value>,
    queue: VecDeque<Packet>,
}
impl Trace<'_> {
    fn record(
        &mut self,
        who: usize,
        now: u64,
        input: (&str, Option<&[u8]>),
        before: Value,
        m: &mut Machine,
        outputs: Outputs<P>,
    ) -> Result<()> {
        let (input, wire) = input;
        let index = self.events.len();
        let mut actions = Vec::new();
        for (n, out) in outputs.into_iter().enumerate() {
            match out {
                Output::Send { msg, kind, route } => {
                    actions.push(json!({"kind":"Send","message_kind":format!("{kind:?}"),"route":format!("{route:?}"),
                        "bytes":self.exporter.bytes(&format!("{index}-{n}-send"),&msg)?}));
                    self.queue.push_back(Packet {
                        from: who,
                        kind: Some(kind),
                        bytes: msg,
                        plaintext: None,
                    });
                }
                Output::Prompt { tag } => {
                    let prompt = m
                        .seal_prompt(TestInstant(now), tag)
                        .expect("prompt epoch sealable");
                    let reserved = &prompt.sealed;
                    let flags = if reserved.k { FSP_FLAG_K } else { 0 };
                    let header =
                        build_fsp_header(reserved.counter, flags, prompt.plaintext.len() as u16);
                    let ciphertext = seal(
                        reserved.key.key(),
                        reserved.counter,
                        &header,
                        &prompt.plaintext,
                    )?;
                    let bytes = build_fsp_encrypted(&header, &ciphertext);
                    actions.push(json!({"kind":"Prompt","tag":tag.get(),"counter":reserved.counter,"k":reserved.k,
                        "plaintext_hex":hex::encode(&prompt.plaintext),"bytes":self.exporter.bytes(&format!("{index}-{n}-prompt"),&bytes)?}));
                    self.queue.push_back(Packet {
                        from: who,
                        kind: None,
                        bytes,
                        plaintext: Some(prompt.plaintext),
                    });
                }
                Output::Release(lease) => {
                    actions.push(json!({"kind":"Release","index":lease.release()}));
                }
                other => actions.push(json!({"debug":format!("{other:?}")})),
            }
        }
        self.events.push(json!({"event":index,"node":who,"at_ms":now,"input":input,
            "input_hex":wire.map(hex::encode),"state_before":before,"outputs":actions,"state_after":state(m)}));
        Ok(())
    }
}
pub fn export(e: &Exporter<'_>) -> Result<()> {
    let t = handshake(true)?;
    let keys = [
        PeerKey::from_xonly(&seeded_keypair(0).x_only_public_key().0),
        PeerKey::from_xonly(&seeded_keypair(1).x_only_public_key().0),
    ];
    let mut hosts = [Host::new(0), Host::new(1)];
    let mut machines = [
        established(t.initiator, keys[0], keys[1], &mut hosts[0]),
        established(t.responder, keys[1], keys[0], &mut hosts[1]),
    ];
    let initial = json!([state(&machines[0]), state(&machines[1])]);
    let clock = TestClock::default();
    clock.advance(600_000);
    seed_entropy(0x72656b6579);
    let mut trace = Trace {
        exporter: e,
        events: Vec::new(),
        queue: VecDeque::new(),
    };
    let before = state(&machines[0]);
    let out = machines[0].start_now(clock.now(), &mut hosts[0].env());
    trace.record(
        0,
        clock.now().0,
        ("start_now (test-hook)", None),
        before,
        &mut machines[0],
        out,
    )?;
    deliver_all(&mut trace, &mut machines, &mut hosts, &keys, &clock)?;
    for m in &machines {
        let v = m.view();
        assert_eq!(v.cycle, CycleView::Idle);
        assert!(v.current.unwrap().tag.get() > 0);
        assert!(m.k());
    }
    // Application traffic under both installed keys confirms adoption, not
    // merely handshake completion. It also exercises the ordinary seal path.
    for who in 0..2 {
        clock.advance(10);
        let before = state(&machines[who]);
        let reserved = machines[who].seal(clock.now()).expect("installed epoch");
        let plaintext = fsp_prepend_inner_header(
            reserved.elapsed.as_millis() as u32,
            SessionMessageType::DataPacket.to_byte(),
            0,
            &[0x11, 0x22, 0x33, 0x44, 0x55],
        );
        let flags = if reserved.k { FSP_FLAG_K } else { 0 };
        let header = build_fsp_header(reserved.counter, flags, plaintext.len() as u16);
        let ciphertext = seal(reserved.key.key(), reserved.counter, &header, &plaintext)?;
        let bytes = build_fsp_encrypted(&header, &ciphertext);
        let out = machines[who].step(
            clock.now(),
            Input::DataSent { only: () },
            &mut hosts[who].env(),
        );
        trace.record(
            who,
            clock.now().0,
            ("Seal(DataPacket) + DataSent", None),
            before,
            &mut machines[who],
            out,
        )?;
        let index = trace.events.len() - 1;
        trace.events[index]["outputs"].as_array_mut().unwrap().push(
            json!({"kind":"ApplicationFrame",
            "plaintext_hex":hex::encode(&plaintext),"counter":reserved.counter,"k":reserved.k,
            "bytes":e.bytes(&format!("{index}-application"),&bytes)?}),
        );
        trace.queue.push_back(Packet {
            from: who,
            kind: None,
            bytes,
            plaintext: Some(plaintext),
        });
        deliver_all(&mut trace, &mut machines, &mut hosts, &keys, &clock)?;
    }
    // Run the actual deadline schedule; do not skip prompt/adoption events
    // by jumping straight to the maximum retention deadline.
    while machines.iter().any(|m| m.view().previous.is_some()) {
        assert!(
            trace.events.len() < 128,
            "drain did not finish in this bounded trace"
        );
        let next = machines
            .iter()
            .filter_map(|m| m.next_wake())
            .min()
            .expect("drain has a wake");
        clock.advance(
            next.0
                .checked_sub(clock.now().0)
                .expect("wake is not in the past"),
        );
        for who in 0..2 {
            let before = state(&machines[who]);
            let out = machines[who].step(clock.now(), Input::Poll, &mut hosts[who].env());
            trace.record(
                who,
                clock.now().0,
                ("Poll (next_wake)", None),
                before,
                &mut machines[who],
                out,
            )?;
        }
        deliver_all(&mut trace, &mut machines, &mut hosts, &keys, &clock)?;
    }
    for m in &machines {
        assert_eq!(m.view().cycle, CycleView::Idle);
        assert_eq!(m.view().current.unwrap().tag.get(), 1, "exactly one rekey");
    }
    let final_states = json!([state(&machines[0]), state(&machines[1])]);
    for m in machines {
        let (out, _) = m.close(Close::Shutdown);
        assert!(out.iter().all(|o| !matches!(o, Output::Release(_))));
    }
    e.finish(json!({"profile":"FspProfile<SeededEntropy> (Noise XK)","keypair_seeds":[0,1],"startup_epoch_hex":hex::encode(EPOCH),
        "entropy_seed_hex":"00000072656b6579","initial_handshake":"noise.json XK (before frame exports)",
        "params_debug":format!("{:?}",Config::fsp()),"clock":"TestClock milliseconds, 0 -> 600000 then 10ms per delivered packet, then application traffic and next_wake deadlines",
        "initial_state":initial,"events":trace.events,"final_state":final_states}),
        &["start_now is a test-hook that forces the initial rekey trigger; this trace validates exchange/cutover/drain, not production start-condition policy.",
          "Legacy v0.5.0/v0.5.1, v0.5.2 and v0.5.3 models are private cfg(test)/slow-tests modules, not exported with test-hooks. Capture/interop deferred to nostrc-f12v2.3; no legacy traces are claimed.",
          "One successful FSP cycle only; FMP and fault/loss/restart rekey traces remain future differential coverage."])
}

fn deliver_all(
    trace: &mut Trace<'_>,
    machines: &mut [Machine; 2],
    hosts: &mut [Host; 2],
    keys: &[PeerKey; 2],
    clock: &TestClock,
) -> Result<()> {
    while let Some(packet) = trace.queue.pop_front() {
        assert!(
            trace.events.len() < 128,
            "unexpected unbounded message exchange"
        );
        clock.advance(10);
        let who = 1 - packet.from;
        let before = state(&machines[who]);
        let original = packet.bytes.clone();
        let mut bytes = packet.bytes;
        let input: Input<'_, P> = match packet.kind {
            Some(MsgKind::Setup) => Input::Setup {
                msg1: &bytes,
                carrier: keys[packet.from].addr(),
                route: TreeCoordinate::root(keys[packet.from].addr()),
                only: (),
            },
            Some(MsgKind::Ack) => Input::Reply {
                bytes: &bytes,
                route: TreeCoordinate::root(keys[packet.from].addr()),
            },
            Some(MsgKind::Msg3) => Input::Final {
                bytes: &bytes,
                only: (),
            },
            None => {
                let h = FspEncryptedHeader::parse(&bytes).expect("framed prompt");
                Input::Frame {
                    buf: &mut bytes,
                    hint: FrameHint {
                        counter: h.counter,
                        k: h.flags & FSP_FLAG_K != 0,
                        index: 0,
                        body: FSP_HEADER_SIZE,
                    },
                }
            }
            _ => unreachable!("XK messages only"),
        };
        let label = packet
            .kind
            .map_or_else(|| "Frame".to_string(), |k| format!("{k:?}"));
        // Input::Frame performs the receiver's trial AEAD open and replay
        // acceptance inside Rekey. Inspect its authenticated deliveries rather
        // than opening separately and mutating the receiver's replay state.
        let out = machines[who].step(clock.now(), input, &mut hosts[who].env());
        let mut delivered = Vec::new();
        let mut delivery_records = Vec::new();
        for action in &out {
            if let Output::Deliver { slot, tag, range } = action {
                let plaintext = bytes
                    .get(range.clone())
                    .expect("valid authenticated plaintext range")
                    .to_vec();
                delivery_records.push(json!({"slot":format!("{slot:?}"),"tag":tag.get(),
                    "range":[range.start,range.end],"plaintext_hex":hex::encode(&plaintext)}));
                delivered.push(plaintext);
            }
        }
        if let Some(expected) = packet.plaintext {
            assert_eq!(
                delivered,
                vec![expected],
                "receiver must authenticate and deliver this exact prompt/data frame"
            );
        } else {
            assert!(
                delivered.is_empty(),
                "handshake input cannot deliver application data"
            );
        }
        trace.record(
            who,
            clock.now().0,
            (&label, Some(&original)),
            before,
            &mut machines[who],
            out,
        )?;
        trace.events.last_mut().unwrap()["authenticated_deliveries"] = json!(delivery_records);
    }
    Ok(())
}
