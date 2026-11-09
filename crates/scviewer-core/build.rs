//! Stamp cache identity with the vendored exporter revision and dirty state.
use sha2::{Digest, Sha256};
use std::{fs, path::Path, process::Command};

fn git(vendor: &Path, args: &[&str]) -> Option<String> {
    let output = Command::new("git")
        .arg("-C")
        .arg(vendor)
        .args(args)
        .output()
        .ok()?;
    output
        .status
        .success()
        .then(|| String::from_utf8_lossy(&output.stdout).trim().to_owned())
}
fn main() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..");
    let vendor = root
        .join("vendor/StarBreaker")
        .canonicalize()
        .expect("vendored StarBreaker");
    // Watch sources/untracked files for clean/dirty transitions as well as git's
    // actual metadata directory (the submodule's .git is usually a pointer file).
    println!("cargo:rerun-if-changed={}", vendor.display());
    if let Some(git_dir) = git(&vendor, &["rev-parse", "--absolute-git-dir"]) {
        for name in ["HEAD", "index", "packed-refs", "refs"] {
            let path = Path::new(&git_dir).join(name);
            if path.exists() {
                println!("cargo:rerun-if-changed={}", path.display());
            }
        }
    }
    println!("cargo:rerun-if-env-changed=PATH");
    let identity = git(&vendor, &["rev-parse", "HEAD"])
        .zip(git(&vendor, &["status", "--porcelain"]))
        .filter(|_| git(&vendor, &["rev-parse", "--show-toplevel"]).as_deref() == vendor.to_str());
    let (revision, dirty) = match identity {
        Some((revision, status)) => (revision, !status.is_empty()),
        None => {
            let lock = fs::read(vendor.join("Cargo.lock")).expect("vendored Cargo.lock fallback");
            (format!("lock-sha256-{:x}", Sha256::digest(lock)), false)
        }
    };
    println!("cargo:rustc-env=SCVIEWER_EXPORTER_REVISION={revision}");
    println!("cargo:rustc-env=SCVIEWER_EXPORTER_DIRTY={dirty}");
    println!("cargo:rustc-env=SCVIEWER_EXPORTER_VERSION={revision}:dirty={dirty}");
}
