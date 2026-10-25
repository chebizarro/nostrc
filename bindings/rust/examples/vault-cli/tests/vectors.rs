use std::{fs, process::Command};

// Independent C reference vectors: seed=00..1f, child node_id=20..3f.
// Keep the complete envelopes and addresses, not only Rust self-roundtrips.
const C_VECTORS: &str = include_str!("../../../../../libporthome/tests/test_seal_v2.c");
const VECTORS: &[(&[u8], &str, &str)] = &[
    (b"", "0226809564499106596a9f18169cdc77a69fb310bd96f28a962b92800f78eb8130e8c8bc6888680c52ea9c9a91aebe8a49630e040410c8bbc9850e2926", "66a051107eaf6bfaad6d3a2d46228c19e903aa78ab2c72b6ed0942a1134a955b"),
    (b"shared vault PCS v2\n", "020c7561865ae0ab7fc0af846aee333b2056b51b6647f6896bc0e36b27d7c757aac3f51c7f19bb9ab723b32dfa18464171a69614aca6b9b5bbe546c1a7", "81d541623c41de9fdee71f94e3ce854bda447722a2bdb183d7f5df6b5eae61b4"),
    (b"a\0", "0286f61591f1108800761e913374dd684e54cfb2434eab1fb634587e3c90e8de1a585185094b621cb69e1bce6029f0bc31ab03cd1c2dc51f9fc94ad82a", "5714531823eb0fbba6f6baf9bb64758465f4a673e094dd559f3f7c595ee14789"),
];
const BINARY_BLOB: &str = "02dabb3ddb0c7f95663abab8885d563265b5479cc3588bc383f3a424cb187ad3d08c4fc04e4a289fdcc8421947e6b4a4cf54a86570452c6a4423744571ea518c25d4be8029f12707274c0214495af4a985e60f8aa28a9a87cabdad14945117865bdb062b6e4d67c70dfdd071484f5ba6c8eaf2e67b2ee03de6a78b6567";
const BINARY_HASH: &str = "f383b7350894ac925d35759e0c9dca482b8e2b4c184337dadf767e02b09633bc";

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

#[test]
fn cli_matches_every_c_chunk_vector_and_refuses_wrong_address() {
    let seed: [u8; 32] = std::array::from_fn(|i| i as u8);
    let id: [u8; 32] = std::array::from_fn(|i| (i + 32) as u8);
    let mut root = [0u8; 32];
    let mut key = [0u8; 32];
    unsafe {
        assert_eq!(
            porthome_sys::porthome_root_node_key(seed.as_ptr(), root.as_mut_ptr()),
            0
        );
        assert_eq!(
            porthome_sys::porthome_child_node_key(
                root.as_ptr(),
                id.as_ptr(),
                std::ptr::null(),
                key.as_mut_ptr()
            ),
            0
        );
    }
    let dir = tempfile::tempdir().unwrap();
    fs::write(dir.path().join("key"), key).unwrap();
    let binary: Vec<u8> = (0..65).collect();
    for (plain, blob, address) in VECTORS.iter().copied().chain(std::iter::once((
        binary.as_slice(),
        BINARY_BLOB,
        BINARY_HASH,
    ))) {
        assert!(
            C_VECTORS.contains(blob) && C_VECTORS.contains(address),
            "C fixture changed"
        );
        fs::write(dir.path().join("input"), plain).unwrap();
        let result = Command::new(env!("CARGO_BIN_EXE_vault-cli"))
            .current_dir(dir.path())
            .args(["seal", "key", "input", "sealed"])
            .output()
            .unwrap();
        assert!(
            result.status.success(),
            "{}",
            String::from_utf8_lossy(&result.stderr)
        );
        assert_eq!(hex(&fs::read(dir.path().join("sealed")).unwrap()), blob);
        assert_eq!(
            String::from_utf8(result.stdout).unwrap(),
            format!("{address} {}\n", plain.len())
        );
        let result = Command::new(env!("CARGO_BIN_EXE_vault-cli"))
            .current_dir(dir.path())
            .args([
                "unseal",
                "key",
                "sealed",
                "opened",
                &plain.len().to_string(),
                address,
            ])
            .output()
            .unwrap();
        assert!(
            result.status.success(),
            "{}",
            String::from_utf8_lossy(&result.stderr)
        );
        assert_eq!(fs::read(dir.path().join("opened")).unwrap(), plain);
    }
    let result = Command::new(env!("CARGO_BIN_EXE_vault-cli"))
        .current_dir(dir.path())
        .args(["unseal", "key", "sealed", "refused", "65", &"0".repeat(64)])
        .output()
        .unwrap();
    assert!(!result.status.success());
    assert!(String::from_utf8_lossy(&result.stderr).contains("SHA256 mismatch"));
    assert!(!dir.path().join("refused").exists());
}

#[test]
fn additive_nip59_api_is_bound_and_linked() {
    let mut carrier = std::ptr::null_mut();
    unsafe {
        assert!(
            vault_sys::vault_nip59_carrier_new(std::ptr::null(), std::ptr::null(), &mut carrier)
                < 0
        );
        assert!(carrier.is_null());
        vault_sys::vault_nip59_carrier_free(carrier);
    }
}
