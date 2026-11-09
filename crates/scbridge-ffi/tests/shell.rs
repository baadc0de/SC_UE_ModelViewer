//! Fault-inject the C runner without an archive, compiler or nested Cargo build.
#[test]
fn shell_runner_propagates_failures_and_cleans_outputs() {
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../..");
    let result = std::process::Command::new("python3")
        .args(["-I", "crates/scbridge-ffi/ctest/test_run.py"])
        .current_dir(root)
        .status()
        .expect("run shell fault injection");
    assert!(result.success());
}
