# fipstop

The ncurses dashboard for a running fipsd. Component version: **0.1.0
(Unreleased)**. The UI follows upstream FIPS docs/reference/cli-fipstop.md;
the daemon control protocol, not a Nostr relay, is refreshed on a timer.

## Build and run

The top-level build discovers **ncursesw**, then **ncurses**, through pkg-config.
FIPSD_BUILD_FIPSTOP defaults ON when either is found; explicitly requesting ON
without the dependency is a configuration error. fipstop installs beside
fipsctl in the configured bindir. Prefer a UTF-8 locale and ncursesw for Unicode
box drawing, sparklines and braille graphs. A narrow-only curses build uses ASCII
fallback characters interactively (snapshot output remains UTF-8).

Package maintainers: add **libncurses-dev** on Debian/Ubuntu, **ncurses** on
Alpine, and the distro's ncurses development/pkg-config package elsewhere.
No workflow/package lists are changed by this component.

    fipstop [-s SOCKET] [--gateway-socket PATH] [-r SECONDS]

-s/--socket overrides the daemon socket. Resolution and symlink-following are
the same shared implementation as fipsctl: system runtime directory, then
XDG_RUNTIME_DIR, then /tmp/fips-control.sock. The adapter socket is the sibling
*-ipv6tun.sock. The gateway follows the same default-directory policy with
gateway.sock (fallback /tmp/fips-gateway.sock).

-r/--refresh is an integer number of seconds, default **2**. -V prints
the component version and build revision when available; --version also prints
the build target. -h/--help prints usage. --no-color or NO_COLOR disables
colours. Terminal resize redraws at the new size. q, Ctrl-C, SIGTERM or SIGHUP
restore the terminal and exit. Terminal initialization/usage failures exit 1.
An unreachable daemon is not fatal: show Disconnected and retry on refresh.

## Panes, in tab order

| Pane | Queries and content |
| --- | --- |
| Node | Always show_status; runtime, identity, counts, root, transport counts, rate/mesh/peer sparklines, TUN/transit counters. The listening panel asks the IPv6 adapter, then node fallback. |
| Peers | show_peers, show_links and show_transports joins; role/LQI-sorted peers, per-path child rows and scrollable detail. |
| Transports | show_transports, links and peers; expandable transport/link tree, instance/bind/presence columns, optional-interface policy, counters and detail. Narrow details stack vertically. |
| Sessions | show_sessions; FSP state, role, RTT/loss/SQI/MTU/activity and detail. |
| Tree | show_tree plus peer-role cross-reference; position/path, announce counters, per-peer depth/distance/effective depth/root. |
| Filters | show_bloom plus peer-role cross-reference; local/uptree state, announce counters, peer filter sequence/fill/cardinality. |
| Performance | show_mmp; independently focused/scrolled/sorted Link MMP and Session MMP panes, metric trends and engineering-unit throughput. |
| Routing | show_routing and show_cache; routing/pending/cache state, forwarding/drop/route-class/lookup/congestion/error counters. |
| Graphs | show_stats_peers, show_stats_all_history, show_stats_history_all_peers; node metrics, one metric across peers, or all metrics for one peer. |
| Gateway | show_gateway and show_mappings on the separate gateway socket; pool utilisation and mapping detail. An absent gateway is non-fatal. |

Links and Cache are cross-references, not extra tabs. Adapter status fills only
missing TUN keys; an unreachable adapter is skipped for ten subsequent fetches.
A reached adapter that cannot answer a panel request does not hide its status.

Histories and sparklines are daemon-provided samples, **already rates**.
The upstream app does not differentiate byte counters locally. Null samples
remain gaps; graph resampling uses upstream linear interpolation, independent
per-metric scales, min/max/last/sample-count summaries, and filled braille plots.
The default graph window is 10m/1s.

## Keys

- Global: **Tab / Shift-Tab** next/previous pane; **g** Graphs; **?** help;
  **Esc** close detail, otherwise deselect; **q / Ctrl-C** quit.
- Peers, Transports, Sessions, Gateway: **Up / Down** select; **Enter** detail;
  **Esc** back/deselect. Detail scrolls with Up/Down.
- Peers: **Del** confirms the displayed sorted peer; **Y** disconnects,
  **N / Esc** cancels. This is the only mutating action.
- Transports: **Right / Space**, **Left** expand/collapse selected transport;
  **e / c** expand/collapse all. Collapsing a child selects its parent.
- Tree, Filters, Routing, Performance: **f** cycles pane focus; **Up / Down**,
  **PageUp / PageDown**, **Home / End** scroll the focused pane.
- Performance: **s / S** cycles sort column / reverses direction for focused pane.
- Graphs: **Right / Space / Left** next/previous window:
  1m/1s, 10m/1s, 1h/1s, 24h/1m. **m** cycles Node → MetricByPeer → PeerByMetric.
  **n / N** next/previous metric or peer selector. **Up / Down** scroll plots,
  or select/follow a peer in MetricByPeer. **Enter / Esc** expand/close the peer
  plot. **s / S** sort/reverse the by-peer summary.

The context registry in event.c supplies footer/help hints and accepted
bindings. Scrollable details expose structured daemon fields, including joined
link/transport fields. Their compact C layout is not a byte-for-byte
transcription of ratatui's detail layout.

## Deterministic rendering and tests

Upstream has no once mode. Hidden **--render-once WxH** writes one plain UTF-8
frame without ncurses or a TTY, then exits; it uses the live socket by default.
Hidden --fixture-dir DIR, --tab NAME and comma-separated --keys are regression
inputs, deliberately omitted from user-facing help. Snapshot timing/revision
headers are deterministic; normal version output retains the build revision.

FipsTopModelTest, FipsTopTest, FipsTopGoldenTest, and FipsTopE2eTest are in the
fipsd_tests build aggregate. Goldens cover every resting pane at 80×24 and
120×40 from tests/control_golden; trailing padding is omitted in committed text
only. Tests exercise the actual renderer/client, live daemon, fake adapter
sockets and real ncurses PTY without sleeps.

Optional tests/fipstop_parity.py accepts --fipsd, --fipstop, --rust and --out.
An isolated tmux server captures both tools against the same live fipsd using
control-output events. --fixtures tests/control_golden adds a fixture-node
comparison. The private server and daemon stop before return. See
[fipstop parity](fipstop-parity.md) for scope and differences.
