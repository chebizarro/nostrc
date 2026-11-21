// Adapted from pinned fips 64f5b4d482cf tests/gates.rs. The original
// thirteen-history Beside assertions run before each history is exported.
use super::At as TestInstant;
use fips_core::{
    identity::NodeAddr,
    proto::{
        fmp::EphemeralDigest,
        settle::{
            gates::{Ask, Epoch, Gates, Liveness, Refusal, Source},
            model::{
                history::{
                    Addr, Binding, Bytes, Epoch as MEpoch, Event, Hs, LinkId, Node as MNode,
                },
                judge as m,
            },
            params::Params,
        },
    },
};
use serde_json::Value;
fn peer() -> NodeAddr {
    NodeAddr::from_bytes([2; 16])
}

fn eph(n: u32) -> EphemeralDigest {
    let mut key = [0u8; 33];
    key[..4].copy_from_slice(&n.to_be_bytes());
    EphemeralDigest::of_noise_msg1(&key).expect("33 bytes carry an ephemeral")
}

fn epoch(n: u8) -> Option<Epoch> {
    Some([n; 8])
}

fn t(ms: u64) -> TestInstant {
    TestInstant(ms)
}

fn unheard(at: u64) -> Liveness<TestInstant> {
    Liveness {
        heard: false,
        installed: t(at),
        last: None,
    }
}

fn heard(at: u64, last: Option<u64>) -> Liveness<TestInstant> {
    Liveness {
        heard: true,
        installed: t(at),
        last: last.map(t),
    }
}

fn msg2(source: Source, held: Liveness<TestInstant>) -> Ask<TestInstant> {
    Ask {
        source,
        held: Some(held),
        ephemeral: None,
        epoch: epoch(2),
        memory: None,
    }
}
/// The gates beside a model `Judge`: each msg1 the model classes as rule 1,
/// 2 or 3.3, and each restart found at a msg2, asks the gate the same
/// source, and the refusal and the dampener's stamp must agree.
struct Beside {
    judge: m::Judge,
    gates: Gates<TestInstant>,
    held: Option<Liveness<TestInstant>>,
}

impl Beside {
    fn new() -> Self {
        Self {
            judge: m::Judge::new(m_small(), m_large(), m::Params::default()),
            gates: Gates::new(),
            held: None,
        }
    }

    /// Runs `history`, failing at the first disagreement.
    fn run(&mut self, name: &str, history: &[Event]) {
        let p = Params::default();
        let interval = u64::try_from(p.interval.as_millis()).expect("the interval fits");
        for (i, ev) in history.iter().enumerate() {
            let step = self.judge.step(*ev);
            let now = t(self.judge.now);
            let asked = match *ev {
                Event::Msg1 { hs, epoch, .. } => {
                    let source = match step.class {
                        Some(m::Class::NotHeld) => Some(Source::NotHeld),
                        Some(m::Class::Restart) => Some(Source::Restart),
                        Some(m::Class::Replacement) => Some(Source::Replacement),
                        _ => None,
                    };
                    source.map(|source| {
                        let ask = Ask {
                            source,
                            held: self.held,
                            ephemeral: Some(eph(hs.0)),
                            epoch: Some(epoch_of(epoch)),
                            memory: None,
                        };
                        let r = self.gates.gate(peer(), ask, now, &p);
                        if r.is_none() {
                            self.held = Some(unheard(now.0));
                            self.gates.promoted(peer(), eph(hs.0), now, None, &p);
                        }
                        r
                    })
                }
                Event::Msg2 { epoch, .. } => match step.change {
                    Some(m::Transition::Promote) => {
                        self.held = Some(heard(now.0, None));
                        None
                    }
                    Some(m::Transition::Replace) => {
                        let held = self.held.expect("a replace tears a peering down");
                        let ask = Ask {
                            epoch: Some(epoch_of(epoch)),
                            ..msg2(Source::OwnMsg2, held)
                        };
                        let r = self.gates.gate(peer(), ask, now, &p);
                        if r.is_none() {
                            self.held = Some(heard(now.0, None));
                        }
                        Some(r)
                    }
                    _ => None,
                },
                Event::RekeyCutover if step.change == Some(m::Transition::Restarted) => {
                    let held = self.held.expect("a rekey keeps its peering");
                    Some(
                        self.gates
                            .gate(peer(), msg2(Source::RekeyMsg2, held), now, &p),
                    )
                }
                Event::Frame { .. } => {
                    if let Some(h) = self.held.as_mut() {
                        h.heard = true;
                        h.last = Some(now);
                    }
                    self.gates.heard(&peer());
                    None
                }
                Event::Advance(_)
                | Event::Dial { .. }
                | Event::Msg2Sent { .. }
                | Event::Timeout { .. }
                | Event::Switch { .. }
                | Event::Follow { .. }
                | Event::RekeyStart { .. }
                | Event::RekeyMsg2 { .. }
                | Event::RekeyCutover
                | Event::Close { .. }
                | Event::Lost
                | Event::Restart => None,
            };
            if let Some(r) = asked {
                assert_eq!(
                    refusal_of(r),
                    step.refused,
                    "{name}, event {i} ({ev:?}): the refusal"
                );
            }
            // The stamp while it still counts: the gates drop an identity
            // whose stamp, replays and silence have all lapsed, where the
            // model keeps its last stamp for good, and a stamp past the
            // interval refuses nothing on either side.
            let counts = |s: u64| self.judge.now.saturating_sub(s) < interval;
            let stamp = self
                .gates
                .stamp(&peer())
                .map(|s| s.0)
                .filter(|s| counts(*s));
            assert_eq!(
                stamp,
                self.judge.stamp.filter(|s| counts(*s)),
                "{name}, event {i} ({ev:?}): the dampener's stamp"
            );
        }
    }
}

fn m_small() -> MNode {
    MNode(1)
}

fn m_large() -> MNode {
    MNode(2)
}

fn epoch_of(e: MEpoch) -> Epoch {
    let mut out = [0u8; 8];
    out[..4].copy_from_slice(&e.0.to_be_bytes());
    out
}

/// The model's name for a core refusal; the replay memory and the silent
/// sessions have none, and no gate history reaches them.
fn refusal_of(r: Option<Refusal>) -> Option<m::Gate> {
    r.map(|r| match r {
        Refusal::Unheard => m::Gate::Unheard,
        Refusal::Heard => m::Gate::Heard,
        Refusal::Dampened => m::Gate::Dampened,
        Refusal::Replayed | Refusal::ReplayFull | Refusal::Silent | Refusal::Redialed => {
            panic!("the model has no {r:?} refusal")
        }
    })
}

fn at(port: u16) -> Addr {
    Addr {
        transport: 1,
        at: port,
    }
}

fn ev_msg1(hs: u32, bytes: u32, epoch: u32, link: u32, from: u16) -> Event {
    Event::Msg1 {
        hs: Hs(hs),
        bytes: Bytes(bytes),
        epoch: MEpoch(epoch),
        link: LinkId(link),
        from: at(from),
        binding: Binding::Connectionless,
        sent: true,
    }
}

/// The model's gate tests' replacement: a same-epoch msg1 on the link,
/// from its setup address, on a link too young to rekey. From a new
/// address, the msg1 of a peering heard from would be a redial, which no
/// gate floor holds.
fn repl(hs: u32) -> Event {
    ev_msg1(hs, hs * 10, 1, hs, 9)
}

fn frame(hs: u32) -> Event {
    Event::frame(Hs(hs), at(9))
}

fn wait(ms: u64) -> Event {
    Event::Advance(ms)
}

pub(super) fn traces() -> Vec<Value> {
    // The histories of `model/tests/gates.rs`, by its test names, and the
    // restart at an own msg2 one second after a frame.
    let first = ev_msg1(10, 100, 1, 1, 9);
    let dial = |hs: u32, link: u32, to: u16| Event::Dial {
        hs: Hs(hs),
        link: LinkId(link),
        to: at(to),
        binding: Binding::Connectionless,
    };
    let own = |hs: u32, e: u32, from: u16| Event::Msg2 {
        hs: Hs(hs),
        epoch: MEpoch(e),
        from: at(from),
    };
    let histories: Vec<(&str, Vec<Event>)> = std::vec![
        (
            "unheard floor, early",
            std::vec![first, wait(999), repl(11)]
        ),
        (
            "unheard floor, at it",
            std::vec![first, wait(1_000), repl(11)]
        ),
        (
            "heard floor from the last frame",
            std::vec![
                first,
                wait(5_000),
                frame(10),
                wait(14_999),
                repl(11),
                wait(1),
                repl(12)
            ],
        ),
        (
            "heard floor from installation",
            std::vec![
                dial(1, 1, 9),
                wait(2_000),
                own(1, 1, 9),
                wait(14_999),
                repl(11),
                wait(1),
                repl(12)
            ],
        ),
        (
            "heard from the first frame, before",
            std::vec![first, wait(1_500), repl(11)]
        ),
        (
            "heard from the first frame, after",
            std::vec![first, wait(1_000), frame(10), wait(500), repl(11)],
        ),
        (
            "dampener",
            std::vec![
                first,
                frame(10),
                wait(15_000),
                ev_msg1(11, 110, 2, 2, 8),
                wait(14_999),
                ev_msg1(12, 120, 3, 3, 7),
                wait(1),
                ev_msg1(12, 120, 3, 3, 7),
            ],
        ),
        (
            "between the floors",
            std::vec![first, wait(2_000), repl(11)]
        ),
        ("stamp, unheard", std::vec![first, wait(2_000), repl(11)]),
        (
            "stamp, heard",
            std::vec![first, frame(10), wait(15_000), repl(11)]
        ),
        (
            "own msg2 restart",
            std::vec![first, frame(10), wait(20_000), dial(1, 2, 8), own(1, 2, 8)],
        ),
        (
            "own msg2 restart inside the heard floor",
            std::vec![first, frame(10), wait(1_000), dial(1, 2, 8), own(1, 2, 8)],
        ),
        (
            "rekey msg2 restart",
            std::vec![
                first,
                frame(10),
                wait(30_000),
                Event::RekeyStart { hs: Hs(20) },
                Event::RekeyMsg2 {
                    hs: Hs(20),
                    epoch: MEpoch(2)
                },
                Event::RekeyCutover,
            ],
        ),
    ];
    let mut traces = Vec::new();
    for (name, history) in &histories {
        Beside::new().run(name, history);
        traces.push(super::model_oracle::record_gate_history(name, history));
    }
    traces
}
