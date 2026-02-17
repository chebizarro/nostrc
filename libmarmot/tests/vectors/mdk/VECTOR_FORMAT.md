# MDK Test Vector Format

Each JSON file contains an array of named test cases for a specific protocol operation.

## Files

| File | Description |
|------|-------------|
| `key_package.json` | KeyPackage creation and serialization |
| `extension.json` | GroupData extension TLS encoding |
| `group_lifecycle.json` | Full group lifecycle (create → add → message → remove) |
| `key_schedule.json` | Key schedule derivation with known inputs |
| `exporter.json` | MLS-Exporter with Marmot labels |
| `welcome.json` | Welcome creation and parsing |

## Common Fields

All binary values are hex-encoded strings.

```json
{
  "name": "descriptive_test_name",
  "ciphersuite": 1,
  "notes": "optional description"
}
```

## key_package.json

```json
{
  "name": "basic_key_package",
  "ciphersuite": 1,
  "identity": "aabbccdd...",
  "init_key_private": "...",
  "init_key_public": "...",
  "signature_key_private": "...",
  "signature_key_public": "...",
  "serialized_key_package": "...",
  "key_package_ref": "..."
}
```

## key_schedule.json

```json
{
  "name": "epoch_0_derivation",
  "ciphersuite": 1,
  "init_secret_prev": "0000...0000",
  "commit_secret": "aabb...ccdd",
  "group_context": "...",
  "psk_secret": null,
  "joiner_secret": "...",
  "welcome_secret": "...",
  "epoch_secret": "...",
  "sender_data_secret": "...",
  "encryption_secret": "...",
  "exporter_secret": "...",
  "external_secret": "...",
  "confirmation_key": "...",
  "membership_key": "...",
  "resumption_psk": "...",
  "epoch_authenticator": "...",
  "init_secret": "..."
}
```

## exporter.json

```json
{
  "name": "nip44_key_derivation",
  "ciphersuite": 1,
  "exporter_secret": "...",
  "label": "marmot-nip44-key",
  "context": "...",
  "length": 32,
  "exported_value": "..."
}
```

## Creating Test Vectors

### Current Status

The MDK (Marmot Development Kit) Rust reference implementation is not yet available.
Until MDK is published, test vectors can be created in the following ways:

### Option 1: Self-Generated Vectors (Current Approach)

The `test_interop.c` test suite generates self-consistency vectors that exercise
the same serialization and protocol paths. To capture these for cross-validation:

1. Run the interop test with vector dumping enabled:
   ```bash
   cd libmarmot
   MARMOT_DUMP_VECTORS=1 ./build/tests/test_interop > vectors.json
   ```

2. Manually format the output into the JSON structure documented above

### Option 2: Manual Vector Creation

Create JSON files manually using known-answer tests from:
- RFC 9420 MLS test vectors
- RFC 5869 HKDF test vectors  
- RFC 8032 Ed25519 test vectors
- Cross-validation with other MLS implementations

### Option 3: Future MDK Integration

Once the MDK Rust implementation is available:

1. Clone MDK repository (URL TBD)
2. Enable vector dumping: `MDK_DUMP_VECTORS=1 cargo test`
3. Copy generated vectors from `target/test_vectors/` to this directory

### Vector Validation

All vectors in this directory should be validated against both:
- libmarmot (this C implementation)
- MDK (Rust reference implementation, when available)

This ensures cross-implementation compatibility.
