use serde_json::Value;
use std::{
    collections::BTreeMap,
    fs,
    path::{Path, PathBuf},
    process::Command,
    sync::atomic::{AtomicUsize, Ordering},
};

fn root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../..")
}
struct Outputs(PathBuf);
impl Outputs {
    fn new() -> Self {
        static NEXT: AtomicUsize = AtomicUsize::new(0);
        let p = Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("target/test-output")
            .join(format!(
                "{}-{}",
                std::process::id(),
                NEXT.fetch_add(1, Ordering::Relaxed)
            ));
        fs::create_dir_all(&p).unwrap();
        Self(p)
    }
    fn export(&self, name: &str) -> PathBuf {
        let p = self.0.join(name);
        let out = Command::new(env!("CARGO_BIN_EXE_fips-vectors"))
            .args(["export", "--out"])
            .arg(&p)
            .output()
            .unwrap();
        assert!(
            out.status.success(),
            "{}",
            String::from_utf8_lossy(&out.stderr)
        );
        p
    }
}
impl Drop for Outputs {
    fn drop(&mut self) {
        fs::remove_dir_all(&self.0).unwrap();
    }
}
fn files(p: &Path) -> BTreeMap<String, Vec<u8>> {
    fs::read_dir(p)
        .unwrap()
        .map(|e| {
            let p = e.unwrap().path();
            (
                p.file_name().unwrap().to_str().unwrap().into(),
                fs::read(p).unwrap(),
            )
        })
        .collect()
}
#[test]
fn real_cli_is_deterministic_and_matches_checked_in_oracle() {
    let outputs = Outputs::new();
    let a = files(&outputs.export("a"));
    let b = files(&outputs.export("b"));
    assert_eq!(a, b, "two fresh CLI processes must produce identical bytes");
    assert_eq!(
        a,
        files(&root().join("libfips-core/tests/vectors")),
        "regenerate/check fixtures after intentional generator or pin changes"
    );
    fn check(v: &Value, all: &BTreeMap<String, Vec<u8>>, refs: &mut Vec<String>) {
        match v {
            Value::Array(a) => {
                for v in a {
                    check(v, all, refs)
                }
            }
            Value::Object(o) => {
                if let Some(file) = o.get("file") {
                    let name = file.as_str().unwrap();
                    let bytes = &all[name];
                    refs.push(name.into());
                    assert_eq!(o["pin"], "6aa80504baa0fa312b392009eea0e3e59fb6fe4c");
                    assert_eq!(o["generator_version"], env!("CARGO_PKG_VERSION"));
                    assert_eq!(o["length"], bytes.len());
                    assert_eq!(o["hex"], hex::encode(bytes));
                    assert_eq!(o["sha256"], hex::encode(fips_core::identity::sha256(bytes)));
                }
                for v in o.values() {
                    check(v, all, refs)
                }
            }
            _ => {}
        }
    }
    let mut refs = Vec::new();
    for family in ["entropy", "identity", "codec", "noise", "proto", "rekey"] {
        let v: Value = serde_json::from_slice(&a[&format!("{family}.json")]).unwrap();
        assert_eq!(v["family"], family);
        assert_eq!(v["pin"], "6aa80504baa0fa312b392009eea0e3e59fb6fe4c");
        assert_eq!(v["generator_version"], env!("CARGO_PKG_VERSION"));
        check(&v, &a, &mut refs);
    }
    refs.sort();
    refs.dedup();
    assert_eq!(
        refs,
        a.keys()
            .filter(|k| k.ends_with(".bin"))
            .cloned()
            .collect::<Vec<_>>(),
        "no orphan/missing binary sidecars"
    );
}
#[test]
fn exported_entropy_agrees_with_provisional_c_header_and_tail_discard() {
    let outputs = Outputs::new();
    let out = outputs.export("entropy");
    let v: Value = serde_json::from_slice(&fs::read(out.join("entropy.json")).unwrap()).unwrap();
    let header = fs::read_to_string(root().join("libfips-core/tests/seeded_vectors.h")).unwrap();
    let mut groups = Vec::new();
    let mut current = String::new();
    for l in header.lines().map(str::trim).filter(|l| l.starts_with('"')) {
        current.push_str(l.split('"').nth(1).unwrap());
        if l.ends_with(',') {
            groups.push(std::mem::take(&mut current));
        }
    }
    assert_eq!(groups.len(), 3);
    for (case, expected) in v["cases"].as_array().unwrap().iter().zip(groups) {
        let stream = hex::decode(case["stream"]["hex"].as_str().unwrap()).unwrap();
        assert_eq!(stream.len(), 1024);
        assert_eq!(hex::encode(&stream), expected);
        let mut word_offset = 0;
        for c in case["chunks"].as_array().unwrap() {
            let n = c["length"].as_u64().unwrap() as usize;
            let bytes = hex::decode(c["bytes"]["hex"].as_str().unwrap()).unwrap();
            assert_eq!(bytes, stream[word_offset..word_offset + n]);
            word_offset += n.div_ceil(8) * 8;
        }
    }
}
#[test]
fn invalid_cli_is_a_visible_failure() {
    for args in [
        vec![],
        vec!["export", "--out"],
        vec!["export", "--wat"],
        vec!["export", "--out", "unused", "extra"],
    ] {
        let out = Command::new(env!("CARGO_BIN_EXE_fips-vectors"))
            .args(args)
            .output()
            .unwrap();
        assert!(!out.status.success());
        assert!(!out.stderr.is_empty());
    }
}

#[test]
fn fault_injection_is_rejected_before_writing() {
    let outputs = Outputs::new();
    for key in ["FIPS_NODE_FAULTS", "FIPS_REKEY_FAULTS"] {
        let p = outputs.0.join(key);
        let out = Command::new(env!("CARGO_BIN_EXE_fips-vectors"))
            .args(["export", "--out"])
            .arg(&p)
            .env(key, "all")
            .output()
            .unwrap();
        assert!(!out.status.success());
        assert!(String::from_utf8_lossy(&out.stderr).contains(key));
        assert!(!p.exists());
    }
}
