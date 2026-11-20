#!/usr/bin/env python3
"""Real fipsctl capture under perf_loopback's proven IK/XK/native-API load."""
import argparse
import json
import os
from pathlib import Path
import re
import tempfile

from perf_loopback import run_pair


def split_capture(lines):
    """A shaped capture as (header through the column row, the rows after it)."""
    column = next(i for i, line in enumerate(lines) if line.startswith('ts_unix'))
    return lines[:column + 1], lines[column + 1:]


def whole_intervals(rows, interval_rows, window_s, interval_secs):
    """How many report intervals a capture holds, each of them the reference's rows.

    The writer flushes the whole report every `interval_secs` of wall time and once
    more when stopped, so a capture holds one interval for every full period of its
    window and one more: a host slow enough to stretch the window past a period
    legitimately ends with the extra flush, and none can exist without the time
    for it. The window is an upper bound (the capture ended before `off` returned),
    so it bounds the count from above; a flush is never missing from the lower end
    except by starvation of the writer, which no wall-clock lower bound can tell
    from a defect."""
    count, rest = divmod(len(rows), len(interval_rows))
    assert not rest and rows == interval_rows * count, 'capture rows are not whole copies of the reference interval'
    most = 1 + int(window_s // interval_secs)
    assert 1 <= count <= most, f'{count} interval(s) in a capture whose window was {window_s:.1f} s (at most {most})'
    return count


def shape(text):
    lines = []
    for line in text.splitlines():
        if '\tdebug_assert_peer_maps_coherent\t' in line:
            continue  # upstream captured with --release
        if line.startswith('#'):
            lines.append(line if line.startswith('# NOTE') else line.split('\t')[0])
        elif line.startswith('ts_unix'):
            lines.append(line)
        elif line:
            row = line.split('\t')
            assert len(row) == 8
            for i in (0, 4, 5, 6):
                assert row[i].isdigit()
                row[i] = 'N'
            lines.append('\t'.join(row))
    return lines


# The field sequence is extracted from the actual upstream pipeline report,
# not inferred from the C formatter. Numeric durations/rates are intentionally
# not compared across different implementations or instrumented builds.
def pipeline_shape(line):
    prefix = re.match(r'\[pipe \d+s\]', line)
    assert prefix, line
    duration = r'\d+(?:\.\d+)?(?:ns|us|ms|s)'
    metric = re.compile(r' (\w+)=\d+/s((?: avg=' + duration +
                        r' p50<=' + duration + r' p95<=' + duration +
                        r' p99<=' + duration + r' max<=' + duration +
                        r' allmax=' + duration + r')?)')
    pos = prefix.end()
    shapes = []
    while pos < len(line):
        match = metric.match(line, pos)
        assert match, line[pos:]
        shapes.append(('stage' if match[2] else 'event',
                       tuple(re.findall(r'(avg=|p50<=|p95<=|p99<=|max<=|allmax=)', match[2]))))
        pos = match.end()
    return shapes


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--fipsd', required=True)
    p.add_argument('--fipsctl', required=True)
    p.add_argument('--native-tool', required=True)
    p.add_argument('--rust', default='')
    p.add_argument('--role', choices=('c', 'rust'), default='c')
    p.add_argument('--seconds', type=float, default=3)
    p.add_argument('--work-dir', type=Path, required=True)
    a = p.parse_args()
    if os.geteuid() == 0:
        print('SKIP: unprivileged capture test (root is confined to /var/log/fips)')
        return 77
    a.latency_samples = 100
    a.profile_dir = a.work_dir / 'captures'
    a.work_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='t', dir=a.work_dir) as tmp:
        result = run_pair(a, (a.role, a.role), Path(tmp))
    reference = Path(__file__).with_name('profile_fixtures')
    rust_shape = shape((reference / 'rust-tick-release.tsv').read_text())
    controls = json.loads((reference / 'rust-control.json').read_text())
    expected_pipeline = {item for line in (reference / 'rust-pipeline.txt').read_text().splitlines()
                         for item in pipeline_shape(line)}
    reports = [line for group in result['pipeline_reports'] for line in group]
    assert reports, 'no real pipeline reporter output'
    assert {item for line in reports for item in pipeline_shape(line)} <= expected_pipeline
    if a.role == 'c':
        assert any('publish_snapshot=' in line for line in reports)
    print('live pipeline field/histogram layout vs captured Rust: PASS')
    print(next(line for line in reports if 'fmp_encrypt=' in line))
    for entry in result['tick_profiles']:
        # The CLI prints data, matching upstream; Response envelopes are wire-only.
        for action in ('on', 'off'):
            assert entry[action].keys() == controls[action].keys(), (action, entry[action])
        assert entry['status']['state'] == 'running' and entry['off']['stopped']
        text = Path(entry['off']['path']).read_text()
        header, rows = split_capture(shape(text))
        rust_header, rust_rows = split_capture(rust_shape)
        assert header == rust_header, 'C/Rust report header mismatch'
        intervals = whole_intervals(rows, rust_rows, entry['window_s'], entry['on']['interval_secs'])
        print(f'capture: {intervals} interval(s) of {len(rust_rows)} rows in a window of {entry["window_s"]:.1f} s')
        match = re.search(r'\twhole_tick\t(\d+)\t', text)
        assert match and int(match[1]) > 0, 'no live ticks recorded'
        print(json.dumps(entry, sort_keys=True))
        print('\n'.join(line for line in text.splitlines()
                        if any(f'\t{name}\t' in line for name in
                               ('whole_tick', 'record_stats_history', 'ticks', 'peers', 'arm_starvation'))))
    for log in a.work_dir.glob('t*/*.log'):
        assert 'ThreadSanitizer' not in log.read_text()
    (a.work_dir / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print('live report shape vs captured Rust release: PASS')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
