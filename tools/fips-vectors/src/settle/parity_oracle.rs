//! The pinned parity vocabulary/parser is an oracle, not a node/world runner.
use fips_core::proto::settle::parity::*;
use serde_json::{Value as Json, json};
#[path = "parity_tests.rs"]
mod original;
fn parsed(text: &str) -> Json {
    match read(text) {Err(_)=>Json::Null,Ok(ts)=>json!(ts.into_iter().map(|t|{
 let cored=t.cored().ok().map(|lines|lines.iter().map(ToString::to_string).collect::<Vec<_>>());
 json!({"name":t.name,"settings":t.settings.iter().map(|s|s.enabled()).collect::<Vec<_>>(),"lines":t.lines.iter().map(ToString::to_string).collect::<Vec<_>>(),"core":t.core.iter().map(ToString::to_string).collect::<Vec<_>>(),"cored":cored})}).collect::<Vec<_>>())}
}
fn step(s: Step) -> Json {
    match s {
        Step::Dial {
            from,
            to,
            transport,
        } => json!(["Dial", from, to, transport.name()]),
        Step::Connected { node } => json!(["Connected", node]),
        Step::Handshake { to, kind, nth } => json!(["Handshake", to, format!("{kind:?}"), nth]),
        Step::Heal => json!(["Heal"]),
        Step::Advance(ms) => json!(["Advance", ms]),
        Step::Restart { node } => json!(["Restart", node]),
        Step::Rekey { node } => json!(["Rekey", node]),
    }
}
pub fn traces() -> Vec<Json> {
    original::run();
    let scripts:Vec<_>=SCRIPTS.iter().map(|s|{assert!(strays(s).is_empty());json!({"name":s.name,"settings":s.settings.iter().map(|s|s.enabled()).collect::<Vec<_>>(),"smaller":s.smaller,"transports":s.transports.iter().map(|t|t.name()).collect::<Vec<_>>(),"steps":s.steps.iter().copied().map(step).collect::<Vec<_>>()})}).collect();
    let mut rows = Vec::new();
    for text in [
        "none",
        "none heard",
        "held own out udp-home udp-home heard",
        "held peer in tcp-second ble-accepted unheard",
        "held own in udp-home udp-home",
        "held own in udp-home udp-home heard extra",
        "held out own udp-home udp-home heard",
        "held own in invalid-home udp-home heard",
        "",
    ] {
        rows.push(json!({"op":["mark",text],"out":Mark::read(text).ok().map(|m|m.to_string())}));
    }
    for text in TRACE
        .lines()
        .map(str::trim_end)
        .filter(|l| !l.is_empty() && !l.starts_with('#') && !l.starts_with("script "))
    {
        rows.push(json!({"op":["line",text],"out":line(text).ok().map(|l|l.to_string())}));
    }
    let held = "5 n0 held own out udp-home udp-home heard";
    let words: Vec<_> = held.split(' ').collect();
    for gone in 3..words.len() {
        let short = words
            .iter()
            .enumerate()
            .filter(|(i, _)| *i != gone)
            .map(|(_, w)| *w)
            .collect::<Vec<_>>()
            .join(" ");
        rows.push(json!({"op":["line",short],"out":line(&short).ok().map(|l|l.to_string())}));
    }
    for text in [
        "core 5 n0 dir=in cause=late-dial-split",
        "core 5 n0 dir=out cause=aged-link-pending",
        "core 5 n0 dir=out cause=the-node-is-wrong",
        "core 5 n0 dir=out",
        "core 5 n0 dir=out cause=",
        "core 5 n0 cause=late-dial-split",
        "core 5 n0 dir=in dir=out cause=late-dial-split",
        "core +5 n+0 heard=unheard active=ble-second cause=aged-link-pending",
        "core 5 n0 held=yes session=own dir=out setup=udp-home active=tcp-home heard=heard cause=late-dial-split",
    ] {
        rows.push(json!({"op":["diff",text],"out":differs(text).ok().map(|d|d.to_string())}));
    }
    for text in [
        TRACE.to_owned(),
        TRACE.replacen(
            held,
            &format!("{held}\ncore 5 n0 dir=in cause=late-dial-split"),
            1,
        ),
        TRACE.replacen(
            "1 n0 none",
            "1 n0 none\ncore 1 n0 held=yes cause=late-dial-split",
            1,
        ),
        TRACE.replacen(
            held,
            &format!("{held}\ncore 500 n0 dir=in cause=late-dial-split"),
            1,
        ),
        "1 n0 none".to_owned(),
        "script x off bogus\n1 n0 none".to_owned(),
        "script x off off\ncore 1 n0 dir=in cause=unknown".to_owned(),
        "# comment\n\n".to_owned(),
    ] {
        let out = parsed(&text);
        rows.push(json!({"op":["read",text],"out":out}));
    }
    let settings:Vec<_>=[false,true].into_iter().map(|on|{let r=rekey(on);let r=r.config();let p=path(on);let p=p.config();json!({"on":on,"rekey":{"after":r.after.as_millis(),"check":r.check.as_millis(),"resends":r.resends,"handshake":r.handshake.as_millis(),"hold":r.hold.as_millis(),"arrival":r.arrival,"floor_link":r.floor_link.as_millis(),"floor_session":r.floor_session.as_millis()},"path":{"enabled":p.enabled,"mix_kinds":p.mix_kinds,"heartbeat":p.heartbeat.as_millis()}})}).collect();
    vec![
        json!({"kind":"parity","scripts":scripts,"unshared":UNSHARED,"settings":settings,"rows":rows}),
    ]
}
