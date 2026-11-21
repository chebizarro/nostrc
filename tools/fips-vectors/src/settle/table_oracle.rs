//! Trace recorder for the pinned upstream table tests. The C implementation is
//! not an oracle input: assertions execute against upstream, then C replays
//! every input and compares ordered effects and complete public state views.
use super::At;
use fips_core::{
    identity::{NodeAddr, sha256},
    proto::{
        fmp::EphemeralDigest,
        settle::{
            gates::{Epoch, Gates},
            input::{Dial, Hello, Input, Read},
            legs::{Kind, Legs},
            links::{Dir, Holder, LinkNo, Links, Pair, Proven},
            output::{Build, Denial, Output, Transition, Why},
            params::Params,
            table::{Held, Table as CoreTable},
        },
    },
    time::{Duration, Instant},
};
use serde_json::{Value, json};
use std::cell::RefCell;
thread_local! {
    static ROWS:RefCell<Vec<Value>>=const { RefCell::new(Vec::new()) };
    static EPHEMERALS:RefCell<Vec<(EphemeralDigest,[u8;32])>>=const { RefCell::new(Vec::new()) };
    static CASE:RefCell<String>=const { RefCell::new(String::new()) };
    static NEXT:RefCell<usize>=const { RefCell::new(0) };
}
fn begin_case(name: &str) {
    ROWS.with(|r| r.borrow_mut().clear());
    EPHEMERALS.with(|r| r.borrow_mut().clear());
    CASE.with(|r| *r.borrow_mut() = name.into());
    NEXT.with(|r| *r.borrow_mut() = 0);
}
fn finish_case() -> Value {
    json!({"kind":"table","test":CASE.with(|r|r.borrow().clone()),"rows":ROWS.with(|r|std::mem::take(&mut *r.borrow_mut()))})
}
fn push(row: Value) {
    ROWS.with(|r| r.borrow_mut().push(row));
}
fn note_ephemeral(n: u32) -> EphemeralDigest {
    let mut key = [0; 33];
    key[..4].copy_from_slice(&n.to_be_bytes());
    let digest = EphemeralDigest::of_noise_msg1(&key).unwrap();
    EPHEMERALS.with(|r| {
        let mut r = r.borrow_mut();
        if !r.iter().any(|(d, _)| *d == digest) {
            r.push((digest, sha256(&key)));
        }
    });
    digest
}
fn eph(e: EphemeralDigest) -> String {
    EPHEMERALS.with(|r| {
        hex::encode(
            r.borrow()
                .iter()
                .find(|(d, _)| *d == e)
                .expect("every test ephemeral is observed at construction")
                .1,
        )
    })
}
fn node(n: NodeAddr) -> String {
    hex::encode(n.as_bytes())
}
fn pair(p: &Pair) -> Value {
    json!([p.transport.as_u32(), hex::encode(p.addr.as_bytes())])
}
fn read(r: &Read) -> Value {
    json!([
        node(r.peer),
        pair(&r.from),
        hex::encode(&r.bytes),
        eph(r.ephemeral),
        hex::encode(r.epoch)
    ])
}
fn dial(d: &Dial) -> Value {
    json!([
        node(d.peer),
        pair(&d.to),
        hex::encode(&d.msg1),
        d.lease.index(),
        d.resends,
        d.link.map(|l| l.0)
    ])
}
fn input(i: &Input) -> Value {
    match i {
        Input::Dial(d) => json!([0, dial(d)]),
        Input::Connecting { peer, to, resends } => json!([1, node(*peer), pair(to), resends]),
        Input::Punched(d) => json!([2, dial(d)]),
        Input::ConnectFailed { link } => json!([3, link.0]),
        Input::Arrived { from, bytes } => json!([4, pair(from), hex::encode(bytes)]),
        Input::Msg1 { read: r, reachable } => json!([5, read(r), reachable]),
        Input::Replace(r) => json!([6, read(r)]),
        Input::Redial(r) => json!([7, read(r)]),
        Input::LegHeard { link, from } => json!([8, link.0, pair(from)]),
        Input::Hello(h) => json!([9, h.link.0, hex::encode(&h.msg2), h.lease.index()]),
        Input::Abandon { link, authorized } => json!([10, link.0, authorized]),
        Input::Msg2 { link, epoch, from } => json!([11, link.0, hex::encode(epoch), pair(from)]),
        Input::Restarted { peer, epoch } => json!([12, node(*peer), hex::encode(epoch)]),
        Input::PeerEpoch { peer, epoch } => json!([13, node(*peer), hex::encode(epoch)]),
        Input::Ended { peer } => json!([14, node(*peer)]),
        Input::Orphaned { peer } => json!([15, node(*peer)]),
        Input::Heard { peer, from, kind } => json!([16, node(*peer), pair(from), kind]),
        Input::Disconnect { peer } => json!([17, node(*peer)]),
        Input::Dead { peer } => json!([18, node(*peer)]),
        Input::Admin { peer } => json!([19, node(*peer)]),
        Input::Sent { link } => json!([20, link.0]),
        Input::SendFailed { link } => json!([21, link.0]),
        Input::Unsendable { link } => json!([22, link.0]),
        Input::Moved {
            link,
            from,
            to,
            roam,
        } => json!([23, link.0, pair(from), pair(to), roam]),
        Input::Switched { link, to, proven } => json!([24, link.0, pair(to), proven_n(*proven)]),
        Input::Released {
            link,
            pair: p,
            keep,
        } => json!([25, link.0, pair(p), keep]),
        Input::Capable { link } => json!([26, link.0]),
        Input::State {
            link,
            pair: p,
            proven,
        } => json!([27, link.0, pair(p), proven_n(*proven)]),
    }
}
fn proven_n(p: Proven) -> u8 {
    match p {
        Proven::Live => 0,
        Proven::Acked => 1,
        Proven::Probing => 2,
    }
}
fn pairs(p: &[Pair]) -> Vec<Value> {
    p.iter().map(pair).collect()
}
fn outputs(out: &[Output]) -> Value {
    json!(
        out.iter()
            .map(|o| match o {
                Output::Opened { link } => json!([0, link.0]),
                Output::Ask { link, peer } => json!([1, link.0, node(*peer)]),
                Output::Answer { link, msg2, stored } =>
                    json!([2, link.0, hex::encode(msg2), stored]),
                Output::Resend { link, bytes } => json!([3, link.0, hex::encode(bytes)]),
                Output::Rekey { peer, arrival } => json!([
                    4,
                    node(*peer),
                    arrival.on_link,
                    arrival.reachable,
                    arrival.heard
                ]),
                Output::Install(i) => json!([
                    5,
                    node(i.peer),
                    i.link.0,
                    pair(&i.pair),
                    i.rekey == Build::FromCarry,
                    i.initiator,
                    i.lease.index()
                ]),
                Output::Reset { peer, transition } =>
                    json!([6, node(*peer), format!("{transition:?}")]),
                Output::Remove { peer, why } => json!([7, node(*peer), format!("{why:?}")]),
                Output::Retire { link, closes } => json!([8, link.0, pairs(closes)]),
                Output::Release { link, closes } => json!([9, link.0, pairs(closes)]),
                Output::Close { closes } => json!([10, pairs(closes)]),
                Output::Free { lease } => json!([11, lease.index()]),
                Output::Swapped { peer, own, unpin } => json!([12, node(*peer), own, unpin]),
                Output::Retry { peer } => json!([13, node(*peer)]),
                Output::Reject { peer, source, why } => {
                    let denial = match why {
                        Denial::Gate(r) => json!([0, format!("{r:?}")]),
                        Denial::Full => json!([1]),
                        Denial::Held => json!([2]),
                        Denial::Cap => json!([3]),
                    };
                    json!([14, peer.map(node), source.map(|s| format!("{s:?}")), denial])
                }
            })
            .collect::<Vec<_>>()
    )
}
fn snapshot(t: &CoreTable<At>, next: u64, now: At) -> Value {
    let links = t.links();
    let mut records = Vec::new();
    for n in 0..next {
        if let Some(r) = links.get(LinkNo(n)) {
            records.push(json!([
                n,
                pair(&r.setup),
                r.dir == Dir::Inbound,
                match r.holder {
                    Holder::Leg => None,
                    Holder::Peer(p) => Some(node(p)),
                },
                r.released,
                r.capable,
                r.paths
                    .iter()
                    .map(|(p, s)| json!([pair(p), proven_n(*s)]))
                    .collect::<Vec<_>>()
            ]));
        }
    }
    let held: Vec<_> = t
        .held_peers()
        .map(|p| {
            let h = t.held(p).unwrap();
            json!([
                node(*p),
                h.link.0,
                hex::encode(h.epoch),
                pair(&h.current),
                h.heard,
                h.installed.0,
                h.last.map(|i| i.0),
                h.setup.map(eph),
                h.sending.as_ref().map(pair)
            ])
        })
        .collect();
    let legs: Vec<_> = t
        .legs()
        .iter()
        .map(|l| {
            json!([
                l.link().0,
                l.peer().map(node),
                hex::encode(l.msg1()),
                t.legs().msg2(l.link()).map(hex::encode),
                l.sent(),
                l.resends(),
                l.next_resend().map(|i| i.0),
                l.deadline().0,
                l.index(),
                l.held().map(|i| i.0)
            ])
        })
        .collect();
    let g = t.gates();
    let gates: Vec<_> = g
        .known()
        .map(|p| {
            json!([
                node(*p),
                g.replays(p),
                g.redials(p),
                g.silent(p),
                g.stamp(p).map(|i| i.0),
                g.refusing(p, now).map(|r| json!([
                    r.epoch.map(hex::encode),
                    r.silent,
                    u64::try_from(r.remaining.as_millis()).unwrap()
                ]))
            ])
        })
        .collect();
    json!({"records":records,"index":links.index().iter().map(|(p,l)|json!([pair(p),l.0])).collect::<Vec<_>>(),"held":held,"legs":legs,"gates":gates,"redials":t.redials().map(|(p,l)|json!([node(*p),l.0])).collect::<Vec<_>>(),"wake":t.next_wake().map(|i|i.0)})
}
fn capture(t: &CoreTable<At>, id: usize, next: u64, now: At, op: Value, out: &[Output]) {
    push(
        json!({"id":id,"now":now.0,"next":next,"op":op,"out":outputs(out),"state":snapshot(t,next,now)}),
    );
}
fn drop_table(t: &mut CoreTable<At>, id: usize, next: u64, now: At) {
    let out = t.close(now);
    capture(t, id, next, now, json!(["close"]), &out);
    for o in out {
        match o {
            Output::Free { lease } => {
                lease.release();
            }
            Output::Install(i) => {
                i.lease.release();
            }
            _ => {}
        }
    }
    push(json!({"id":id,"op":["drop"]}));
}
#[derive(Debug)]
struct Table<I: Instant> {
    inner: CoreTable<I>,
    id: usize,
    next: u64,
    now: I,
    dropper: fn(&mut CoreTable<I>, usize, u64, I),
}
impl<I: Instant> Drop for Table<I> {
    fn drop(&mut self) {
        (self.dropper)(&mut self.inner, self.id, self.next, self.now)
    }
}
impl Table<At> {
    fn new(me: NodeAddr, p: Params) -> Self {
        let id = NEXT.with(|n| {
            let id = *n.borrow();
            *n.borrow_mut() += 1;
            id
        });
        push(json!({"id":id,"op":["new",node(me),p.pending,p.peers,p.resends]}));
        Self {
            inner: CoreTable::new(me, p),
            id,
            next: 0,
            now: At(0),
            dropper: drop_table,
        }
    }
    fn step(&mut self, i: Input, now: At) -> Vec<Output> {
        let op = json!(["step", input(&i)]);
        let out = self.inner.step(i, now);
        for o in &out {
            if let Output::Opened { link } | Output::Ask { link, .. } = o {
                self.next = self.next.max(link.0 + 1);
            }
        }
        self.now = now;
        capture(&self.inner, self.id, self.next, now, op, &out);
        out
    }
    fn poll(&mut self, now: At) -> Vec<Output> {
        let out = self.inner.poll(now);
        self.now = now;
        capture(&self.inner, self.id, self.next, now, json!(["poll"]), &out);
        out
    }
    fn reap_leg(&mut self, link: LinkNo, now: At) -> Vec<Output> {
        let out = self.inner.reap_leg(link, now);
        self.now = now;
        capture(
            &self.inner,
            self.id,
            self.next,
            now,
            json!(["reap", link.0]),
            &out,
        );
        out
    }
    fn close(&mut self, now: At) -> Vec<Output> {
        let out = self.inner.close(now);
        self.now = now;
        capture(&self.inner, self.id, self.next, now, json!(["close"]), &out);
        out
    }
    fn held(&self, p: &NodeAddr) -> Option<&Held<At>> {
        self.inner.held(p)
    }
    fn links(&self) -> &Links {
        self.inner.links()
    }
    fn legs(&self) -> &Legs<At> {
        self.inner.legs()
    }
    fn gates(&self) -> &Gates<At> {
        self.inner.gates()
    }
    fn next_wake(&self) -> Option<At> {
        self.inner.next_wake()
    }
    fn redialing(&self, p: &NodeAddr) -> Option<LinkNo> {
        self.inner.redialing(p)
    }
    fn record_ended(
        &mut self,
        peer: NodeAddr,
        epoch: Option<Epoch>,
        setup: Option<EphemeralDigest>,
        now: At,
        memory: Option<At>,
        p: &Params,
    ) -> Option<Duration> {
        let op = json!([
            "gate_ended",
            node(peer),
            epoch.map(hex::encode),
            setup.map(eph),
            memory.map(|i| i.0)
        ]);
        let result = self
            .inner
            .gates_mut()
            .0
            .ended(peer, epoch, setup, now, memory, p);
        self.now = now;
        capture(&self.inner, self.id, self.next, now, op, &[]);
        result
    }
}
mod upstream_tests {
    include!("table_tests.rs");
    include!("table_supplement.rs");
}
pub(super) fn traces() -> Vec<Value> {
    let mut traces = upstream_tests::run();
    traces.extend(upstream_tests::supplemental());
    traces
}
