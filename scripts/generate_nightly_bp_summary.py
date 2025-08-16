#!/usr/bin/env python3
import argparse
from pathlib import Path
import re

tmpl = """# Nightly Backpressure Summary

- Date: {date}
- Variants: {variants}

## Reports

{reports}

## Key Comparisons

### Send Failure Ratio vs Capacity

{send_fail_imgs}

### Send Wait Latencies vs Capacity

- p50: {send_p50_imgs}
- p90: {send_p90_imgs}
- p99: {send_p99_imgs}

### Wakeup Productivity (Spurious Ratio)

- Send: {send_spur_imgs}
- Recv: {recv_spur_imgs}

### Wakeup-to-Progress Latencies (Send)

- p50: {send_wtp_p50_imgs}
- p90: {send_wtp_p90_imgs}
- p99: {send_wtp_p99_imgs}

### Wakeup-to-Progress Latencies (Recv)

- p50: {recv_wtp_p50_imgs}
- p90: {recv_wtp_p90_imgs}
- p99: {recv_wtp_p99_imgs}

"""

def rel(path: Path, base: Path) -> str:
    return str(path.resolve().relative_to(base.resolve()))

def generate(base_root: Path, out_md: Path):
    # discover bp_matrix_cu* dirs
    dirs = sorted([p for p in base_root.glob('bp_matrix_cu*') if p.is_dir()], key=lambda p: p.name)
    if not dirs:
        raise SystemExit("No bp_matrix_cu* directories found")
    # variant labels from names
    def label(p: Path):
        m = re.search(r"cu(\d+)$", p.name)
        return f"consume_us={m.group(1)}" if m else p.name
    variants = ", ".join(label(d) for d in dirs)
    # build sections
    reports = []
    send_fail_imgs = []
    send_p50_imgs = []
    send_p90_imgs = []
    send_p99_imgs = []
    send_spur_imgs = []
    recv_spur_imgs = []
    send_wtp_p50_imgs = []
    send_wtp_p90_imgs = []
    send_wtp_p99_imgs = []
    recv_wtp_p50_imgs = []
    recv_wtp_p90_imgs = []
    recv_wtp_p99_imgs = []
    for d in dirs:
        lab = label(d)
        rep = d / 'REPORT.md'
        summ = d / 'summary_matrix_summary.csv'
        reports.append(f"- {lab}: [{rel(rep, base_root)}]({rel(rep, base_root)}) · CSV: [{rel(summ, base_root)}]({rel(summ, base_root)})")
        prefix = d / 'summary'
        send_fail = rel(prefix.with_name(prefix.name + '_send_fail_ratio.svg'), base_root)
        send_p50 = rel(prefix.with_name(prefix.name + '_send_wait_p50.svg'), base_root)
        send_p90 = rel(prefix.with_name(prefix.name + '_send_wait_p90.svg'), base_root)
        send_p99 = rel(prefix.with_name(prefix.name + '_send_wait_p99.svg'), base_root)
        send_spur = rel(prefix.with_name(prefix.name + '_send_spurious_ratio.svg'), base_root)
        recv_spur = rel(prefix.with_name(prefix.name + '_recv_spurious_ratio.svg'), base_root)
        send_fail_imgs.append(f"- {lab}: ![]({send_fail})")
        send_p50_imgs.append(f"![]({send_p50})")
        send_p90_imgs.append(f"![]({send_p90})")
        send_p99_imgs.append(f"![]({send_p99})")
        send_spur_imgs.append(f"![]({send_spur})")
        recv_spur_imgs.append(f"![]({recv_spur})")
        # wakeup-to-progress images
        send_wtp_p50 = rel(prefix.with_name(prefix.name + '_send_wakeup_to_progress_p50.svg'), base_root)
        send_wtp_p90 = rel(prefix.with_name(prefix.name + '_send_wakeup_to_progress_p90.svg'), base_root)
        send_wtp_p99 = rel(prefix.with_name(prefix.name + '_send_wakeup_to_progress_p99.svg'), base_root)
        recv_wtp_p50 = rel(prefix.with_name(prefix.name + '_recv_wakeup_to_progress_p50.svg'), base_root)
        recv_wtp_p90 = rel(prefix.with_name(prefix.name + '_recv_wakeup_to_progress_p90.svg'), base_root)
        recv_wtp_p99 = rel(prefix.with_name(prefix.name + '_recv_wakeup_to_progress_p99.svg'), base_root)
        send_wtp_p50_imgs.append(f"![]({send_wtp_p50})")
        send_wtp_p90_imgs.append(f"![]({send_wtp_p90})")
        send_wtp_p99_imgs.append(f"![]({send_wtp_p99})")
        recv_wtp_p50_imgs.append(f"![]({recv_wtp_p50})")
        recv_wtp_p90_imgs.append(f"![]({recv_wtp_p90})")
        recv_wtp_p99_imgs.append(f"![]({recv_wtp_p99})")
    content = tmpl.format(
        date='${{ github.run_id }}',
        variants=variants,
        reports="\n".join(reports),
        send_fail_imgs="\n".join(send_fail_imgs),
        send_p50_imgs=" ".join(send_p50_imgs),
        send_p90_imgs=" ".join(send_p90_imgs),
        send_p99_imgs=" ".join(send_p99_imgs),
        send_spur_imgs=" ".join(send_spur_imgs),
        recv_spur_imgs=" ".join(recv_spur_imgs),
        send_wtp_p50_imgs=" ".join(send_wtp_p50_imgs),
        send_wtp_p90_imgs=" ".join(send_wtp_p90_imgs),
        send_wtp_p99_imgs=" ".join(send_wtp_p99_imgs),
        recv_wtp_p50_imgs=" ".join(recv_wtp_p50_imgs),
        recv_wtp_p90_imgs=" ".join(recv_wtp_p90_imgs),
        recv_wtp_p99_imgs=" ".join(recv_wtp_p99_imgs),
    )
    out_md.write_text(content)
    print(f"Wrote nightly summary: {out_md}")

if __name__ == '__main__':
    ap = argparse.ArgumentParser(description='Generate top-level nightly summary for bp_matrix runs')
    ap.add_argument('--root', default='build', help='root build directory containing bp_matrix_cu* folders')
    ap.add_argument('--out', default=None, help='output markdown path (default: <root>/bp_matrix_nightly/INDEX.md)')
    args = ap.parse_args()
    root = Path(args.root)
    out = Path(args.out) if args.out else (root / 'bp_matrix_nightly' / 'INDEX.md')
    out.parent.mkdir(parents=True, exist_ok=True)
    generate(root, out)
