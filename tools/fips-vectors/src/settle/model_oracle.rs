//! Executes all pinned independent-model assertion bodies and records their
//! abstract histories. No production settlement core is used by this oracle.
use fips_core::proto::settle::model::{
    history,
    judge::{Judge as CoreJudge, Params, Step},
};
use history::{Addr, Binding, Event};
use serde_json::{Value, json};
use std::{cell::RefCell, ops::Deref};
thread_local! {
    static ROWS:RefCell<Vec<Value>>=const { RefCell::new(Vec::new()) };
    static CASE:RefCell<String>=const { RefCell::new(String::new()) };
    static NEXT:RefCell<usize>=const { RefCell::new(0) };
}
fn begin_case(name: &str) {
    ROWS.with(|r| r.borrow_mut().clear());
    CASE.with(|r| *r.borrow_mut() = name.into());
    NEXT.with(|r| *r.borrow_mut() = 0);
}
fn finish_case() -> Value {
    json!({"kind":"model","test":CASE.with(|r|r.borrow().clone()),"rows":ROWS.with(|r|std::mem::take(&mut *r.borrow_mut()))})
}
fn push(row: Value) {
    ROWS.with(|r| r.borrow_mut().push(row));
}
fn addr(a: Addr) -> Value {
    json!([a.transport, a.at])
}
fn binding(b: Binding) -> bool {
    b == Binding::Connected
}
fn leg(l: history_model::Leg) -> Value {
    json!([
        l.hs.0,
        l.bytes.0,
        l.epoch.0,
        l.link.0,
        addr(l.from),
        binding(l.binding)
    ])
}
use fips_core::proto::settle::model::judge as history_model;
fn snapshot(j: &CoreJudge) -> Value {
    let peering = j.peering.as_ref().map(|p| {
        json!([
            p.hs.0,
            p.epoch.0,
            p.link.0,
            binding(p.binding),
            addr(p.setup),
            p.dialed,
            p.msg1.map(|b| b.0),
            addr(p.current),
            p.installed,
            p.linkborn,
            p.sessionborn,
            p.heard,
            p.last,
            p.pending.map(|pd| json!([
                pd.hs.0,
                pd.initiator,
                pd.armed.map(|b| b.0),
                pd.restarted.map(|e| e.0)
            ])),
            p.fallback.map(|f| json!([
                f.hs.0,
                f.link.0,
                binding(f.binding),
                addr(f.setup),
                addr(f.current)
            ])),
            p.record.iter().map(|h| h.0).collect::<Vec<_>>()
        ])
    });
    json!({"me":j.me.0,"peer":j.peer.0,"params":[j.params.unheard,j.params.interval,j.params.linkage,j.params.sessionage],"now":j.now,"peering":peering,
        "attempts":j.attempts.iter().map(|a|json!([a.hs.0,a.link.0,addr(a.to),binding(a.binding),a.aged])).collect::<Vec<_>>(),"legs":j.legs.iter().copied().map(leg).collect::<Vec<_>>(),
        "redial":j.redial.map(|r|json!([leg(r.leg),r.at,r.sent])),"rekey":j.rekey.map(|h|h.0),"stamp":j.stamp,"closed":j.closed.iter().copied().map(addr).collect::<Vec<_>>(),
        "settled":j.settled().map(|(h,l,a)|json!([h.0,l.0,addr(a)])),"needs":j.needs().iter().map(|(l,a)|json!([l.0,addr(*a)])).collect::<Vec<_>>()})
}
fn event(e: Event) -> Value {
    match e {
        Event::Advance(ms) => json!([0, ms]),
        Event::Dial {
            hs,
            link,
            to,
            binding: b,
        } => json!([1, hs.0, link.0, addr(to), binding(b)]),
        Event::Msg1 {
            hs,
            bytes,
            epoch,
            link,
            from,
            binding: b,
            sent,
        } => json!([
            2,
            hs.0,
            bytes.0,
            epoch.0,
            link.0,
            addr(from),
            binding(b),
            sent
        ]),
        Event::Msg2Sent { hs } => json!([3, hs.0]),
        Event::Msg2 { hs, epoch, from } => json!([4, hs.0, epoch.0, addr(from)]),
        Event::Timeout { hs } => json!([5, hs.0]),
        Event::Frame {
            hs,
            from,
            paths,
            message,
        } => json!([6, hs.0, addr(from), paths, message]),
        Event::Switch { to } => json!([7, addr(to)]),
        Event::Follow { from, to } => json!([8, addr(from), addr(to)]),
        Event::RekeyStart { hs } => json!([9, hs.0]),
        Event::RekeyMsg2 { hs, epoch } => json!([10, hs.0, epoch.0]),
        Event::RekeyCutover => json!([11]),
        Event::Close { at } => json!([12, addr(at)]),
        Event::Lost => json!([13]),
        Event::Restart => json!([14]),
    }
}
fn step(s: &Step) -> Value {
    json!([
        s.class.map(|x| format!("{x:?}")),
        s.refused.map(|x| format!("{x:?}")),
        s.reply.map(|x| format!("{x:?}")),
        s.change.map(|x| format!("{x:?}")),
        s.closes.iter().copied().map(addr).collect::<Vec<_>>(),
        s.disconnect,
        s.rule.map(|x| format!("{x:?}")),
        s.open
    ])
}
#[derive(Debug)]
struct Judge {
    inner: CoreJudge,
    id: usize,
}
impl Deref for Judge {
    type Target = CoreJudge;
    fn deref(&self) -> &CoreJudge {
        &self.inner
    }
}
impl Drop for Judge {
    fn drop(&mut self) {
        push(json!({"id":self.id,"op":["drop"]}));
    }
}
impl Judge {
    fn new(me: history::Node, peer: history::Node, params: Params) -> Self {
        let id = NEXT.with(|n| {
            let id = *n.borrow();
            *n.borrow_mut() += 1;
            id
        });
        let inner = CoreJudge::new(me, peer, params);
        push(
            json!({"id":id,"op":["new",me.0,peer.0,[params.unheard,params.interval,params.linkage,params.sessionage]],"state":snapshot(&inner)}),
        );
        Self { inner, id }
    }
    fn step(&mut self, e: Event) -> Step {
        let out = self.inner.step(e);
        push(
            json!({"id":self.id,"op":["step",event(e)],"out":step(&out),"state":snapshot(&self.inner)}),
        );
        out
    }
    fn run(&mut self, history: &[Event]) -> Vec<Step> {
        history.iter().map(|e| self.step(*e)).collect()
    }
}
mod upstream_tests {
    use super::Value;
    include!("model_tests/mod.rs");
    mod classify {
        include!("model_tests/classify.rs");
    }
    mod close {
        include!("model_tests/close.rs");
    }
    mod crossing {
        include!("model_tests/crossing.rs");
    }
    mod gates {
        include!("model_tests/gates.rs");
    }
    mod redial {
        include!("model_tests/redial.rs");
    }
    mod roam {
        include!("model_tests/roam.rs");
    }
    pub(super) fn run() -> Vec<Value> {
        let mut out = Vec::new();
        out.extend(classify::run_cases());
        out.extend(close::run_cases());
        out.extend(crossing::run_cases());
        out.extend(gates::run_cases());
        out.extend(redial::run_cases());
        out.extend(roam::run_cases());
        out
    }
}
pub(super) fn record_gate_history(name: &str, history: &[Event]) -> Value {
    begin_case(&format!(
        "tests/gates.rs::the_gates_agree_with_the_model_on_every_gate_history/{name}"
    ));
    {
        let mut j = Judge::new(history::Node(1), history::Node(2), Params::default());
        j.run(history);
    }
    let mut out = finish_case();
    out["kind"] = json!("gate_model");
    out
}
pub(super) fn traces() -> Vec<Value> {
    upstream_tests::run()
}
