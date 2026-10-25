use std::{env, path::PathBuf};

fn main() {
    let lib = pkg_config::Config::new()
        .atleast_version("0.2.0")
        .probe("vault")
        .expect("install libvault >= 0.2.0 and set PKG_CONFIG_PATH to its prefix");
    let mut builder = bindgen::Builder::default()
        .header("wrapper.h")
        .allowlist_function("vault_.*")
        .allowlist_type("vault_.*")
        .allowlist_var("VAULT_.*")
        // Share the actual Rust types across crates, not duplicate C layouts.
        .blocklist_type("porthome_.*")
        .blocklist_type("_NostrEvent")
        .raw_line("use porthome_sys::*;")
        .derive_debug(false)
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()));
    for path in lib.include_paths {
        builder = builder.clang_arg(format!("-I{}", path.display()));
    }
    builder
        .generate()
        .expect("generate bindings from installed vault headers (libclang is required)")
        .write_to_file(PathBuf::from(env::var_os("OUT_DIR").unwrap()).join("bindings.rs"))
        .expect("write vault bindings");
}
