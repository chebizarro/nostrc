#!/usr/bin/env python3
"""netns/veth chaos harness driver for fipsd (nostrc-f66sr.3). See README.md.

Run as root:
  chaos.py --fipsd build/fipsd/fipsd [--rust /path/to/fips] [--scenario flap mtu nat netem ...]
           [--variant cc cr] [--output DIR] [--nat-mode masquerade|cone] [--timeout-scale X] [--keep]
  chaos.py --purge        delete the namespaces a crashed run left behind, nothing else

Each scenario/variant gets its own namespaces (prefix `fc<scenario><variant>-`),
daemons and services; teardown runs in `finally` and again from run.sh's trap.
On failure the daemon logs, service logs, harness trace and per-namespace
diagnostics (addresses, routes, qdiscs, nft, conntrack) are written under
--output/<scenario>-<variant>/ and the failure is printed. Exit 0 when every
selected scenario passed, 1 on any failure, 2 on usage or environment errors.
"""
import argparse
import json
import os
import shutil
import signal
import sys
import tempfile
import time
import traceback
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from chaoslib.net import Lab, LabError  # noqa: E402
from chaoslib.nodes import Daemon, Failure, Mesh, Timeouts  # noqa: E402
from chaoslib.scenarios import ALL  # noqa: E402


class Context:
    """What one scenario run sees: binaries, lab, work directory, timeouts and the trace."""

    Failure = Failure

    def __init__(self, args, name, variant, lab, workdir):
        self.fipsd, self.rust, self.nat_mode = args.fipsd, args.rust, args.nat_mode
        self.timeouts = Timeouts(args.timeout_scale)
        self.lab, self.workdir = lab, workdir
        self.tag = f'{name}-{variant}'
        self.env = dict(os.environ, FIPSD_WORKERS='2', TOKIO_WORKER_THREADS='2', NO_COLOR='1')
        self.env.setdefault('RUST_LOG', 'info,fips::nostr=debug')
        self.lines = []
        self.services = []
        self.current_mesh = None

    def note(self, line):
        text = f'[{time.strftime("%H:%M:%S")} {self.tag}] {line}'
        self.lines.append(text)
        print(text, flush=True)

    def after(self, seconds):
        return time.monotonic() + seconds

    def mesh(self):
        self.current_mesh = Mesh(self, self.tag)
        return self.current_mesh

    def service(self, name, argv):
        daemon = Daemon(name, argv, self.env, str(self.workdir))
        self.services.append(daemon)
        return daemon

    def stop_services(self):
        for daemon in self.services:
            daemon.stop(5)

    def dump(self, out_dir):
        out_dir.mkdir(parents=True, exist_ok=True)
        if self.current_mesh:
            self.current_mesh.dump(out_dir)
        for daemon in self.services:
            (out_dir / f'{daemon.name}.log').write_text(daemon.text())
        (out_dir / 'trace.txt').write_text('\n'.join(self.lines) + '\n')
        try:
            (out_dir / 'netns-diagnostics.txt').write_text(self.lab.diagnostics())
        except LabError as e:
            (out_dir / 'netns-diagnostics.txt').write_text(f'diagnostics failed: {e}\n')


def run_one(args, name, variant, workdir, output):
    module = ALL[name]
    stale = Lab.purge(f'{Lab.PREFIX}{name}{variant}-')
    if stale:
        print(f'purged namespaces left by an earlier {name}/{variant} run: {stale}', flush=True)
    lab = Lab(f'{name}{variant}')
    ctx = Context(args, name, variant, lab, workdir)
    out_dir = output / f'{name}-{variant}'
    started = time.monotonic()
    verdict, detail, failures = 'PASS', {}, []
    try:
        detail = module.run(ctx, variant) or {}
    except Exception as e:  # noqa: BLE001 - every failure must carry the evidence
        verdict = 'FAIL'
        failures.append(f'{type(e).__name__}: {e}')
        ctx.note('FAILED: ' + failures[-1])
        if not isinstance(e, (Failure, LabError)):
            ctx.note(traceback.format_exc())
    finally:
        try:
            if verdict == 'FAIL':
                ctx.dump(out_dir)
            if ctx.current_mesh:
                shutdown_failures = ctx.current_mesh.stop_all()
                if shutdown_failures:
                    failures += shutdown_failures
                    ctx.note('shutdown: ' + '; '.join(shutdown_failures))
                    if verdict == 'PASS':
                        verdict = 'FAIL'
                        ctx.dump(out_dir)
            ctx.stop_services()
        finally:
            if not args.keep:
                left = lab.teardown()
                if left:
                    ctx.note(f'teardown could not delete: {left}')
    elapsed = time.monotonic() - started
    if verdict == 'FAIL':
        print(f'----- {name}/{variant}: evidence written to {out_dir} -----', flush=True)
        for daemon in ([n.daemon for n in ctx.current_mesh.nodes if n.daemon] if ctx.current_mesh else []) + ctx.services:
            print(f'----- {name}/{variant}: {daemon.name} log (last 60 lines) -----', flush=True)
            for line in daemon.text().splitlines()[-60:]:
                print(f'[{daemon.name}] {line}')
    if ctx.current_mesh and ctx.current_mesh.control_stalls:
        detail = dict(detail, control_stalls=ctx.current_mesh.control_stalls)
    return {'scenario': name, 'variant': variant, 'verdict': verdict, 'seconds': round(elapsed, 1),
            'detail': detail, 'failures': failures}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--fipsd', help='fipsd binary')
    ap.add_argument('--rust', default=os.environ.get('FIPSD_UDP_INTEROP_RUST_BINARY', ''),
                    help='fips daemon built from the libfips-core/UPSTREAM.md pin; without it the mixed (cr) variants are skipped')
    ap.add_argument('--scenario', nargs='+', choices=sorted(ALL), default=sorted(ALL))
    ap.add_argument('--variant', nargs='+', choices=('cc', 'cr'), default=('cc', 'cr'))
    ap.add_argument('--output', default='fips-chaos-results', help='evidence directory (written on failure, plus results.json)')
    ap.add_argument('--nat-mode', choices=('masquerade', 'cone'), default='masquerade')
    ap.add_argument('--timeout-scale', type=float, default=1.0)
    ap.add_argument('--keep', action='store_true', help='leave namespaces and the work directory in place for inspection')
    ap.add_argument('--purge', action='store_true', help='delete leftover fc* namespaces and exit')
    args = ap.parse_args()
    if os.geteuid() != 0:
        print('chaos.py: must run as root (ip netns, nftables, tc)', file=sys.stderr)
        return 2
    if args.purge:
        removed = Lab.purge()
        print(f'purged namespaces: {removed}' if removed else 'no leftover namespaces')
        return 0
    if not args.fipsd or not Path(args.fipsd).is_file():
        print('chaos.py: --fipsd must name the fipsd binary', file=sys.stderr)
        return 2
    args.fipsd = str(Path(args.fipsd).resolve())
    if args.rust and not Path(args.rust).is_file():
        print(f'chaos.py: --rust {args.rust} is not a file', file=sys.stderr)
        return 2
    args.rust = str(Path(args.rust).resolve()) if args.rust else ''
    for tool in ('ip', 'nft', 'tc'):
        if not shutil.which(tool):
            print(f'chaos.py: missing required tool {tool}', file=sys.stderr)
            return 2
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    workdir = Path(tempfile.mkdtemp(prefix='fc-', dir='/tmp'))
    os.chmod(workdir, 0o755)

    def interrupted(signum, _frame):
        raise KeyboardInterrupt(f'signal {signum}')

    signal.signal(signal.SIGTERM, interrupted)
    results = []
    try:
        for name in args.scenario:
            for variant in args.variant:
                if variant not in ALL[name].VARIANTS:
                    continue
                if variant == 'cr' and not args.rust:
                    results.append({'scenario': name, 'variant': variant, 'verdict': 'SKIP', 'seconds': 0,
                                    'detail': {}, 'failures': ['no Rust daemon supplied (--rust / FIPSD_UDP_INTEROP_RUST_BINARY)']})
                    continue
                print(f'=== {name}/{variant} ===', flush=True)
                results.append(run_one(args, name, variant, workdir, output))
    except KeyboardInterrupt as e:
        print(f'interrupted ({e}); namespaces torn down', file=sys.stderr)
        Lab.purge()
        return 1
    finally:
        if not args.keep:
            shutil.rmtree(workdir, ignore_errors=True)
    (output / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    print('\n=== fips chaos results ===')
    for r in results:
        extra = json.dumps(r['detail']) if r['verdict'] == 'PASS' else '; '.join(r['failures'])
        print(f'RESULT {r["scenario"]}/{r["variant"]} {r["verdict"]} {r["seconds"]}s {extra}')
    failed = [r for r in results if r['verdict'] == 'FAIL']
    print(f'{len(results) - len(failed)} of {len(results)} passed' + (f', {len(failed)} FAILED' if failed else ''))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
