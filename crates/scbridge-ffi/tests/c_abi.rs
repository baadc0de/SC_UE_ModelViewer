//! Run a real C caller and validate its live archive export without a build script.
#[test]
fn c_abi_live_export() {
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../..");
    let result = std::process::Command::new("bash")
        .arg("crates/scbridge-ffi/ctest/run.sh")
        .current_dir(root)
        .status()
        .expect("run C integration test");
    assert!(result.success(), "C integration test failed: {result}");
}
