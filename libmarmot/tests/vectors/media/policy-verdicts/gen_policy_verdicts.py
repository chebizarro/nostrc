#!/usr/bin/env python3
"""Build ../policy-verdicts-mdk-v0.11.0.json: component states of 0x8006
(agent text stream) and 0x800b (encrypted media v2 policy) judged by MDK
v0.11.0's own decoders (cgka-traits, through the w24i-verdicts tool in this
directory; see ../README.md).  Deterministic: no randomness but a seeded PRNG.

Usage: gen_policy_verdicts.py <path to the built w24i-verdicts binary>
"""
import json, os, random, subprocess, sys

TOOL = sys.argv[1]
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'policy-verdicts-mdk-v0.11.0.json')


def vli(n):
    if n < 64: return bytes([n])
    if n < 16384: return bytes([0x40 | (n >> 8), n & 0xff])
    return bytes([0x80 | (n >> 24), (n >> 16) & 0xff, (n >> 8) & 0xff, n & 0xff])


def vec(b):
    return vli(len(b)) + b


def policy(fmt=b'encrypted-media-v2', kinds=(b'blossom-v1',), eps=((b'blossom-v1', b'https://blossom.example.com/'),), tail=b''):
    k = b''.join(vec(x) for x in kinds)
    e = b''.join(vec(a) + vec(b) for a, b in eps)
    return vec(fmt) + vec(k) + vec(e) + tail


def agent(req, allowed, frame, ttl, pad):
    return bytes([req, allowed]) + frame.to_bytes(4, 'big') + ttl.to_bytes(4, 'big') + pad.to_bytes(2, 'big')


def run(args, stdin):
    return subprocess.run([TOOL] + args, input=stdin, capture_output=True, text=True, check=True).stdout


cases = []  # (id, note, bytes)
# ── 0x8006 ──
for note, b in [
    ('user_to_agent_default', agent(1, 3, 4096, 0, 0)),
    ('max bounds', agent(1, 7, 65519, 300, 4096)),
    ('frame 65520', agent(1, 3, 65520, 0, 0)),
    ('frame 0', agent(1, 3, 0, 0, 0)),
    ('ttl 301', agent(1, 3, 4096, 301, 0)),
    ('padding 4097', agent(1, 3, 4096, 0, 4097)),
    ('required empty', agent(0, 3, 4096, 0, 0)),
    ('required unknown bit', agent(9, 15, 4096, 0, 0)),
    ('allowed unknown bit', agent(1, 0x81, 4096, 0, 0)),
    ('required not allowed', agent(1, 2, 4096, 0, 0)),
    ('required send', agent(2, 3, 4096, 0, 0)),
    ('required receive+send', agent(3, 3, 4096, 0, 0)),
    ('required fanout', agent(4, 7, 4096, 0, 0)),
    ('allowed all, required receive', agent(1, 7, 1, 1, 1)),
    ('11 bytes', agent(1, 3, 4096, 0, 0)[:11]),
    ('13 bytes', agent(1, 3, 4096, 0, 0) + b'\0'),
    ('empty', b''),
]:
    cases.append(('8006', note, b))
rng = random.Random(0x8006)
for i in range(200):
    req = rng.choice([0, 1, 1, 1, 2, 3, 4, 5, 7, 8, 0x41])
    allowed = rng.choice([0, 1, 3, 3, 7, 7, 2, 0x10])
    frame = rng.choice([0, 1, 4096, 65519, 65520, rng.randrange(1 << 32)])
    ttl = rng.choice([0, 300, 301, rng.randrange(1 << 32)])
    pad = rng.choice([0, 4096, 4097, rng.randrange(1 << 16)])
    cases.append(('8006', 'prng %d' % i, agent(req, allowed, frame, ttl, pad)))

# ── 0x800b: structure ──
U = b'https://blossom.example.com/'
B = b'blossom-v1'
for note, b in [
    ('white noise default', policy()),
    ('two kinds, two endpoints', policy(kinds=(B, b'ipfs-v1'), eps=((B, U), (b'ipfs-v1', b'https://ipfs.example/')))),
    ('http endpoint', policy(eps=((B, b'http://blossom.example.com/'),))),
    ('endpoint with path', policy(eps=((B, b'https://blossom.example.com/media/v1'),))),
    ('endpoint with port', policy(eps=((B, b'https://blossom.example.com:8443/'),))),
    ('wrong format', policy(fmt=b'encrypted-media-v1')),
    ('format with space', policy(fmt=b'encrypted-media-v2 ')),
    ('no kinds', policy(kinds=(), eps=())),
    ('no endpoints', policy(eps=())),
    ('duplicate kind', policy(kinds=(B, B))),
    ('uppercase kind', policy(kinds=(b'Blossom-v1',), eps=((b'Blossom-v1', U),))),
    ('kind with underscore', policy(kinds=(b'blossom_v1',), eps=((b'blossom_v1', U),))),
    ('empty kind', policy(kinds=(b'',), eps=((b'', U),))),
    ('kind 64 bytes', policy(kinds=(b'a' * 64,), eps=((b'a' * 64, U),))),
    ('kind 65 bytes', policy(kinds=(b'a' * 65,), eps=((b'a' * 65, U),))),
    ('16 kinds', policy(kinds=tuple(b'k%d' % i for i in range(16)), eps=((b'k0', U),))),
    ('17 kinds', policy(kinds=tuple(b'k%d' % i for i in range(17)), eps=((b'k0', U),))),
    ('16 endpoints', policy(eps=tuple((B, b'https://b%d.example/' % i) for i in range(16)))),
    ('17 endpoints', policy(eps=tuple((B, b'https://b%d.example/' % i) for i in range(17)))),
    ('endpoint kind not allowed', policy(eps=((b'ipfs-v1', U),))),
    ('duplicate endpoint', policy(eps=((B, U), (B, U)))),
    ('same url two kinds', policy(kinds=(B, b'other'), eps=((B, U), (b'other', U)))),
    ('trailing byte', policy(tail=b'\x00')),
    ('non-minimal format prefix', b'\x40\x12' + b'encrypted-media-v2' + policy()[19:]),
    ('truncated', policy()[:-1]),
    ('empty', b''),
    ('url not utf-8', policy(eps=((B, b'https://blossom.example.com/\xff'),))),
    ('url 2048 bytes', policy(eps=((B, b'https://b.example/' + b'a' * (2048 - 18)),))),
    ('url 2049 bytes', policy(eps=((B, b'https://b.example/' + b'a' * (2049 - 18)),))),
]:
    cases.append(('800b', note, b))

# ── 0x800b: endpoint URLs, canonical (MDK-normalized) and raw ──
raw_urls = [
    'https://blossom.example.com/', 'https://blossom.example.com', 'https://Blossom.Example.com/',
    'http://blossom.example.com/', 'http://blossom.example.com:80/', 'https://blossom.example.com:443/',
    'https://blossom.example.com:8443', 'https://blossom.example.com/a/../b', 'https://blossom.example.com/./a',
    'https://blossom.example.com/a b', 'https://blossom.example.com/?', 'https://blossom.example.com/?q=1',
    'https://blossom.example.com/#f', 'https://u:p@blossom.example.com/', 'ftp://blossom.example.com/',
    'wss://blossom.example.com/', 'https://127.0.0.1/', 'https://127.1/', 'https://0x7f.0.0.1/', 'https://[::1]/',
    'https://[0:0:0:0:0:0:0:1]/', 'https://xn--bcher-kva.example/', 'https://bücher.example/',
    'https://my_host.example.com/', 'https://blossom.example.com/a^b', 'https://blossom.example.com/a|b',
    'https://blossom.example.com/[x]', 'https://blossom.example.com/%7e', 'https://blossom.example.com/~',
    'https://blossom.example.com/a%20b', 'https://blossom.example.com/a`b', 'https://blossom.example.com/{x}',
    'https://blossom.example.com/a\\b', 'https://blossom.example.com:0/', 'https://blossom.example.com:00443/',
    'https://blossom.example.com:/', 'https://blossom.example.com./', 'https://a..example/', 'HTTPS://blossom.example.com/',
    'https:blossom.example.com', 'https:///blossom.example.com/', ' https://blossom.example.com/ ',
    'https://blossom.example.com/b:/..', 'https://blossom.example.com/%2e/x', 'https://e%41.example/',
    'https://ex%2Eample.com/', 'https://localhost/', 'https://blossom.example.com:65535/', 'https://blossom.example.com:65536/',
    # IP literals (slice I review L2): provably invalid, and valid edge forms.
    'https://256.1.1.1/', 'https://1.2.3.4.5/', 'https://x.123/', 'https://[1::2::3]/',
    'https://[::1%25eth0]/', 'https://08.1.1.1/', 'https://1.2.3.256/', 'https://0x/',
    'https://0x7f.1/', 'https://[::1:2:3:4:5:6:7]/', 'https://[0:1:2:3:4:5:6:7]/', 'https://[1:2]/',
    'https://[::ffff:1.2.3.4]/', 'https://[::1.2.3.04]/', 'https://4294967296/', 'https://4294967295/',
]
norm = [json.loads(l) for l in run(['normalize'], '\n'.join(u.encode().hex() for u in raw_urls) + '\n').splitlines()]
urls = []
for n in norm:
    urls.append(n['in'])
    if n['ok'] and n['normalized'] != n['in']:
        urls.append(n['normalized'])
seen = set()
for u in urls:
    if u in seen: continue
    seen.add(u)
    cases.append(('800b', 'endpoint ' + u, policy(eps=((B, u.encode()),))))

inp = '\n'.join('%s %s' % (cid, b.hex()) for cid, _, b in cases) + '\n'
out = [json.loads(l) for l in run([], inp).splitlines()]
assert len(out) == len(cases)
doc = {
    'source': 'MDK v0.11.0 946e0547485c9a2c393c2048ec3a968fd50fb441 cgka-traits: '
              'AgentTextStreamQuicPolicyV1::decode_component_state (0x8006), '
              'decode_encrypted_media_policy_v2 (0x800b), '
              'validate_and_normalize_blob_endpoint_url_v2 (url 2.5.8)',
    'generator': 'policy-verdicts/gen_policy_verdicts.py',
    'normalize': norm,
    'cases': [dict(id=cid, note=note, bytes=b.hex(), mdk_ok=o['ok'], mdk_error=o.get('error'))
              for (cid, note, b), o in zip(cases, out)],
}
with open(OUT, 'w') as f:
    json.dump(doc, f, indent=1, ensure_ascii=False)
    f.write('\n')
print('wrote', OUT, len(cases), 'cases')
