//! Give the Linux cdylib a stable SONAME for linkers and the UE runtime loader.
fn main() {
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("linux") {
        println!("cargo:rustc-link-arg-cdylib=-Wl,-soname,libscbridge.so");
    }
}
