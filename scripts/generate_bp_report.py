#!/usr/bin/env python3
import argparse, csv
from pathlib import Path

def read_csv(path: Path):
    rows = []
    with open(path) as f:
        r = csv.DictReader(f)
        for row in r:
            rows.append(row)
    return rows

TEMPLATE = """# Backpressure Capacity Matrix Report

- Base: {base}
- Modes: {modes}
- Capacities: {caps}

## Summary CSV

- Matrix summary: [{summary_csv}]({summary_csv})
- Correlations: [{corr_csv}]({corr_csv})

## Key Comparisons (SVG)

- Send failure ratio vs capacity: ![]({prefix}_send_fail_ratio.svg)
- Send wait p50 vs capacity: ![]({prefix}_send_wait_p50.svg)
- Send wait p90 vs capacity: ![]({prefix}_send_wait_p90.svg)
- Send wait p99 vs capacity: ![]({prefix}_send_wait_p99.svg)
- Recv wait p50 vs capacity: ![]({prefix}_recv_wait_p50.svg)
- Recv wait p90 vs capacity: ![]({prefix}_recv_wait_p90.svg)
- Recv wait p99 vs capacity: ![]({prefix}_recv_wait_p99.svg)
- Block sends per second vs capacity: ![]({prefix}_block_sends.svg)
- Send avg depth vs capacity: ![]({prefix}_send_avg_depth.svg)
- Recv avg depth vs capacity: ![]({prefix}_recv_avg_depth.svg)

### Wakeup Productivity

- Send spurious wakeup ratio vs capacity: ![]({prefix}_send_spurious_ratio.svg)
- Recv spurious wakeup ratio vs capacity: ![]({prefix}_recv_spurious_ratio.svg)
- Send wakeups per second vs capacity: ![]({prefix}_send_wakeups.svg)
- Recv wakeups per second vs capacity: ![]({prefix}_recv_wakeups.svg)

## Per-mode observations

{mode_sections}

"""

MODE_SECTION_TMPL = """### Mode: {mode}

- Capacities covered: {caps}
- Avg tx msgs/s by cap: {tx_series}
- Avg send failure ratio by cap: {fail_series}
- Avg block_sends/s by cap: {block_series}
- Avg send spurious ratio by cap: {send_spur_series}
- Avg recv spurious ratio by cap: {recv_spur_series}

"""

def generate(base_dir: Path, out_md: Path):
    base_dir = base_dir.resolve()
    summary_csv = base_dir / 'summary_matrix_summary.csv'
    corr_csv = base_dir / 'summary_correlations.csv'
    if not summary_csv.exists():
        raise SystemExit(f"Missing {summary_csv}. Run aggregate_bp_matrix.py first.")
    rows = read_csv(summary_csv)
    if not rows:
        raise SystemExit("Empty summary CSV")
    caps = sorted({int(r['cap']) for r in rows})
    modes = sorted({r['mode'] for r in rows})
    # build per-mode maps
    by_mode = {m: [] for m in modes}
    for r in rows:
        r['cap'] = int(r['cap'])
        # cast floats
        for k in ['tx_msgs_avg','go_chan_send_failure_ratio_avg','go_chan_block_sends_per_s_avg',
                  'go_chan_send_wait_spurious_ratio_avg','go_chan_recv_wait_spurious_ratio_avg']:
            try:
                r[k] = float(r.get(k, 0) or 0)
            except Exception:
                r[k] = 0.0
        by_mode[r['mode']].append(r)
    for m in modes:
        by_mode[m].sort(key=lambda x: x['cap'])
    # mode sections
    mode_sections = []
    for m in modes:
        arr = by_mode[m]
        tx = [f"{e['tx_msgs_avg']:.1f}" for e in arr]
        fail = [f"{e['go_chan_send_failure_ratio_avg']:.3f}" for e in arr]
        block = [f"{e['go_chan_block_sends_per_s_avg']:.1f}" for e in arr]
        send_spur = [f"{e.get('go_chan_send_wait_spurious_ratio_avg',0.0):.3f}" for e in arr]
        recv_spur = [f"{e.get('go_chan_recv_wait_spurious_ratio_avg',0.0):.3f}" for e in arr]
        mode_sections.append(MODE_SECTION_TMPL.format(
            mode=m,
            caps=", ".join(str(e['cap']) for e in arr),
            tx_series=", ".join(tx),
            fail_series=", ".join(fail),
            block_series=", ".join(block),
            send_spur_series=", ".join(send_spur),
            recv_spur_series=", ".join(recv_spur),
        ))
    content = TEMPLATE.format(
        base=base_dir,
        modes=", ".join(modes),
        caps=", ".join(map(str, caps)),
        summary_csv=summary_csv.relative_to(base_dir),
        corr_csv=corr_csv.relative_to(base_dir),
        prefix=(base_dir / 'summary').relative_to(base_dir),
        mode_sections="\n".join(mode_sections)
    )
    out_md.write_text(content)
    print(f"Report written: {out_md}")


def main():
    ap = argparse.ArgumentParser(description='Generate Markdown report for backpressure matrix outputs')
    ap.add_argument('--base', required=True, help='base directory with aggregated outputs (e.g., build/bp_matrix)')
    ap.add_argument('--out', default=None, help='output markdown path (default: <base>/REPORT.md)')
    args = ap.parse_args()
    base = Path(args.base)
    out_md = Path(args.out) if args.out else base / 'REPORT.md'
    generate(base, out_md)

if __name__ == '__main__':
    main()
