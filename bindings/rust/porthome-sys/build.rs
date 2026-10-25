use std::{env, path::PathBuf};

fn main() {
    let lib = pkg_config::Config::new()
        .atleast_version("0.4.0")
        .probe("porthome")
        .expect("install libporthome >= 0.4.0 and set PKG_CONFIG_PATH to its prefix");
    let mut builder = bindgen::Builder::default()
        .header("wrapper.h")
        .allowlist_function("porthome_.*")
        .allowlist_type("porthome_.*")
        .allowlist_var("PORTHOME_.*")
        .derive_debug(false)
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()));
    for path in lib.include_paths {
        builder = builder.clang_arg(format!("-I{}", path.display()));
    }
    builder
        .generate()
        .expect("generate bindings from installed porthome headers (libclang is required)")
        .write_to_file(PathBuf::from(env::var_os("OUT_DIR").unwrap()).join("bindings.rs"))
        .expect("write porthome bindings");
}
