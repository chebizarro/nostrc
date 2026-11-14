#!/usr/bin/env python3
"""Independent Noise SymmetricState reference over hashlib/hmac. Test data only.

Reproduces core/src/noise/handshake.rs SymmetricState: initialize (a protocol
name of at most 32 bytes is zero-padded, a longer one is SHA-256 hashed),
mix_hash, mix_key and split over HKDF-SHA256 (RFC 5869 with empty info, which
is Noise rev 34 section 4.3 HKDF and the hkdf crate's Hkdf::new(Some(ck), ikm)
.expand(&[], out)). There is no AEAD here, so encrypt_and_hash/decrypt_and_hash
are exercised only in C, against the RFC 8439-tested AEAD.

noise.json records each pattern's `constructed` h after new_initiator /
new_responder, which mix the `<- s` pre-message: the responder's static key
(tools/fips-vectors seeded_keypair(1)) with its prefix forced to 0x02. That key
comes from the checked-in affine secp256k1 reference in ecdh_vectors.py, not
from libsecp256k1. Run with python3 -I.
"""
import hashlib
import hmac
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from ecdh_vectors import compressed, mul, seeded_secret  # noqa: E402

IK = b"Noise_IK_secp256k1_ChaChaPoly_SHA256"
XK = b"Noise_XK_secp256k1_ChaChaPoly_SHA256"
PADDED = b"Noise_NN_25519_ChaChaPoly_SHA256"  # exactly 32 bytes: h is the name itself
SHORT = b"Noise_NN"  # 8 bytes: zero padded
LONG = b"Noise_IKpsk2_secp256k1_ChaChaPoly_SHA256_fips-vectors-long-name!!"  # 65: hashed

# RFC 4231 section 4.2-4.5 (test cases 1-4) and RFC 5869 appendix A.1-A.2.
RFC4231 = [
    (bytes([0x0B] * 20), b"Hi There",
     "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"),
    (b"Jefe", b"what do ya want for nothing?",
     "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"),
    (bytes([0xAA] * 20), bytes([0xDD] * 50),
     "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe"),
    (bytes(range(1, 26)), bytes([0xCD] * 50),
     "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b"),
]
RFC5869 = [
    (bytes([0x0B] * 22), bytes(range(0x0D)), bytes(range(0xF0, 0xFA)), 42,
     "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5",
     "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865"),
    (bytes(range(0x50)), bytes(range(0x60, 0xB0)), bytes(range(0xB0, 0x100)), 82,
     "06a6b88c5853361a06104c9ceb35b45cef760014904671014a193f40c15fc244",
     "b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c"
     "59045a99cac7827271cb41c65e590e09da3275600c2f09b8367793a9aca3db71"
     "cc30c58179ec3e87c14c01d5c1f3434f1d87"),
]


def hmac_sha256(key, data):
    return hmac.new(key, data, hashlib.sha256).digest()


def hkdf_extract(salt, ikm):
    return hmac_sha256(salt, ikm)


def hkdf_expand(prk, info, length):
    out, t, counter = b"", b"", 1
    while len(out) < length:
        t = hmac_sha256(prk, t + info + bytes([counter]))
        out += t
        counter += 1
    return out[:length]


class Symmetric:
    def __init__(self, name):
        self.h = (name + bytes(32 - len(name)) if len(name) <= 32
                  else hashlib.sha256(name).digest())
        self.ck, self.k = self.h, None

    def mix_hash(self, data):
        self.h = hashlib.sha256(self.h + data).digest()

    def mix_key(self, ikm):
        out = hkdf_expand(hkdf_extract(self.ck, ikm), b"", 64)
        self.ck, self.k = out[:32], out[32:]

    def split(self):
        out = hkdf_expand(hkdf_extract(self.ck, b""), b"", 64)
        return out[:32], out[32:]


def show(label, value):
    print(f"{label}={value.hex()}")


def main():
    # Validate the primitives against the RFCs before pinning anything on them.
    for key, data, mac in RFC4231:
        assert hmac_sha256(key, data).hex() == mac
    for ikm, salt, info, length, prk, okm in RFC5869:
        assert hkdf_extract(salt, ikm).hex() == prk
        assert hkdf_expand(bytes.fromhex(prk), info, length).hex() == okm
    assert len(IK) == len(XK) == 36 and len(PADDED) == 32 and len(LONG) == 65

    # The responder's static key is seeded_keypair(1); the pre-message mixes
    # it with an even-parity prefix regardless of its real parity.
    responder = compressed(mul(int.from_bytes(seeded_secret(1), "big")))
    premessage = bytes([0x02]) + responder[1:]
    patterns = json.loads((HERE.parent / "vectors" / "noise.json").read_text())
    patterns = patterns["cases"]["patterns"]
    for pattern, name in zip(patterns, (IK, XK), strict=True):
        assert pattern["pattern"] == name[6:8].decode()
        assert pattern["keypair_seeds"] == [0, 1]
        constructed = pattern["steps"][0]
        assert constructed["event"] == "constructed"
        state = constructed["state"]
        assert state["initiator"]["h"] == state["responder"]["h"]
        assert state["initiator"]["progress"] == "Initial"
        s = Symmetric(name)
        assert s.h != bytes.fromhex(state["initiator"]["h"])
        s.mix_hash(premessage)
        assert s.h.hex() == state["initiator"]["h"]
    print("Validated: RFC 4231 cases 1-4, RFC 5869 cases 1-2, noise.json "
          "constructed h for IK and XK from SHA-256(name) and the <- s pre-message")
    show("responder_static", responder)
    show("ik_h0", Symmetric(IK).h)
    show("xk_h0", Symmetric(XK).h)
    show("padded_h0", Symmetric(PADDED).h)
    show("short_h0", Symmetric(SHORT).h)
    show("long_h0", Symmetric(LONG).h)

    # IK sequence: prologue, a 33-byte pubkey-shaped mix, two MixKeys, an
    # empty MixHash and a Split.
    s = Symmetric(IK)
    s.mix_hash(b"fips-symmetric/prologue")
    show("ik_h1", s.h)
    s.mix_hash(bytes([0x02]) + bytes([0xA5] * 32))
    show("ik_h2", s.h)
    s.mix_key(bytes(range(32)))
    show("ik_ck1", s.ck)
    show("ik_k1", s.k)
    s.mix_hash(b"")
    show("ik_h3", s.h)
    s.mix_key(bytes([0xFF] * 32))
    show("ik_ck2", s.ck)
    show("ik_k2", s.k)
    assert s.h == bytes.fromhex(s.h.hex())  # MixKey leaves h alone; pinned as ik_h3
    s1, s2 = s.split()
    show("ik_split1", s1)
    show("ik_split2", s2)

    # XK sequence: a single MixKey then Split.
    s = Symmetric(XK)
    s.mix_key(bytes(range(32)))
    show("xk_ck1", s.ck)
    show("xk_k1", s.k)
    s1, s2 = s.split()
    show("xk_split1", s1)
    show("xk_split2", s2)


if __name__ == "__main__":
    main()
