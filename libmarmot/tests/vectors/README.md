# libmarmot Test Vectors

This directory contains test vectors for validating libmarmot's MLS and
Marmot protocol implementations.

## Directory Structure

```
vectors/
├── README.md           # This file
├── rfc9420/            # RFC 9420 MLS key schedule vectors
│   └── (hardcoded in test_rfc9420_vectors.c)
├── crypto/             # Low-level crypto primitive vectors
│   └── (hardcoded in test_rfc9420_vectors.c)
└── mdk/                # MDK interop vectors (captured from Rust ref impl)
    └── (JSON files — see mm51 for format)
```

## RFC 9420 Vectors

The RFC 9420 test vectors validate the key schedule, secret tree, and
crypto primitive implementations against known-answer tests derived from
the [MLS WG test vector repository](https://github.com/mlswg/mls-implementations).

Test vectors for ciphersuite 0x0001 (MLS_128_DHKEMX25519_AES128GCM_SHA256_Ed25519)
are hardcoded in `test_rfc9420_vectors.c`.

## MDK Interop Vectors

MDK interop vectors are JSON files captured by running the MDK (Rust reference
implementation) test suite with intermediate value dumping enabled.

### Vector Format

Each JSON file contains an array of test cases:

```json
{
  "description": "KeyPackage round-trip",
  "ciphersuite": 1,
  "inputs": { ... },
  "outputs": { ... }
}
```

All binary data is hex-encoded.

### Capturing New Vectors

To capture vectors from MDK:

1. Clone the MDK repository
2. Run: `MDK_DUMP_VECTORS=1 cargo test`
3. Copy `target/test_vectors/*.json` to this directory
