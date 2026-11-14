mod basic;
mod noise;
mod proto;
mod rekey;
mod support;

use fips_core::identity::sha256;
use serde_json::{Value, json};
use std::{
    error::Error,
    fs,
    path::{Path, PathBuf},
};

type Result<T> = std::result::Result<T, Box<dyn Error>>;
const PIN: &str = "6aa80504baa0fa312b392009eea0e3e59fb6fe4c";
const VERSION: &str = env!("CARGO_PKG_VERSION");
const SEEDS: [u64; 3] = [0, 0x0123_4567_89ab_cdef, u64::MAX];
const EPOCH: [u8; 8] = [0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef];

struct Exporter<'a> {
    out: &'a Path,
    family: &'a str,
}
impl Exporter<'_> {
    fn bytes(&self, name: &str, bytes: &[u8]) -> Result<Value> {
        let file = format!("{}-{name}.bin", self.family);
        fs::write(self.out.join(&file), bytes)?;
        Ok(
            json!({"file":file, "hex":hex::encode(bytes), "length":bytes.len(),
                  "sha256":hex::encode(sha256(bytes)), "pin":PIN, "generator_version":VERSION}),
        )
    }
    fn finish(&self, cases: Value, gaps: &[&str]) -> Result<()> {
        let document = json!({"schema_version":1, "pin":PIN,
            "generator":"fips-vectors", "generator_version":VERSION,
            "family":self.family, "cases":cases, "gaps":gaps});
        let mut bytes = serde_json::to_vec_pretty(&document)?;
        bytes.push(b'\n');
        fs::write(self.out.join(format!("{}.json", self.family)), bytes)?;
        Ok(())
    }
}
fn export(out: &Path) -> Result<()> {
    for name in ["FIPS_NODE_FAULTS", "FIPS_REKEY_FAULTS"] {
        if std::env::var_os(name).is_some() {
            return Err(
                format!("unset {name}: oracle export must not enable mutation switches").into(),
            );
        }
    }
    fs::create_dir_all(out)?;
    // Deterministic context blinding, not OS entropy. Does not change signatures.
    assert!(fips_core::identity::init_secp(&[0x42; 32]));
    basic::entropy(&Exporter {
        out,
        family: "entropy",
    })?;
    basic::identity(&Exporter {
        out,
        family: "identity",
    })?;
    basic::codec(&Exporter {
        out,
        family: "codec",
    })?;
    noise::export(&Exporter {
        out,
        family: "noise",
    })?;
    proto::export(&Exporter {
        out,
        family: "proto",
    })?;
    rekey::export(&Exporter {
        out,
        family: "rekey",
    })?;
    Ok(())
}
fn default_out() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../libfips-core/tests/vectors")
}
fn run() -> Result<()> {
    let mut args = std::env::args().skip(1);
    if args.next().as_deref() != Some("export") {
        return Err("usage: fips-vectors export [--out <dir>]".into());
    }
    let out = match args.next().as_deref() {
        None => default_out(),
        Some("--out") => PathBuf::from(args.next().ok_or("--out needs a directory")?),
        _ => return Err("usage: fips-vectors export [--out <dir>]".into()),
    };
    if args.next().is_some() {
        return Err("unexpected argument".into());
    }
    export(&out)
}
fn main() {
    if let Err(e) = run() {
        eprintln!("fips-vectors: {e}");
        std::process::exit(1);
    }
}
