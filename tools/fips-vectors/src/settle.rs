//! Settlement primitive traces. Keep this module independent of other families
//! while D1 upgrades their oracle scaffolding at the same upstream revision.
use crate::{Result, VERSION};
use fips_core::{
    identity::NodeAddr,
    proto::{
        rekey::IndexLease,
        settle::{
            legs::{LegOut, Legs, Start},
            links::{Dir, Holder, LinkNo, Links, Pair, Proven},
            params::Params,
        },
    },
    time::{Duration, Instant},
    transport::{TransportAddr, TransportId},
};
use serde_json::{Value, json};
use std::{fs, path::Path};
const PIN: &str = "64f5b4d482cfe14abad108ffc2d92128f7658311";
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
struct At(u64);
impl Instant for At {
    fn saturating_duration_since(self, b: Self) -> Duration {
        Duration::from_millis(self.0.saturating_sub(b.0))
    }
    fn checked_add(self, d: Duration) -> Option<Self> {
        self.0
            .checked_add(u64::try_from(d.as_millis()).ok()?)
            .map(Self)
    }
    fn checked_sub(self, d: Duration) -> Option<Self> {
        self.0
            .checked_sub(u64::try_from(d.as_millis()).ok()?)
            .map(Self)
    }
}
fn pair(p: u64) -> Pair {
    Pair {
        transport: TransportId::new((p / 8 + 1) as u32),
        addr: TransportAddr::from_bytes(&[(p % 8) as u8]),
    }
}
fn number(p: &Pair) -> u64 {
    u64::from(p.transport.as_u32() - 1) * 8 + u64::from(p.addr.as_bytes()[0])
}
fn holder(n: u64) -> Holder {
    if n == 0 {
        Holder::Leg
    } else {
        Holder::Peer(NodeAddr::from_bytes([n as u8; 16]))
    }
}
fn who(h: Holder) -> u64 {
    match h {
        Holder::Leg => 0,
        Holder::Peer(p) => u64::from(p.as_bytes()[0]),
    }
}
fn state(s: u64) -> Proven {
    match s {
        0 => Proven::Live,
        1 => Proven::Acked,
        _ => Proven::Probing,
    }
}
fn state_n(s: Proven) -> u64 {
    match s {
        Proven::Live => 0,
        Proven::Acked => 1,
        Proven::Probing => 2,
    }
}
fn snapshot(links: &Links, next: u64) -> Value {
    let mut records = Vec::new();
    let mut places = Vec::new();
    for i in 0..next {
        if let Some(r) = links.get(LinkNo(i)) {
            records.push(json!([
                i,
                number(&r.setup),
                r.dir == Dir::Inbound,
                who(r.holder),
                r.released,
                r.capable,
                r.paths
                    .iter()
                    .map(|(p, s)| json!([number(p), state_n(*s)]))
                    .collect::<Vec<_>>()
            ]));
            for p in 0..16 {
                let at = links.placement(&pair(p), |l| {
                    links.get(l).and_then(|r| {
                        (number(&r.setup) != 7).then_some(NodeAddr::from_bytes([3; 16]))
                    })
                });
                places.push(json!([
                    i,
                    p,
                    at.established(),
                    at.expected().map(|p| p.as_bytes()[0]),
                    at.unanswered().map(|l| l.0),
                    at.on_link(LinkNo(i))
                ]));
            }
        }
    }
    json!({"records":records,"index":links.index().iter().map(|(p,l)|json!([number(p),l.0])).collect::<Vec<_>>(),"places":places})
}
fn link_trace() -> Value {
    let mut links = Links::new();
    let mut next = 0u64;
    let mut rows = Vec::new();
    let mut seed = 0x5eedu64;
    // Enough slot reuse and conflicting addresses to exercise newest-link and
    // first-path rules, including moves producing duplicate destinations.
    for step in 0..192 {
        seed = seed.wrapping_mul(6364136223846793005).wrapping_add(1);
        let live: Vec<_> = (0..next)
            .filter(|i| links.get(LinkNo(*i)).is_some())
            .collect();
        let id = if live.is_empty() {
            0
        } else {
            live[(seed >> 12) as usize % live.len()]
        };
        let p = (seed >> 24) % 16;
        let q = (seed >> 36) % 16;
        let choice = if live.is_empty() || (live.len() < 4 && step % 7 == 0) {
            0
        } else {
            1 + (seed % 7)
        };
        let mut closes = Vec::new();
        let op = match choice {
            0 => {
                let dir = if seed & 1 == 0 {
                    Dir::Outbound
                } else {
                    Dir::Inbound
                };
                let h = (seed >> 5) % 5;
                let l = links.open(pair(p), dir, holder(h));
                assert_eq!(l.0, next);
                next += 1;
                json!([0, p, dir == Dir::Inbound, h])
            }
            1 => {
                let s = (seed >> 8) % 3;
                links.path(LinkNo(id), pair(p), state(s));
                json!([1, id, p, s])
            }
            2 => {
                links.moved(LinkNo(id), &pair(p), pair(q));
                json!([2, id, p, q])
            }
            3 => {
                let keep = seed & 1 != 0;
                closes = links.release(LinkNo(id), &pair(p), keep);
                json!([3, id, p, keep])
            }
            4 => {
                links.capable(LinkNo(id));
                json!([4, id])
            }
            5 => {
                closes = links.retire(LinkNo(id));
                json!([5, id])
            }
            _ => {
                let h = (seed >> 6) % 5;
                links.hold(LinkNo(id), holder(h));
                json!([6, id, h])
            }
        };
        rows.push(json!({"op":op,"closes":closes.iter().map(number).collect::<Vec<_>>(),"want":snapshot(&links,next)}));
    }
    json!({"kind":"links","rows":rows})
}
fn leg_trace(multiplier: f64, resend: u64, cap: Option<u32>, answer: bool, retry: bool) -> Value {
    let p = Params {
        handshake: Duration::from_secs(120),
        backoff: multiplier.to_bits(),
        resend: Duration::from_millis(resend),
        ..Params::default()
    };
    let mut legs = Legs::<At>::new();
    let mut links = Links::new();
    let mut rows = Vec::new();
    let link = links.open(pair(1), Dir::Outbound, Holder::Leg);
    let start = Start {
        link,
        peer: Some(NodeAddr::from_bytes([2; 16])),
        msg1: b"msg1 of the dial".to_vec(),
        lease: IndexLease::for_test(42),
        resends: cap,
        held: Some(LinkNo(99)),
    };
    let old = if answer {
        legs.answer(start, b"msg2 of the answer".to_vec(), At(0), &p)
    } else {
        legs.dial(start, At(0), &p)
    };
    assert!(old.is_none());
    let mut times = vec![0, 999, 1000, 1001, 120000];
    // Capture polls at actual scheduled wakes. Failed send is immediately due
    // on the next poll, even though reported wake lies a retry interval on.
    let mut attempts = 0;
    while let Some(wake) = legs.next_wake() {
        let t = if times.is_empty() {
            wake.0
        } else {
            let t = times.remove(0);
            t.min(wake.0)
        };
        if t == 120000 && wake.0 < 120000 {
            continue;
        }
        let outs = legs.poll(At(t), &mut links);
        let mut events = Vec::new();
        for o in outs {
            match o {
                LegOut::Resend { link, bytes } => {
                    events.push(json!([0, link.0, hex::encode(bytes)]));
                    if retry && attempts == 0 {
                        legs.unsent(link, At(t), &p)
                    } else {
                        legs.resent(link, At(t), &p)
                    };
                    attempts += 1;
                }
                LegOut::Reaped {
                    link,
                    peer,
                    dialed,
                    closes,
                    lease,
                } => events.push(json!([
                    1,
                    link.0,
                    peer.map(|p| p.as_bytes()[0]),
                    dialed,
                    lease.release(),
                    closes.iter().map(number).collect::<Vec<_>>()
                ])),
            }
        }
        let leg = legs.get(link).map(|l| {
            json!([
                l.sent(),
                l.resends(),
                l.next_resend().map(|t| t.0),
                l.deadline().0,
                l.index(),
                l.held().map(|l| l.0)
            ])
        });
        rows.push(json!({"now":t,"events":events,"leg":leg,"wake":legs.next_wake().map(|t|t.0),"answering":legs.answering(),"stored":legs.stored(link,b"msg1 of the dial").map(hex::encode),"links":snapshot(&links,1)}));
        assert!(rows.len() < 32);
    }
    json!({"kind":"legs","multiplier_bits":format!("{:016x}",multiplier.to_bits()),"resend":resend,"cap":cap,"answer":answer,"retry":retry,"rows":rows})
}
fn gate_trace() -> Value {
    use fips_core::proto::{
        fmp::EphemeralDigest,
        settle::gates::{Ask, Gates, Liveness, Refusal, Source},
    };
    fn eph(n: u64) -> EphemeralDigest {
        EphemeralDigest::of_noise_msg1(&[n as u8; 33]).unwrap()
    }
    fn peer(n: u64) -> NodeAddr {
        NodeAddr::from_bytes([n as u8; 16])
    }
    fn refusal(r: Option<Refusal>) -> u64 {
        match r {
            None => 0,
            Some(Refusal::Unheard) => 1,
            Some(Refusal::Heard) => 2,
            Some(Refusal::Dampened) => 3,
            Some(Refusal::Replayed) => 4,
            Some(Refusal::ReplayFull) => 5,
            Some(Refusal::Silent) => 6,
            Some(Refusal::Redialed) => 7,
        }
    }
    let p = Params::default();
    let mut g = Gates::<At>::new();
    let mut ops = Vec::new();
    for source in 0..6 {
        for now in [999, 1000, 14999, 15000, 29999, 30000] {
            ops.push(json!([0, 0, source, 1, 0, null, 1, 1, now, null]));
            ops.push(json!([0, 1, source, 2, 0, 5000, 2, 1, now, null]));
        }
    }
    for n in 0..10 {
        ops.push(json!([1, 2, n, n * 1000, null]));
        ops.push(json!([
            0,
            2,
            2,
            1,
            n * 1000,
            null,
            n + 1,
            1,
            n * 1000 + 1000,
            null
        ]));
    }
    for n in 1..10 {
        ops.push(json!([2, 3, 1, n, n * 1000, null]));
        ops.push(json!([8, 3]));
    }
    for source in 0..6 {
        ops.push(json!([0, 3, source, 2, 0, 0, 20, 1, 10000, null]));
    }
    ops.extend([
        json!([9, 3]),
        json!([3, 3]),
        json!([6, 3, 1, 40000]),
        json!([3, 3]),
        json!([4, 3]),
        json!([5, 3]),
    ]);
    for n in 1..10 {
        ops.push(json!([6, 3, n, 41000]));
        ops.push(json!([0, 3, 3, 2, 0, 41000, n, 1, 42000, null]));
    }
    ops.extend([
        json!([7, 3, false, 43000]),
        json!([0, 3, 3, 2, 0, 43000, 1, 1, 43001, null]),
        json!([7, 3, true, 44000]),
        json!([0, 3, 3, 2, 0, 44000, 2, 1, 58999, null]),
        json!([0, 3, 3, 2, 0, 44000, 2, 1, 59000, null]),
        json!([1, 2, 20, 60000, 1000]),
        json!([0, 2, 2, 1, 0, null, 20, 1, 60000, 1001]),
        json!([0, 0, 0, 0, 0, null, 0, 1, 3600000, null]),
    ]);
    let mut now = 0;
    let mut rows = Vec::new();
    for op in ops {
        let a = op.as_array().unwrap();
        let n = a[1].as_u64().unwrap();
        let who = peer(n);
        let result = match a[0].as_u64().unwrap() {
            0 => {
                let sources = [
                    Source::NotHeld,
                    Source::Restart,
                    Source::Replacement,
                    Source::Redial,
                    Source::OwnMsg2,
                    Source::RekeyMsg2,
                ];
                now = a[8].as_u64().unwrap();
                let held = if a[3] == 0 {
                    None
                } else {
                    Some(Liveness {
                        heard: a[3] == 2,
                        installed: At(a[4].as_u64().unwrap()),
                        last: a[5].as_u64().map(At),
                    })
                };
                let ask = Ask {
                    source: sources[a[2].as_u64().unwrap() as usize],
                    held,
                    ephemeral: a[6].as_u64().map(eph),
                    epoch: a[7].as_u64().map(|n| [n as u8; 8]),
                    memory: a[9].as_u64().map(At),
                };
                json!(refusal(g.gate(who, ask, At(now), &p)))
            }
            1 => {
                now = a[3].as_u64().unwrap();
                g.promoted(
                    who,
                    eph(a[2].as_u64().unwrap()),
                    At(now),
                    a[4].as_u64().map(At),
                    &p,
                );
                Value::Null
            }
            2 => {
                now = a[4].as_u64().unwrap();
                json!(
                    g.ended(
                        who,
                        a[2].as_u64().map(|n| [n as u8; 8]),
                        a[3].as_u64().map(eph),
                        At(now),
                        a[5].as_u64().map(At),
                        &p
                    )
                    .map(|d| u64::try_from(d.as_millis()).unwrap())
                )
            }
            3 => {
                g.heard(&who);
                Value::Null
            }
            4 => {
                g.framed(&who);
                Value::Null
            }
            5 => {
                g.forget(&who);
                Value::Null
            }
            6 => {
                now = a[3].as_u64().unwrap();
                g.redialed(who, eph(a[2].as_u64().unwrap()), At(now), &p);
                Value::Null
            }
            7 => {
                now = a[3].as_u64().unwrap();
                g.swapped(who, a[2].as_bool().unwrap(), At(now));
                Value::Null
            }
            8 => json!(g.count_refused(&who)),
            9 => json!(g.take_refused(&who)),
            _ => unreachable!(),
        };
        let view: Vec<_> = g
            .known()
            .map(|who| {
                json!([
                    who.as_bytes()[0],
                    g.replays(who),
                    g.redials(who),
                    g.silent(who),
                    g.stamp(who).map(|t| t.0),
                    g.refusing(who, At(now)).map(|r| json!([
                        r.epoch.map(|e| e[0]),
                        r.silent,
                        u64::try_from(r.remaining.as_millis()).unwrap()
                    ]))
                ])
            })
            .collect();
        rows.push(json!({"op":op,"result":result,"now":now,"view":view}));
    }
    json!({"kind":"gates","rows":rows})
}
pub fn export(out: &Path) -> Result<()> {
    for name in [
        "FIPS_SETTLE_FAULTS",
        "FIPS_SETTLE_CANDIDATE",
        "FIPS_NODE_FAULTS",
        "FIPS_REKEY_FAULTS",
    ] {
        if std::env::var_os(name).is_some() {
            return Err(
                format!("unset {name}: settle oracle must not enable mutation switches").into(),
            );
        }
    }
    let mut traces = vec![link_trace(), gate_trace()];
    for m in [1.3333, 1.5, 2.0, 2.5005] {
        traces.push(leg_trace(m, 1000, None, false, false));
    }
    for r in [500, 3000] {
        traces.push(leg_trace(2.0, r, None, false, false));
    }
    for cap in [0, 2, 8] {
        traces.push(leg_trace(2.0, 1000, Some(cap), false, false));
    }
    traces.push(leg_trace(2.0, 1000, None, true, false));
    traces.push(leg_trace(2.0, 1000, None, false, true));
    let doc = json!({"schema_version":1,"pin":PIN,"generator":"fips-vectors","generator_version":VERSION,"family":"settle","cases":traces,"gaps":["Params/links/legs and gates: no table/judge/joint/legacy simulator equivalence"]});
    let mut data = serde_json::to_vec(&doc)?;
    data.push(b'\n');
    fs::write(out.join("settle.json"), data)?;
    Ok(())
}
