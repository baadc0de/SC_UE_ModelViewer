//! Generate or diff-check the public C header using the pinned cbindgen version.
fn main() {
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR"));
    let config = cbindgen::Config::from_file(root.join("cbindgen.toml")).unwrap();
    let bindings = cbindgen::Builder::new()
        .with_crate(root)
        .with_config(config)
        .generate()
        .expect("generate scbridge.h");
    let mut bytes = Vec::new();
    bindings.write(&mut bytes);
    let path = root.join("include/scbridge.h");
    if std::env::args().any(|arg| arg == "--check") {
        assert_eq!(std::fs::read(path).unwrap(), bytes, "scbridge.h is stale");
        println!("scbridge.h matches cbindgen output");
    } else {
        std::fs::write(path, bytes).unwrap();
    }
}
