#!/usr/bin/env python3
import argparse, csv, json
from pathlib import Path
from statistics import mean

def read_rates_csv(path: Path):
    rows = []
    with open(path) as f:
        r = csv.DictReader(f)
        for row in r:
            rows.append({k: float(v) if v not in (None, '') else 0.0 for k,v in row.items() if k != 'interval_index'})
    return rows

def read_percentiles_csv(path: Path):
    pct = {}
    with open(path) as f:
        r = csv.DictReader(f)
        for row in r:
            name = row['metric']
            try:
                pct[name] = {
                    'p50_ns': int(row.get('p50_ns',0)),
                    'p90_ns': int(row.get('p90_ns',0)),
                    'p99_ns': int(row.get('p99_ns',0)),
                }
            except Exception:
                pct[name] = {'p50_ns':0,'p90_ns':0,'p99_ns':0}
    return pct

def read_depth_avgs(metrics_jsonl: Path):
    # compute avg depths from last counters snapshot
    last = None
    with open(metrics_jsonl) as f:
        for line in f:
            line = line.strip()
            if not line.startswith('{'):
                continue
            try:
                obj = json.loads(line)
                last = obj
            except Exception:
                pass
    send_avg_depth = recv_avg_depth = 0.0
    if last and 'counters' in last:
        c = last['counters']
        ss = c.get('go_chan_send_depth_samples', 0)
        rs = c.get('go_chan_recv_depth_samples', 0)
        if ss:
            send_avg_depth = c.get('go_chan_send_depth_sum', 0) / ss
        if rs:
            recv_avg_depth = c.get('go_chan_recv_depth_sum', 0) / rs
    return send_avg_depth, recv_avg_depth


def write_svg_xy(series_map, x_values, filename, title, y_label=""):
    width, height = 900, 360
    margin = 50
    plot_w, plot_h = width - 2*margin, height - 2*margin
    xs = list(x_values)
    if not xs:
        return
    max_x = max(xs)
    max_y = max((max(vals) if vals else 0.0) for vals in series_map.values())
    max_y = max_y if max_y > 0 else 1.0
    colors = ["#1f77b4","#ff7f0e","#2ca02c","#d62728","#9467bd","#8c564b","#e377c2","#7f7f7f","#bcbd22","#17becf"]
    def sx(x):
        return margin + (x/(max_x if max_x>0 else 1.0))*plot_w
    def sy(y):
        return margin + (1 - y/max_y)*plot_h
    lines = []
    # axes
    lines.append(f'<line x1="{margin}" y1="{margin+plot_h}" x2="{margin+plot_w}" y2="{margin+plot_h}" stroke="#000" stroke-width="1"/>')
    lines.append(f'<line x1="{margin}" y1="{margin}" x2="{margin}" y2="{margin+plot_h}" stroke="#000" stroke-width="1"/>')
    # y ticks
    for t in range(6):
        yv = (t/5)*max_y
        yy = sy(yv)
        lines.append(f'<line x1="{margin-4}" y1="{yy}" x2="{margin}" y2="{yy}" stroke="#000" stroke-width="1"/>')
        lines.append(f'<text x="{margin-8}" y="{yy+4}" font-size="10" text-anchor="end">{yv:.2f}</text>')
    # x ticks
    for i,x in enumerate(xs):
        xx = sx(x)
        lines.append(f'<line x1="{xx}" y1="{margin+plot_h}" x2="{xx}" y2="{margin+plot_h+4}" stroke="#000" stroke-width="1"/>')
        lines.append(f'<text x="{xx}" y="{margin+plot_h+16}" font-size="10" text-anchor="middle">{x}</text>')
    # series
    for idx,(name, vals) in enumerate(series_map.items()):
        pts = " ".join(f"{sx(xs[i]):.1f},{sy(vals[i]):.1f}" for i in range(len(xs)))
        color = colors[idx % len(colors)]
        lines.append(f'<polyline fill="none" stroke="{color}" stroke-width="1.5" points="{pts}"/>')
    # legend
    lx, ly = margin+10, margin+10
    for idx, name in enumerate(series_map.keys()):
        color = colors[idx % len(colors)]
        lines.append(f'<rect x="{lx}" y="{ly+idx*16-8}" width="10" height="10" fill="{color}"/>')
        lines.append(f'<text x="{lx+16}" y="{ly+idx*16}" font-size="11">{name}</text>')
    # title & labels
    lines.append(f'<text x="{width/2}" y="{margin-14}" font-size="12" text-anchor="middle">{title}</text>')
    lines.append(f'<text x="{margin-36}" y="{margin-14}" font-size="11" text-anchor="start">{y_label}</text>')
    svg = f"<svg xmlns='http://www.w3.org/2000/svg' width='{width}' height='{height}'>" + ''.join(lines) + "</svg>"
    with open(filename, 'w') as f:
        f.write(svg)


def aggregate(base_dir: Path, out_prefix: Path):
    rows = []
    # Iterate modes and capacities
    for mode_dir in sorted(base_dir.glob('mode_*')):
        mode = mode_dir.name.split('_',1)[1]
        for cap_dir in sorted(mode_dir.glob('cap_*')):
            try:
                cap = int(cap_dir.name.split('_',1)[1])
            except Exception:
                continue
            rates_path = cap_dir / 'cap_rates.csv'
            pct_path = cap_dir / 'cap_percentiles.csv'
            mjsonl = cap_dir / 'metrics.jsonl'
            if not rates_path.exists() or not pct_path.exists() or not mjsonl.exists():
                continue
            rates_rows = read_rates_csv(rates_path)
            if not rates_rows:
                continue
            # fields of interest (averages over intervals)
            def avg_field(name):
                vals = [r.get(name,0.0) for r in rates_rows]
                return mean(vals) if vals else 0.0
            rx_msgs = avg_field('rx_msgs_per_s')
            tx_msgs = avg_field('tx_msgs_per_s')
            bp_gen = avg_field('bp_events_generated_per_s')
            send_fail_ratio = avg_field('go_chan_send_failure_ratio')
            block_sends = avg_field('go_chan_block_sends_per_s')
            signal_empty = avg_field('go_chan_signal_empty_per_s')
            # wakeup productivity averages (per second)
            s_wake = avg_field('go_chan_send_wait_wakeups_per_s')
            s_spur = avg_field('go_chan_send_wait_spurious_per_s')
            s_prod = avg_field('go_chan_send_wait_productive_per_s')
            r_wake = avg_field('go_chan_recv_wait_wakeups_per_s')
            r_spur = avg_field('go_chan_recv_wait_spurious_per_s')
            r_prod = avg_field('go_chan_recv_wait_productive_per_s')
            # ratios (handle divide-by-zero)
            s_spur_ratio = (s_spur / s_wake) if s_wake > 0 else 0.0
            r_spur_ratio = (r_spur / r_wake) if r_wake > 0 else 0.0
            # percentiles
            pct = read_percentiles_csv(pct_path)
            sw_p50 = pct.get('go_chan_send_wait_ns',{}).get('p50_ns',0)
            sw_p90 = pct.get('go_chan_send_wait_ns',{}).get('p90_ns',0)
            sw_p99 = pct.get('go_chan_send_wait_ns',{}).get('p99_ns',0)
            rw_p50 = pct.get('go_chan_recv_wait_ns',{}).get('p50_ns',0)
            rw_p90 = pct.get('go_chan_recv_wait_ns',{}).get('p90_ns',0)
            rw_p99 = pct.get('go_chan_recv_wait_ns',{}).get('p99_ns',0)
            # wakeup-to-progress latency percentiles
            swp_p50 = pct.get('go_chan_send_wakeup_to_progress_ns',{}).get('p50_ns',0)
            swp_p90 = pct.get('go_chan_send_wakeup_to_progress_ns',{}).get('p90_ns',0)
            swp_p99 = pct.get('go_chan_send_wakeup_to_progress_ns',{}).get('p99_ns',0)
            rwp_p50 = pct.get('go_chan_recv_wakeup_to_progress_ns',{}).get('p50_ns',0)
            rwp_p90 = pct.get('go_chan_recv_wakeup_to_progress_ns',{}).get('p90_ns',0)
            rwp_p99 = pct.get('go_chan_recv_wakeup_to_progress_ns',{}).get('p99_ns',0)
            # depth avgs
            send_avg_depth, recv_avg_depth = read_depth_avgs(mjsonl)
            rows.append({
                'mode': mode,
                'cap': cap,
                'rx_msgs_avg': rx_msgs,
                'tx_msgs_avg': tx_msgs,
                'bp_events_generated_avg': bp_gen,
                'go_chan_send_failure_ratio_avg': send_fail_ratio,
                'go_chan_block_sends_per_s_avg': block_sends,
                'go_chan_signal_empty_per_s_avg': signal_empty,
                # wakeup productivity (send/recv)
                'go_chan_send_wait_wakeups_per_s_avg': s_wake,
                'go_chan_send_wait_spurious_per_s_avg': s_spur,
                'go_chan_send_wait_productive_per_s_avg': s_prod,
                'go_chan_send_wait_spurious_ratio_avg': s_spur_ratio,
                'go_chan_recv_wait_wakeups_per_s_avg': r_wake,
                'go_chan_recv_wait_spurious_per_s_avg': r_spur,
                'go_chan_recv_wait_productive_per_s_avg': r_prod,
                'go_chan_recv_wait_spurious_ratio_avg': r_spur_ratio,
                'go_chan_send_wait_p50_ns': sw_p50,
                'go_chan_send_wait_p90_ns': sw_p90,
                'go_chan_send_wait_p99_ns': sw_p99,
                'go_chan_recv_wait_p50_ns': rw_p50,
                'go_chan_recv_wait_p90_ns': rw_p90,
                'go_chan_recv_wait_p99_ns': rw_p99,
                'go_chan_send_wakeup_to_progress_p50_ns': swp_p50,
                'go_chan_send_wakeup_to_progress_p90_ns': swp_p90,
                'go_chan_send_wakeup_to_progress_p99_ns': swp_p99,
                'go_chan_recv_wakeup_to_progress_p50_ns': rwp_p50,
                'go_chan_recv_wakeup_to_progress_p90_ns': rwp_p90,
                'go_chan_recv_wakeup_to_progress_p99_ns': rwp_p99,
                'send_avg_depth': send_avg_depth,
                'recv_avg_depth': recv_avg_depth,
            })
    # write CSV
    if not rows:
        print("No rows aggregated.")
        return 1
    rows.sort(key=lambda r: (r['mode'], r['cap']))
    fields = list(rows[0].keys())
    with open(f"{out_prefix}_matrix_summary.csv", 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for r in rows:
            w.writerow(r)
    # plots per metric vs cap, grouping by mode
    # collect caps per mode
    by_mode = {}
    for r in rows:
        by_mode.setdefault(r['mode'], []).append(r)
    # ensure sorted by cap
    for m in by_mode:
        by_mode[m].sort(key=lambda r: r['cap'])
    # series maps: name -> y-values aligned with x caps
    caps_union = sorted(set(r['cap'] for r in rows))
    def build_series(metric):
        series = {}
        for m, arr in by_mode.items():
            y = []
            arr_map = {e['cap']: e for e in arr}
            for c in caps_union:
                y.append(arr_map.get(c, {}).get(metric, 0.0))
            series[m] = y
        return series
    # create svgs
    base = out_prefix
    write_svg_xy(build_series('go_chan_send_failure_ratio_avg'), caps_union, f"{base}_send_fail_ratio.svg", "Send Failure Ratio vs Capacity", "ratio")
    write_svg_xy(build_series('go_chan_send_wait_p50_ns'), caps_union, f"{base}_send_wait_p50.svg", "Send Wait p50 (ns) vs Capacity", "ns")
    write_svg_xy(build_series('go_chan_send_wait_p90_ns'), caps_union, f"{base}_send_wait_p90.svg", "Send Wait p90 (ns) vs Capacity", "ns")
    write_svg_xy(build_series('go_chan_send_wait_p99_ns'), caps_union, f"{base}_send_wait_p99.svg", "Send Wait p99 (ns) vs Capacity", "ns")
    write_svg_xy(build_series('go_chan_recv_wait_p50_ns'), caps_union, f"{base}_recv_wait_p50.svg", "Recv Wait p50 (ns) vs Capacity", "ns")
    write_svg_xy(build_series('go_chan_recv_wait_p90_ns'), caps_union, f"{base}_recv_wait_p90.svg", "Recv Wait p90 (ns) vs Capacity", "ns")
    write_svg_xy(build_series('go_chan_recv_wait_p99_ns'), caps_union, f"{base}_recv_wait_p99.svg", "Recv Wait p99 (ns) vs Capacity", "ns")
    # wakeup-to-progress plots
    write_svg_xy(build_series('go_chan_send_wakeup_to_progress_p50_ns'), caps_union, f"{base}_send_wakeup_to_progress_p50.svg", "Send Wake->Progress p50 (ns) vs Capacity", "ns")
    write_svg_xy(build_series('go_chan_send_wakeup_to_progress_p90_ns'), caps_union, f"{base}_send_wakeup_to_progress_p90.svg", "Send Wake->Progress p90 (ns) vs Capacity", "ns")
    write_svg_xy(build_series('go_chan_send_wakeup_to_progress_p99_ns'), caps_union, f"{base}_send_wakeup_to_progress_p99.svg", "Send Wake->Progress p99 (ns) vs Capacity", "ns")
    write_svg_xy(build_series('go_chan_recv_wakeup_to_progress_p50_ns'), caps_union, f"{base}_recv_wakeup_to_progress_p50.svg", "Recv Wake->Progress p50 (ns) vs Capacity", "ns")
    write_svg_xy(build_series('go_chan_recv_wakeup_to_progress_p90_ns'), caps_union, f"{base}_recv_wakeup_to_progress_p90.svg", "Recv Wake->Progress p90 (ns) vs Capacity", "ns")
    write_svg_xy(build_series('go_chan_recv_wakeup_to_progress_p99_ns'), caps_union, f"{base}_recv_wakeup_to_progress_p99.svg", "Recv Wake->Progress p99 (ns) vs Capacity", "ns")
    write_svg_xy(build_series('go_chan_block_sends_per_s_avg'), caps_union, f"{base}_block_sends.svg", "Block Sends per Second vs Capacity", "ops/s")
    write_svg_xy(build_series('send_avg_depth'), caps_union, f"{base}_send_avg_depth.svg", "Send Avg Depth vs Capacity", "items")
    write_svg_xy(build_series('recv_avg_depth'), caps_union, f"{base}_recv_avg_depth.svg", "Recv Avg Depth vs Capacity", "items")
    # wakeup productivity plots
    write_svg_xy(build_series('go_chan_send_wait_spurious_ratio_avg'), caps_union, f"{base}_send_spurious_ratio.svg", "Send Wait Spurious Wakeup Ratio vs Capacity", "ratio")
    write_svg_xy(build_series('go_chan_recv_wait_spurious_ratio_avg'), caps_union, f"{base}_recv_spurious_ratio.svg", "Recv Wait Spurious Wakeup Ratio vs Capacity", "ratio")
    write_svg_xy(build_series('go_chan_send_wait_wakeups_per_s_avg'), caps_union, f"{base}_send_wakeups.svg", "Send Wait Wakeups per Second vs Capacity", "ops/s")
    write_svg_xy(build_series('go_chan_recv_wait_wakeups_per_s_avg'), caps_union, f"{base}_recv_wakeups.svg", "Recv Wait Wakeups per Second vs Capacity", "ops/s")
    # correlations per mode vs capacity
    def pearson(xs, ys):
        n = len(xs)
        if n < 2:
            return 0.0
        mx = sum(xs)/n
        my = sum(ys)/n
        num = sum((xs[i]-mx)*(ys[i]-my) for i in range(n))
        denx = sum((x-mx)**2 for x in xs)
        deny = sum((y-my)**2 for y in ys)
        if denx <= 0 or deny <= 0:
            return 0.0
        return num / (denx**0.5 * deny**0.5)
    metrics = [
        'go_chan_send_failure_ratio_avg',
        'go_chan_send_wait_p50_ns',
        'go_chan_send_wait_p90_ns',
        'go_chan_send_wait_p99_ns',
        'go_chan_recv_wait_p50_ns',
        'go_chan_recv_wait_p90_ns',
        'go_chan_recv_wait_p99_ns',
        'go_chan_send_wakeup_to_progress_p50_ns',
        'go_chan_send_wakeup_to_progress_p90_ns',
        'go_chan_send_wakeup_to_progress_p99_ns',
        'go_chan_recv_wakeup_to_progress_p50_ns',
        'go_chan_recv_wakeup_to_progress_p90_ns',
        'go_chan_recv_wakeup_to_progress_p99_ns',
        'go_chan_block_sends_per_s_avg',
        'send_avg_depth',
        'rx_msgs_avg',
        'tx_msgs_avg',
        'go_chan_send_wait_spurious_ratio_avg',
        'go_chan_recv_wait_spurious_ratio_avg',
        'go_chan_send_wait_wakeups_per_s_avg',
        'go_chan_recv_wait_wakeups_per_s_avg',
    ]
    corr_rows = []
    for m, arr in by_mode.items():
        arr.sort(key=lambda r: r['cap'])
        xs = [r['cap'] for r in arr]
        for metric in metrics:
            ys = [r.get(metric, 0.0) for r in arr]
            corr_rows.append({'mode': m, 'metric': metric, 'pearson_r_vs_cap': pearson(xs, ys)})
    with open(f"{base}_correlations.csv", 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=['mode','metric','pearson_r_vs_cap'])
        w.writeheader()
        for row in corr_rows:
            w.writerow(row)
    print(f"Aggregated summary written to {out_prefix}_matrix_summary.csv and SVGs with prefix {out_prefix}_*.svg")
    return 0


def main():
    ap = argparse.ArgumentParser(description='Aggregate bp matrix outputs into CSV/SVG comparisons')
    ap.add_argument('--base', type=str, required=True, help='base directory (e.g., build/bp_matrix)')
    ap.add_argument('--out-prefix', type=str, default=None, help='output prefix path for summary (default: <base>/summary)')
    args = ap.parse_args()
    base = Path(args.base)
    out_prefix = Path(args.out_prefix) if args.out_prefix else base / 'summary'
    return aggregate(base, out_prefix)

if __name__ == '__main__':
    import sys
    sys.exit(main())
