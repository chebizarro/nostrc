// New boundary history found by the C guard-mutation sweep. Executes against
// pinned upstream; it supplements, and does not alter, the78 original tests.
pub(super) fn supplemental() -> Vec<Value> {
    begin_case("replacement_request_observes_the_heard_floor");
    {
        let mut table = Table::new(small(), Params::default());
        let initial = hi(&mut table, large(), 1, 9, 0);
        let first = asked(&initial).expect("the initial msg1 was admitted");
        go(&mut table, Input::Sent { link: first }, 0);
        go(
            &mut table,
            Input::Heard {
                peer: large(),
                from: pair(9),
                kind: heartbeat(),
            },
            100,
        );
        let refused = go(&mut table, Input::Replace(read(large(), 2, 9, E1)), 200);
        assert!(matches!(
            refused.as_slice(),
            [Output::Reject {
                why: Denial::Gate(Refusal::Heard),
                ..
            }]
        ));
        assert_eq!(table.held(&large()).map(|h| h.link), Some(first));
        let admitted = greet(
            &mut table,
            Input::Replace(read(large(), 2, 9, E1)),
            2,
            15_100,
        );
        let next = asked(&admitted).expect("the exact heard-floor boundary admits");
        go(&mut table, Input::Sent { link: next }, 15_100);
        assert_eq!(table.held(&large()).map(|h| h.link), Some(next));
    }
    vec![finish_case()]
}
