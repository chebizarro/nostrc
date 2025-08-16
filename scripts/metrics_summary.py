#!/usr/bin/env python3
import argparse, json, sys, csv
from statistics import mean

def summarize(path, interval_s=None, csv_prefix=None, svg_prefix=None):
    try:
        with open(path) as f:
            lines = [l.strip() for l in f if l.strip().startswith('{')]
    except FileNotFoundError:
        print(f"File not found: {path}")
        return 1
    if not lines:
        print(f"No metrics JSON lines found in {path}")
        return 1
    objs = []
    for l in lines:
        try:
            objs.append(json.loads(l))
        except Exception:
            # skip non-json lines
            pass
    if len(objs) < 1:
        print(f"No valid JSON objects in {path}")
        return 1

    # If interval not provided, try to infer from first two counters deltas over time not available.
    # Default to 1.0s if unknown; user can supply.
    if interval_s is None:
        interval_s = 1.0

    rates = []
    for i in range(1, len(objs)):
        a, b = objs[i-1].get('counters',{}), objs[i].get('counters',{})
        rx_msgs = b.get('ws_rx_messages',0) - a.get('ws_rx_messages',0)
        rx_bytes = b.get('ws_rx_bytes',0) - a.get('ws_rx_bytes',0)
        tx_msgs = b.get('ws_tx_messages',0) - a.get('ws_tx_messages',0)
        tx_bytes = b.get('ws_tx_bytes',0) - a.get('ws_tx_bytes',0)
        row = {
            'rx_msgs_per_s': rx_msgs/interval_s,
            'rx_bytes_per_s': rx_bytes/interval_s,
            'tx_msgs_per_s': tx_msgs/interval_s,
            'tx_bytes_per_s': tx_bytes/interval_s,
        }
        # backpressure per-interval counters if present
        for k in ['bp_events_generated','bp_probe_rx','bp_eose_sent','bp_notices']:
            row[f'{k}_per_s'] = (b.get(k,0) - a.get(k,0)) / interval_s
        # libgo channel/context counters per-interval rates
        for k in [
            'go_chan_try_send_failures','go_chan_try_recv_failures',
            'go_chan_signal_empty','go_chan_signal_full',
            'go_chan_block_sends','go_chan_block_recvs',
            'go_chan_send_successes','go_chan_recv_successes',
            'go_ctx_cancel_invocations','go_ctx_cancel_broadcasts']:
            row[f'{k}_per_s'] = (b.get(k,0) - a.get(k,0)) / interval_s
        # Derived ratios for send-side
        send_fail_d = (b.get('go_chan_try_send_failures',0) - a.get('go_chan_try_send_failures',0))
        send_succ_d = (b.get('go_chan_send_successes',0) - a.get('go_chan_send_successes',0))
        denom_send = send_fail_d + send_succ_d
        row['go_chan_send_failure_ratio'] = (send_fail_d/denom_send) if denom_send > 0 else 0.0
        # Derived ratios for recv-side
        recv_fail_d = (b.get('go_chan_try_recv_failures',0) - a.get('go_chan_try_recv_failures',0))
        recv_succ_d = (b.get('go_chan_recv_successes',0) - a.get('go_chan_recv_successes',0))
        denom_recv = recv_fail_d + recv_succ_d
        row['go_chan_recv_failure_ratio'] = (recv_fail_d/denom_recv) if denom_recv > 0 else 0.0
        rates.append(row)

    def agg(key):
        if not rates:
            return 0.0, 0.0, 0.0
        vals = [r[key] for r in rates]
        return (min(vals), mean(vals), max(vals))

    rxm_min, rxm_avg, rxm_max = agg('rx_msgs_per_s')
    rxb_min, rxb_avg, rxb_max = agg('rx_bytes_per_s')
    txm_min, txm_avg, txm_max = agg('tx_msgs_per_s')
    txb_min, txb_avg, txb_max = agg('tx_bytes_per_s')

    last = objs[-1].get('histograms',{})
    last_c = objs[-1].get('counters',{})
    def getp(name):
        h = last.get(name, {})
        return h.get('p50_ns',0), h.get('p90_ns',0), h.get('p99_ns',0)

    # Pre-select some common metrics for display if present
    display_histos = [
        'ws_read_ns','ws_write_ns','ws_socket_write_ns',
        'bp_dispatch_ns','bp_burst_ns',
        'go_chan_send_wait_ns','go_chan_recv_wait_ns','go_ctx_wait_ns'
    ]
    present_display = [k for k in display_histos if k in last]

    print(f"SUMMARY for {path}")
    print("THROUGHPUT (msgs/s, bytes/s) over intervals:")
    print(f"  RX msgs/s: min={rxm_min:.1f} avg={rxm_avg:.1f} max={rxm_max:.1f}")
    print(f"  RX bytes/s: min={rxb_min:.0f} avg={rxb_avg:.0f} max={rxb_max:.0f}")
    print(f"  TX msgs/s: min={txm_min:.1f} avg={txm_avg:.1f} max={txm_max:.1f}")
    print(f"  TX bytes/s: min={txb_min:.0f} avg={txb_avg:.0f} max={txb_max:.0f}")
    print("LATENCY PERCENTILES (ns) from last dump:")
    for name in present_display:
        p50, p90, p99 = getp(name)
        print(f"  {name}: p50={p50} p90={p90} p99={p99}")

    # Queue depth averages from cumulative counters
    send_samples = last_c.get('go_chan_send_depth_samples', 0)
    recv_samples = last_c.get('go_chan_recv_depth_samples', 0)
    send_avg_depth = (last_c.get('go_chan_send_depth_sum', 0) / send_samples) if send_samples else 0.0
    recv_avg_depth = (last_c.get('go_chan_recv_depth_sum', 0) / recv_samples) if recv_samples else 0.0
    if send_samples or recv_samples:
        print("QUEUE DEPTH (avg, last cumulative):")
        print(f"  send_avg_depth={send_avg_depth:.2f} (samples={send_samples})")
        print(f"  recv_avg_depth={recv_avg_depth:.2f} (samples={recv_samples})")

    # Backpressure metrics (bp_*)
    # Compute per-interval rates for selected counters if present
    def rate_series(key):
        vals = []
        for i in range(1, len(objs)):
            a, b = objs[i-1].get('counters',{}), objs[i].get('counters',{})
            vals.append((b.get(key,0) - a.get(key,0)) / interval_s)
        return vals

    bp_keys = ['bp_events_generated','bp_probe_rx','bp_eose_sent','bp_notices']
    any_bp = any(k in objs[-1].get('counters',{}) for k in bp_keys)
    if any_bp:
        print("BACKPRESSURE COUNTERS (per second):")
        for k in bp_keys:
            series = rate_series(k)
            if series:
                print(f"  {k}: min={min(series):.1f} avg={mean(series):.1f} max={max(series):.1f}")
        # Histograms
        # If bp_* histos are present they are already printed via all_histos

    # CSV Export
    if csv_prefix:
        # intervals CSV
        fields = sorted({k for r in rates for k in r.keys()})
        with open(f"{csv_prefix}_rates.csv", "w", newline='') as f:
            w = csv.writer(f)
            w.writerow(["interval_index"] + fields)
            for idx, r in enumerate(rates):
                w.writerow([idx] + [r.get(k, 0.0) for k in fields])
        # percentiles CSV (last dump only) for all histograms present
        with open(f"{csv_prefix}_percentiles.csv", "w", newline='') as f:
            w = csv.writer(f)
            w.writerow(["metric","p50_ns","p90_ns","p99_ns"]) 
            for name, stats in sorted(last.items()):
                p50 = stats.get('p50_ns',0)
                p90 = stats.get('p90_ns',0)
                p99 = stats.get('p99_ns',0)
                w.writerow([name, p50, p90, p99])

    # SVG plots (interval rates)
    if svg_prefix and rates:
        def write_svg(series_map, filename, title):
            width, height = 900, 320
            margin = 40
            plot_w, plot_h = width - 2*margin, height - 2*margin
            # X from 0..N-1
            N = max(len(next(iter(series_map.values()))), 1)
            max_y = max((max(v) if v else 0.0) for v in series_map.values())
            max_y = max_y if max_y > 0 else 1.0
            colors = ["#1f77b4","#ff7f0e","#2ca02c","#d62728","#9467bd","#8c564b","#e377c2","#7f7f7f","#bcbd22","#17becf"]
            def sx(i):
                return margin + (i/(N-1 if N>1 else 1))*plot_w
            def sy(y):
                return margin + (1 - y/max_y)*plot_h
            lines = []
            # axes
            lines.append(f'<line x1="{margin}" y1="{margin+plot_h}" x2="{margin+plot_w}" y2="{margin+plot_h}" stroke="#000" stroke-width="1"/>')
            lines.append(f'<line x1="{margin}" y1="{margin}" x2="{margin}" y2="{margin+plot_h}" stroke="#000" stroke-width="1"/>')
            # y ticks (5)
            for t in range(6):
                yv = (t/5)*max_y
                yy = sy(yv)
                lines.append(f'<line x1="{margin-4}" y1="{yy}" x2="{margin}" y2="{yy}" stroke="#000" stroke-width="1"/>')
                lines.append(f'<text x="{margin-8}" y="{yy+4}" font-size="10" text-anchor="end">{yv:.1f}</text>')
            # series polylines
            for idx, (name, vals) in enumerate(series_map.items()):
                pts = " ".join(f"{sx(i):.1f},{sy(vals[i]):.1f}" for i in range(len(vals)))
                color = colors[idx % len(colors)]
                lines.append(f'<polyline fill="none" stroke="{color}" stroke-width="1.5" points="{pts}"/>')
            # legend
            lx, ly = margin+10, margin+10
            for idx, name in enumerate(series_map.keys()):
                color = colors[idx % len(colors)]
                lines.append(f'<rect x="{lx}" y="{ly+idx*16-8}" width="10" height="10" fill="{color}"/>')
                lines.append(f'<text x="{lx+16}" y="{ly+idx*16}" font-size="11">{name}</text>')
            # title
            lines.append(f'<text x="{width/2}" y="{margin-12}" font-size="12" text-anchor="middle">{title}</text>')
            svg = f"<svg xmlns='http://www.w3.org/2000/svg' width='{width}' height='{height}'>" + "".join(lines) + "</svg>"
            with open(filename, 'w') as f:
                f.write(svg)

        # compose series
        msgs = {
            'rx_msgs_per_s': [r.get('rx_msgs_per_s',0.0) for r in rates],
            'tx_msgs_per_s': [r.get('tx_msgs_per_s',0.0) for r in rates],
        }
        bytes_series = {
            'rx_bytes_per_s': [r.get('rx_bytes_per_s',0.0) for r in rates],
            'tx_bytes_per_s': [r.get('tx_bytes_per_s',0.0) for r in rates],
        }
        bp_series = {}
        for k in ['bp_events_generated','bp_probe_rx','bp_eose_sent','bp_notices']:
            s = [r.get(f'{k}_per_s',0.0) for r in rates]
            if any(s):
                bp_series[k] = s
        # Channel rates plot (optional quick view)
        chan_series = {}
        for k in ['go_chan_try_send_failures','go_chan_send_successes','go_chan_signal_empty','go_chan_block_sends']:
            s = [r.get(f'{k}_per_s',0.0) for r in rates]
            if any(s):
                chan_series[k] = s
        write_svg(msgs, f"{svg_prefix}_rates_msgs.svg", "WebSocket Messages per Second")
        write_svg(bytes_series, f"{svg_prefix}_rates_bytes.svg", "WebSocket Bytes per Second")
        if bp_series:
            write_svg(bp_series, f"{svg_prefix}_bp_rates.svg", "Backpressure Rates per Second")
        if chan_series:
            write_svg(chan_series, f"{svg_prefix}_chan_rates.svg", "Channel Rates per Second")
    return 0

if __name__ == '__main__':
    ap = argparse.ArgumentParser(description='Summarize nostr metrics JSONL')
    ap.add_argument('files', nargs='+', help='metrics .jsonl files')
    ap.add_argument('--interval', type=float, default=None, help='interval seconds between dumps (defaults to 1.0 if unknown)')
    ap.add_argument('--csv-prefix', type=str, default=None, help='prefix path to write CSVs (writes *_rates.csv and *_percentiles.csv)')
    ap.add_argument('--svg-prefix', type=str, default=None, help='prefix path to write SVG plots for interval rates')
    args = ap.parse_args()
    rc = 0
    for f in args.files:
        rc |= summarize(f, args.interval, args.csv_prefix, args.svg_prefix)
        print()
    sys.exit(rc)
