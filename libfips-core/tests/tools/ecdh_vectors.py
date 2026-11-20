#!/usr/bin/env python3
"""Independent reference, not derived from libsecp256k1. Test data only.

Affine secp256k1 double-and-add using Python integers; hashlib supplies SHA-256.
The seeded secrets reproduce tools/fips-vectors/src/support.rs (seeded_keypair),
NOT a plain SeededEntropy(seed) fill: xor the keypair domain before SplitMix64.
Run with python3 -I. Never use this variable-time arithmetic with real secrets.
"""
import hashlib
import json
from pathlib import Path

P = 2**256 - 2**32 - 977
N = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
G = (0x79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798,
     0x483ADA7726A3C4655DA4FBFC0E1108A8FD17B448A68554199C47D08FFB10D4B8)
SEEDS = (0, 0x0123456789ABCDEF, 2**64 - 1)


def add(a, b):
    if a is None:
        return b
    if b is None:
        return a
    x, y = a
    u, v = b
    if x == u and (y + v) % P == 0:
        return None
    slope = (3*x*x * pow(2*y, -1, P) if a == b else
             (v-y) * pow(u-x, -1, P)) % P
    w = (slope*slope - x - u) % P
    return w, (slope*(x-w) - y) % P


def mul(k, point=G):
    out = None
    while k:
        if k & 1:
            out = add(out, point)
        point = add(point, point)
        k >>= 1
    return out


def seeded_secret(seed):
    state, mask, out = seed ^ 0x6B65797061697273, 2**64 - 1, b""
    for _ in range(4):
        state = (state + 0x9E3779B97F4A7C15) & mask
        z = ((state ^ (state >> 30)) * 0xBF58476D1CE4E5B9) & mask
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & mask
        out += (z ^ (z >> 31)).to_bytes(8, "little")
    return out


def compressed(point):
    x, y = point
    assert (y*y - x*x*x - 7) % P == 0
    return bytes([2 + (y & 1)]) + x.to_bytes(32, "big")


def main():
    # Validate arithmetic independently of any C or libsecp256k1 execution.
    assert add(G, (G[0], -G[1] % P)) is None
    assert mul(N) is None and mul(N+1) == G
    assert mul(2)[0] == 0xC6047F9441ED7D6D3045406E95C07CD85C778E4B8CEF3CA7ABAC09B95C709EE5
    secrets = [seeded_secret(seed) for seed in SEEDS]
    vectors = Path(__file__).resolve().parents[1] / "vectors"
    identity = json.loads((vectors / "identity.json").read_text())["cases"]
    entropy = json.loads((vectors / "entropy.json").read_text())["cases"]
    for seed, secret, ident, ent in zip(SEEDS, secrets, identity, entropy, strict=True):
        assert ident["seed_hex"] == f"{seed:016x}"
        assert 0 < int.from_bytes(secret, "big") < N
        assert secret.hex() == ident["secret_hex"] == ent["keypair"]["secret"]
        pub = compressed(mul(int.from_bytes(secret, "big")))
        assert pub.hex() == ent["keypair"]["pubkey_compressed"]
        assert pub[1:].hex() == ident["xonly_pubkey"]
    print("Validated: group identities, known 2G, all 3 pinned identity/entropy keypairs")
    pairs = [(secrets[i], secrets[(i+1) % 3]) for i in range(3)]
    pairs.append(((1).to_bytes(32, "big"), (2).to_bytes(32, "big")))
    for i, (a, b) in enumerate(pairs):
        ka, kb = int.from_bytes(a, "big"), int.from_bytes(b, "big")
        shared = mul(ka, mul(kb))
        assert shared == mul(kb, mul(ka)) == mul(ka*kb % N)
        assert mul(ka, (mul(kb)[0], -mul(kb)[1] % P)) == (shared[0], -shared[1] % P)
        x = shared[0].to_bytes(32, "big")
        print(f"{i}: x={x.hex()} sha256={hashlib.sha256(x).hexdigest()}")


if __name__ == "__main__":
    main()
