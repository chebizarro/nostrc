"""Operational workflow families; experimental receipt/custody policy is separate."""

BROAD_FAMILIES = {
    "core": "nostrc-ci", "libgo": "libgo-ci", "hanami": "libhanami-ci", "gi": "libnostr-gi",
    "groundhog": "groundhog-ci", "grotto": "grotto-ci", "nip55l": "nip55l-libsecret-ci",
    "homed": "nostr-homed-ci", "vault_syncd": "vault-syncd-ci", "signet": "signet-ci", "static": "static-checks",
    "appimage": "gnostr-appimage", "windows": "windows-validation", "bp": "bp-matrix",
    "bp_extended": "bp-matrix-nightly", "fuzz": "grotto-fuzz", "mdk08": "marmot-mdk-interop",
    "mdk011": "marmot-mdk011-interop", "rust": "rust-bindings-ci",
}
