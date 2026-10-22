#!/usr/bin/env python3
"""Actual Docker cache-export ownership, lost-reply and cancellation regressions.

Requires the independently attested source-only image; never builds/fetches an
image, starts its contents, or treats these finite cases as original MDK tests.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, required=True)
    parser.add_argument('--producer-receipt', type=Path, required=True)
    parser.add_argument('--evidence-dir', type=Path, required=True)
    args = parser.parse_args()
    source = args.source_root.resolve(strict=True)
    producer = json.loads(args.producer_receipt.read_text())
    helper = source / 'scripts/mdk011-cache-build.py'
    spec = importlib.util.spec_from_file_location('cache_builder', helper)
    builder = importlib.util.module_from_spec(spec); spec.loader.exec_module(builder)
    out = args.evidence_dir.resolve(); out.mkdir(mode=0o700)
    docker = producer['docker']
    assert hashlib.sha256(Path(docker).read_bytes()).hexdigest() == producer['docker_executable']['sha256']
    env = dict(os.environ, DOCKER_HOST=producer['docker_endpoint'])
    def actual(argv):
        return subprocess.check_output([docker, *argv], env=env, timeout=300,
                                       stderr=subprocess.PIPE, start_new_session=True)
    assert actual(['info', '--format', '{{.ID}}']).decode().strip() == producer['daemon_id']
    image_id = producer['image_results'][0]['image_id']
    assert actual(['image', 'inspect', image_id, '--format', '{{.Id}}']).decode().strip() == image_id
    helper_hash = builder.sha(helper)
    results = []
    for case in ('actual-export', 'lost-create-response', 'cancel-after-create'):
        result = {'case': case, 'complete': False, 'complete_gate_eligible': False}
        destination = out / (case + '.json')
        def save():
            destination.write_text(json.dumps(result, indent=2) + '\n')
        def call(argv):
            raw = actual(argv)
            if argv[0] == 'create':
                assert destination.is_file(), 'ownership journal must precede create'
                reservation = json.loads(destination.read_text())['cache_export_reservation']
                assert reservation['name'] in argv
                result['actual_create_reply'] = raw.decode().strip()
                if case == 'lost-create-response':
                    # Real create completed; inject only loss of its client reply.
                    raise subprocess.TimeoutExpired([docker, *argv], 300)
                if case == 'cancel-after-create':
                    os.kill(os.getpid(), signal.SIGTERM)
            return raw
        refused = False
        try:
            with builder.export_container(call, image_id, producer['run_id'], result, save) as cid:
                archive = out / 'actual-export.tar'
                actual(['export', '--output', str(archive), cid])
                result['export_sha256'] = builder.sha(archive)
                result['export_bytes'] = archive.stat().st_size
        except (subprocess.TimeoutExpired, InterruptedError):
            refused = True
        assert refused == (case != 'actual-export'), result
        assert result['cache_export_container_removed'] is True, result
        assert 'cache_export_cleanup_error' not in result, result
        assert result['cache_export_cid'] == result['actual_create_reply'], result
        result['expected_disposition_verified'] = True
        save(); results.append(result)
    assert builder.sha(helper) == helper_hash, 'helper changed during cases'
    summary = {'helper_sha256': helper_hash, 'producer_receipt_sha256': builder.sha(args.producer_receipt),
               'image_id': image_id, 'cases': len(results), 'passed': len(results),
               'complete': False, 'complete_gate_eligible': False}
    (out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary))


if __name__ == '__main__':
    main()
