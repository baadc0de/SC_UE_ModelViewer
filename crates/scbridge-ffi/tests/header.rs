//! CI drift guard: the checked-in ABI must be exactly the pinned cbindgen output.
#[test]
fn generated_header_matches() {
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR"));
    let bindings = cbindgen::Builder::new()
        .with_crate(root)
        .with_config(cbindgen::Config::from_file(root.join("cbindgen.toml")).unwrap())
        .generate()
        .unwrap();
    let mut generated = Vec::new();
    bindings.write(&mut generated);
    assert_eq!(
        std::fs::read(root.join("include/scbridge.h")).unwrap(),
        generated,
        "regenerate with cargo run -p scbridge-ffi --example header"
    );
}
