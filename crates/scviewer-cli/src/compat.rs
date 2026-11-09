//! Package-name binary alias; the established `scviewer` executable remains available.
#[path = "main.rs"]
mod cli;
fn main() -> anyhow::Result<()> {
    cli::main()
}
