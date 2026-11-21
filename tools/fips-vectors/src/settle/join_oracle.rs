//! Join routing vectors use the real pinned join and production FSP profile.
//! Native C scenarios additionally exercise authenticated IK and lease lifecycles.
use crate::support::{SeededEntropy, TestClock};
use fips_core::{
    identity::NodeAddr,
    proto::{
        fsp::rekey::FspProfile,
        path::{self, PathRef, PathState, Refusal, Switch, Why},
        rekey::{self, *},
        settle::{
            input::Input,
            join::{self, Event, Step},
            links::{LinkNo, Pair, Proven},
        },
    },
    transport::{TransportAddr, TransportId},
};
use serde_json::{Value, json};
type P = FspProfile<SeededEntropy>;
fn peer() -> NodeAddr {
    NodeAddr::from_bytes([2; 16])
}
fn path(n: u16) -> PathRef {
    PathRef {
        transport: TransportId::new(1),
        addr: TransportAddr::from_string(&format!("10.0.0.1:{n}")),
    }
}
fn port(p: &Pair) -> u16 {
    (1..=6)
        .find(|n| join::pair(&path(*n)) == *p)
        .expect("fixture pair")
}
fn read(p: &PathRef) -> Proven {
    if *p == path(2) {
        Proven::Acked
    } else {
        Proven::Live
    }
}
fn input(i: Input) -> Value {
    match i {
        Input::Restarted { peer: p, epoch } => {
            assert_eq!(p, peer());
            json!(["Restarted", hex::encode(epoch)])
        }
        Input::Ended { peer: p } => {
            assert_eq!(p, peer());
            json!(["Ended"])
        }
        Input::State { link, pair, proven } => {
            json!(["State", link.0, port(&pair), format!("{proven:?}")])
        }
        Input::Moved {
            link,
            from,
            to,
            roam,
        } => json!(["Moved", link.0, port(&from), port(&to), roam]),
        Input::Switched { link, to, proven } => {
            json!(["Switched", link.0, port(&to), format!("{proven:?}")])
        }
        Input::Released { link, pair, keep } => json!(["Released", link.0, port(&pair), keep]),
        Input::Capable { link } => json!(["Capable", link.0]),
        Input::Orphaned { peer: p } => {
            assert_eq!(p, peer());
            json!(["Orphaned"])
        }
        other => panic!("unexpected join input {other:?}"),
    }
}
fn rekey_name(o: &rekey::Output<P>) -> &'static str {
    match o {
        rekey::Output::Send { .. } => "Send",
        rekey::Output::Prompt { .. } => "Prompt",
        rekey::Output::Deliver { .. } => "Deliver",
        rekey::Output::Bind { .. } => "Bind",
        rekey::Output::Unbind { .. } => "Unbind",
        rekey::Output::Delegate { .. } => "Delegate",
        rekey::Output::Revoke { .. } => "Revoke",
        rekey::Output::Release(_) => "Release",
        rekey::Output::Installed { .. } => "Installed",
        rekey::Output::Retired { .. } => "Retired",
        rekey::Output::Reverted { .. } => "Reverted",
        rekey::Output::Rekeyed { .. } => "Rekeyed",
        rekey::Output::Reject(_) => "Reject",
        rekey::Output::Abandoned(_) => "Abandoned",
        other => panic!("not shell: {other:?}"),
    }
}
fn path_name(o: &path::Output<()>) -> &'static str {
    match o {
        path::Output::Send { .. } => "Send",
        path::Output::Moved { .. } => "Moved",
        path::Output::Released { .. } => "Released",
        path::Output::State { .. } => "State",
        path::Output::Capable => "Capable",
        path::Output::NotCapable => "NotCapable",
        path::Output::Refused { .. } => "Refused",
        path::Output::Switched { .. } => "Switched",
        path::Output::Orphaned => "Orphaned",
        path::Output::Connect { .. } => "Connect",
        path::Output::Proven { .. } => "Proven",
    }
}
fn joined(e: Event<'_, '_, '_, P, TestClock, SeededEntropy, ()>) -> Vec<Value> {
    join::join(peer(), e)
        .expect("routing cannot refuse")
        .map(|s| match s {
            Step::Core(i) => json!(["core", input(i)]),
            Step::Rekey(o) => {
                let v = json!(["rekey", rekey_name(&o)]);
                if let rekey::Output::Release(l) = o {
                    l.release();
                }
                v
            }
            Step::Path(o) => json!(["path", path_name(&o)]),
            Step::Replace(_) => json!(["replace"]),
            Step::Redial { .. } => json!(["redial"]),
        })
        .collect()
}
fn paths() -> Vec<path::Output<()>> {
    let state = |n, next| path::Output::State {
        to: path(n),
        path_id: 1,
        from: Some(PathState::Probing),
        next,
        why: Why::Answered,
        timeout: None,
    };
    vec![
        path::Output::Send {
            to: path(1),
            bytes: vec![1],
        },
        path::Output::Moved {
            from: path(1),
            to: path(3),
            driver: (),
        },
        path::Output::Released {
            to: path(4),
            driver: (),
            keep: true,
        },
        state(2, Some(PathState::Live)),
        state(5, Some(PathState::Live)),
        state(6, None),
        path::Output::Capable,
        path::Output::NotCapable,
        path::Output::Refused {
            to: path(1),
            why: Refusal::Full,
        },
        path::Output::Switched {
            from: Some(path(1)),
            to: path(5),
            why: Switch::Mandatory,
        },
        path::Output::Switched {
            from: None,
            to: path(2),
            why: Switch::Handover,
        },
        path::Output::Orphaned,
        path::Output::Connect { to: path(1) },
        path::Output::Proven {
            to: path(1),
            mtu: 1280,
        },
    ]
}
pub fn traces() -> Vec<Value> {
    let mut rows = Vec::new();
    let t = TagSource::new().draw();
    let outputs = vec![
        rekey::Output::Send {
            msg: vec![1, 2],
            kind: MsgKind::Msg1,
            route: Route::Arrival,
        },
        rekey::Output::Prompt { tag: t },
        rekey::Output::Deliver {
            slot: Slot::Current,
            tag: t,
            range: 0..1,
        },
        rekey::Output::Bind {
            transport: (),
            index: 4,
            tag: Some(t),
            purpose: Purpose::Current,
        },
        rekey::Output::Unbind {
            transport: (),
            index: 4,
        },
        rekey::Output::Revoke {
            tag: t,
            transport: (),
            index: 4,
        },
        rekey::Output::Release(IndexLease::for_test(4)),
        rekey::Output::Installed {
            tag: t,
            role: Role::Responder,
            evidence: EvidenceKind::Frame,
        },
        rekey::Output::Retired { tag: t },
        rekey::Output::Reverted { tag: t },
        rekey::Output::PeerRestarted {
            layer: Layer::Fmp,
            epoch: [6; 8],
        },
        rekey::Output::Rekeyed {
            role: Role::Initiator,
        },
        rekey::Output::Reject(Reason::Replay),
        rekey::Output::Abandoned(Abandon::Unanswered),
        rekey::Output::Teardown(Teardown::OpeningIdle),
    ];
    let names = [
        "Send",
        "Prompt",
        "Deliver",
        "Bind",
        "Unbind",
        "Revoke",
        "Release",
        "Installed",
        "Retired",
        "Reverted",
        "PeerRestarted",
        "Rekeyed",
        "Reject",
        "Abandoned",
        "Teardown",
    ];
    for (o, name) in outputs.into_iter().zip(names) {
        let out = joined(Event::Rekey { out: vec![o] });
        rows.push(json!({"op":["rekey",name],"out":out}));
    }
    for link in [None, Some(LinkNo(7))] {
        for released in [None, Some(LinkNo(3))] {
            for (index, o) in paths().into_iter().enumerate() {
                let out = joined(Event::Paths {
                    out: vec![o],
                    read: &read,
                    link,
                    released,
                });
                rows.push(
                    json!({"op":["paths",index,link.map(|l|l.0),released.map(|l|l.0)],"out":out}),
                );
            }
        }
    }
    for from in [false, true] {
        for standby in [false, true] {
            let out = joined(Event::Roam {
                link: LinkNo(7),
                from: from.then(|| join::pair(&path(1))),
                to: join::pair(&path(2)),
                standby,
            });
            rows.push(json!({"op":["roam",from,standby],"out":out}));
        }
    }
    let out = joined(Event::Paths {
        out: paths(),
        read: &read,
        link: Some(LinkNo(7)),
        released: Some(LinkNo(3)),
    });
    rows.push(json!({"op":["path_sequence"],"out":out}));
    vec![json!({"kind":"join","rows":rows})]
}
