#!/usr/bin/env python3
"""Copy verified proto differential sidecars, adapting dispatch-only harness APIs.

Run from any directory. This never modifies the oracle vectors. --check verifies
that committed seeds match them (and catches a missing lookup corpus).
"""
import argparse
import json
from pathlib import Path


def seeds():
    core = Path(__file__).resolve().parents[1]
    vectors = core / 'tests/vectors'
    cases = json.loads((vectors / 'proto.json').read_text())['cases']['wire_cases']
    by_name = {case['name']: case for case in cases}
    families = {
        'coord': ['coords', 'coords-empty'],
        'bloom': ['bloom', 'filter-announce'],
        'stp': ['parent-declaration', 'tree-announce'],
        'lookup': ['lookup-request', 'lookup-response'],
        'link': ['session-datagram'],
        'fmp': ['disconnect', 'fmp-msg-1', 'fmp-msg-2', 'fmp-encrypted'],
        'fsp': ['fsp-setup', 'fsp-ack', 'fsp-msg3', 'fsp-encrypted'],
    }
    for family, names in families.items():
        for name in names:
            case = by_name[name]
            blob = case.get('bytes', case.get('signing_bytes'))
            raw = (vectors / blob['file']).read_bytes()
            assert raw == bytes.fromhex(blob['hex']) and len(raw) == blob['length']
            folder = core / f'fuzz/corpus/fips_fuzz_{family}'
            yield folder / blob['file'], raw
            body = raw[case.get('decode_offset', 0):]
            if family == 'stp':
                body = bytes([name == 'tree-announce']) + body
            if body != raw:
                yield folder / (blob['file'] + '.input'), body
            # Tree gossip also supplies the independent 32-byte metadata codec.
            if name == 'tree-announce':
                yield core / 'fuzz/corpus/fips_fuzz_coord/proto-coord-entry.bin', raw[36:68]
    # Catalog rows have no sidecar: put each exact JSON value at the session
    # payload offset so the link harness reaches both from_u8 entrypoints.
    document = json.loads((vectors / 'proto.json').read_text())['cases']
    for catalog in ['link_catalog', 'session_catalog']:
        for entry in document[catalog]:
            yield core / f'fuzz/corpus/fips_fuzz_link/{catalog}-{entry["value"]:02x}', bytes(35) + bytes([entry['value']])
    # No routing-signal or port-header oracle row exists at this pin. These
    # independently authored valid bodies supplement, never impersonate, vectors.
    folder = core / 'fuzz/corpus/fips_fuzz_routing'
    dest = bytes(range(16))
    reporter = bytes(range(16, 32))
    for signal in range(0x20, 0x23):
        yield folder / f'signal-type-{signal:02x}', bytes([signal, 0])
    base = b'\0' + dest + reporter
    yield folder / 'coords-required', base
    yield folder / 'mtu-exceeded', base + b'\0\x05'
    yield folder / 'path-broken', base + b'\0\0'
    yield folder / 'path-broken-coords', base + b'\1\0' + dest
    yield core / 'fuzz/corpus/fips_fuzz_fsp/ports-exact', b'\0\1\0\1'
    yield core / 'fuzz/corpus/fips_fuzz_fsp/encrypted-coords-some', b'\1\0' + dest + b'\0\0'
    yield core / 'fuzz/corpus/fips_fuzz_fsp/encrypted-coords-none', bytes(4)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    count = 0
    for path, data in seeds():
        if args.check:
            if not path.is_file() or path.read_bytes() != data:
                raise SystemExit(f'stale or missing seed: {path}')
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        count += 1
    print(f'proto corpus: {count} seeds {"verified" if args.check else "written"}')


if __name__ == '__main__':
    main()
