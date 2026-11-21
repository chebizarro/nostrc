// Adapted from fips 64f5b4d482cf core/src/proto/settle/model/tests/roam.rs.
// Only imports and test registration are adapted; assertion bodies are preserved.
// Where the peer is: the peer's current address follows the path bundle of
// its link, as the node holds it (the operator's decision of 2026-11-14,
// which the README's "Where the peer is" states). FRC-1201 Sections 5.5 to
// 5.8 say nothing of a peer's address.

use super::*;
use fips_core::proto::settle::model::judge::Transition;

fn a_switch_of_the_active_path_moves_the_peer_there_and_is_no_transition() {
    // The bundle switched the link's active path from the setup address to
    // a second one; this node's frames to the peer go there now.
    let mut j = responder(LARGE, SMALL);
    let s = j.step(Event::Switch { to: at(7) });
    assert_eq!(s, Step::default(), "a switch changes no session or link");
    assert_eq!(
        j.settled().map(|(_, _, current)| current),
        Some(at(7)),
        "the peer is at the new active path"
    );
    let f = j.step(frame(10, at(7)));
    assert_eq!(
        f.change, None,
        "a frame from the active path the peer was switched to moves nothing"
    );
}

fn a_switch_with_no_peer_held_changes_nothing() {
    let mut j = judge(LARGE, SMALL);
    let s = j.step(Event::Switch { to: at(7) });
    assert_eq!(s, Step::default());
    assert_eq!(j.settled(), None, "no peer is held");
}

fn a_frame_from_a_new_address_on_the_one_paths_transport_roams_the_peer() {
    // A bundle of one path roams it inside its transport at an authentic
    // frame from a new address there (multipath v1 draft, Section 5.3).
    let mut j = responder(LARGE, SMALL);
    let f = j.step(frame(10, at(8)));
    assert_eq!(f.change, Some(Transition::Roam), "the one path roamed");
    assert_eq!(j.settled().map(|(_, _, current)| current), Some(at(8)));
}

fn a_frame_from_another_transport_moves_nothing_at_one_path() {
    // The bundle's one path is on transport 1; a frame from transport 2
    // moves no address.
    let mut j = responder(LARGE, SMALL);
    let other = Addr {
        transport: 2,
        at: 9,
    };
    let f = j.step(frame(10, other));
    assert_eq!(f.change, None, "a frame from another transport is no roam");
    assert_eq!(
        j.settled().map(|(_, _, current)| current),
        Some(at(9)),
        "the peer stays at its one path"
    );
}

/// A frame under handshake 10 from `from`, on a link whose bundle held
/// `paths` paths, a path message if `message`.
fn told(from: Addr, paths: u8, message: bool) -> Event {
    Event::Frame {
        hs: Hs(10),
        from,
        paths,
        message,
    }
}

fn a_frame_from_a_new_address_moves_nothing_while_the_link_holds_several_paths() {
    // With more than one path no frame moves an address (multipath v1
    // draft, Section 5.3).
    let mut j = responder(LARGE, SMALL);
    let f = j.step(told(at(8), 2, false));
    assert_eq!(f.change, None, "a frame is no roam at several paths");
    assert_eq!(
        j.settled().map(|(_, _, current)| current),
        Some(at(9)),
        "the peer stays at the active path"
    );
    let back = j.step(told(at(8), 1, false));
    assert_eq!(
        back.change,
        Some(Transition::Roam),
        "down to one path again, a frame roams it"
    );
}

fn a_path_message_from_a_new_address_moves_nothing_on_a_link_that_runs_multipath() {
    // A path message on a link that runs multipath moves no address by the
    // frame rule, even at one path (multipath v1 draft, Section 5.3).
    let mut j = responder(LARGE, SMALL);
    let f = j.step(told(at(8), 1, true));
    assert_eq!(f.change, None, "a path message is no roam");
    assert_eq!(
        j.settled().map(|(_, _, current)| current),
        Some(at(9)),
        "the peer stays at its one path"
    );
}

fn a_follow_of_the_path_the_peer_is_at_moves_the_peer_and_is_no_transition() {
    // The link holds two paths; a NAT rebinds the peer's address of the
    // active path, and the bundle follows a path message from there.
    let mut j = responder(LARGE, SMALL);
    let f = j.step(told(at(8), 2, true));
    assert_eq!(f.change, None, "the frame alone moves nothing");
    let s = j.step(Event::Follow {
        from: at(9),
        to: at(8),
    });
    assert_eq!(s, Step::default(), "a follow changes no session or link");
    assert_eq!(
        j.settled().map(|(_, _, current)| current),
        Some(at(8)),
        "the peer moved with the path it was at"
    );
}

fn a_follow_of_another_path_leaves_the_peer() {
    // The bundle followed its second path to a new address; the peer stays
    // at the active path.
    let mut j = responder(LARGE, SMALL);
    let s = j.step(Event::Follow {
        from: at(7),
        to: at(8),
    });
    assert_eq!(s, Step::default());
    assert_eq!(
        j.settled().map(|(_, _, current)| current),
        Some(at(9)),
        "the peer stays at the active path"
    );
}

fn a_frame_at_a_node_with_no_bundle_roams_the_peer_from_any_transport() {
    // The deployed release has no path bundle: its address rule moves the
    // peer to every authentic frame's source, on any transport.
    let mut j = responder(LARGE, SMALL);
    let other = Addr {
        transport: 2,
        at: 9,
    };
    let f = j.step(told(other, 0, false));
    assert_eq!(
        f.change,
        Some(Transition::Roam),
        "the frame roamed the peer"
    );
    assert_eq!(j.settled().map(|(_, _, current)| current), Some(other));
}

pub(super) fn run_cases() -> Vec<serde_json::Value> {
    let mut traces = Vec::new();
    super::super::begin_case(
        "model/tests/roam.rs::a_switch_of_the_active_path_moves_the_peer_there_and_is_no_transition",
    );
    a_switch_of_the_active_path_moves_the_peer_there_and_is_no_transition();
    traces.push(super::super::finish_case());
    super::super::begin_case("model/tests/roam.rs::a_switch_with_no_peer_held_changes_nothing");
    a_switch_with_no_peer_held_changes_nothing();
    traces.push(super::super::finish_case());
    super::super::begin_case(
        "model/tests/roam.rs::a_frame_from_a_new_address_on_the_one_paths_transport_roams_the_peer",
    );
    a_frame_from_a_new_address_on_the_one_paths_transport_roams_the_peer();
    traces.push(super::super::finish_case());
    super::super::begin_case(
        "model/tests/roam.rs::a_frame_from_another_transport_moves_nothing_at_one_path",
    );
    a_frame_from_another_transport_moves_nothing_at_one_path();
    traces.push(super::super::finish_case());
    super::super::begin_case(
        "model/tests/roam.rs::a_frame_from_a_new_address_moves_nothing_while_the_link_holds_several_paths",
    );
    a_frame_from_a_new_address_moves_nothing_while_the_link_holds_several_paths();
    traces.push(super::super::finish_case());
    super::super::begin_case(
        "model/tests/roam.rs::a_path_message_from_a_new_address_moves_nothing_on_a_link_that_runs_multipath",
    );
    a_path_message_from_a_new_address_moves_nothing_on_a_link_that_runs_multipath();
    traces.push(super::super::finish_case());
    super::super::begin_case(
        "model/tests/roam.rs::a_follow_of_the_path_the_peer_is_at_moves_the_peer_and_is_no_transition",
    );
    a_follow_of_the_path_the_peer_is_at_moves_the_peer_and_is_no_transition();
    traces.push(super::super::finish_case());
    super::super::begin_case("model/tests/roam.rs::a_follow_of_another_path_leaves_the_peer");
    a_follow_of_another_path_leaves_the_peer();
    traces.push(super::super::finish_case());
    super::super::begin_case(
        "model/tests/roam.rs::a_frame_at_a_node_with_no_bundle_roams_the_peer_from_any_transport",
    );
    a_frame_at_a_node_with_no_bundle_roams_the_peer_from_any_transport();
    traces.push(super::super::finish_case());
    traces
}
