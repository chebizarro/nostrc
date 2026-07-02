#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Phase 0 spike: independently verify signet's WebAuthn artifacts with python-fido2.

Reads the JSON produced by emit_artifacts and:
  1. CBOR-decodes the attestation object (independent of our encoder),
  2. checks fmt == "none" and the attested credential data,
  3. extracts the registered COSE public key and confirms it is ES256 with the
     same P-256 coordinates we emitted,
  4. verifies the assertion signature over authenticatorData || clientDataHash
     using python-fido2 / cryptography (independent of OpenSSL here),
  5. confirms a tampered message is rejected.
"""
import hashlib
import json
import sys

from fido2.webauthn import AttestationObject
from fido2.cose import ES256


def fail(msg):
    print(f"FAIL: {msg}")
    sys.exit(1)


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "artifacts.json"
    with open(path) as f:
        a = json.load(f)

    # 1. independent CBOR decode of the attestation object
    att = AttestationObject(bytes.fromhex(a["attestationObject"]))
    if att.fmt != "none":
        fail(f"fmt is {att.fmt!r}, expected 'none'")

    ad = att.auth_data
    # rpIdHash must equal SHA-256(rpId)
    if ad.rp_id_hash != hashlib.sha256(a["rpId"].encode()).digest():
        fail("rpIdHash mismatch")
    if not (ad.flags & 0x40):
        fail("AT flag not set in registration authData")
    if not (ad.flags & 0x08) or not (ad.flags & 0x10):
        fail("BE/BS flags not set (expected synced credential)")

    cred = ad.credential_data
    if cred.credential_id.hex() != a["credentialId"]:
        fail("credentialId mismatch")

    # 3. registered public key is ES256 with matching coordinates
    pub = cred.public_key
    if not isinstance(pub, ES256):
        fail(f"public key is {type(pub).__name__}, expected ES256")
    if pub[-2] != bytes.fromhex(a["pubX"]) or pub[-3] != bytes.fromhex(a["pubY"]):
        fail("COSE public key coordinates do not match emitted X/Y")

    # 4. independently verify the assertion signature
    signed = bytes.fromhex(a["authDataAssert"]) + bytes.fromhex(a["clientDataHashAssert"])
    sig = bytes.fromhex(a["signature"])
    try:
        pub.verify(signed, sig)
    except Exception as e:  # noqa: BLE001
        fail(f"assertion signature did not verify: {e}")

    # 5. tampered message must be rejected
    bad = bytearray(signed)
    bad[-1] ^= 0x01
    try:
        pub.verify(bytes(bad), sig)
        fail("tampered message unexpectedly verified")
    except Exception:
        pass

    print("PASS verify_external (python-fido2 independently decoded and verified "
          "signet's attestation object + ES256 assertion)")


if __name__ == "__main__":
    main()
