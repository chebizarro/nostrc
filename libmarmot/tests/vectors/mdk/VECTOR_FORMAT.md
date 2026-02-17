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

## Capturing from MDK

1. Clone MDK: `git clone https://github.com/marmot-labs/mdk`
2. Set dump env: `MDK_DUMP_VECTORS=1`
3. Run tests: `cargo test`
4. Copy from `target/test_vectors/` to this directory
5. Or use the MDK CLI: `mdk test-vectors --output ./`
