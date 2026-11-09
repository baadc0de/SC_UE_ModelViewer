//! Cloneable cooperative cancellation and its typed anyhow error discriminator.
use std::sync::{
    Arc,
    atomic::{AtomicBool, Ordering},
};

/// Distinguishes a cooperative cancellation from ordinary export failures.
/// Export errors retain the core's anyhow API; inspect with downcast_ref::<Error>().
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Error {
    Cancelled,
}
impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Cancelled => f.write_str("export cancelled"),
        }
    }
}
impl std::error::Error for Error {}

/// One-way cancellation shared with a worker. Use a fresh token for each export.
/// Cancellation is observed at phase boundaries, not during individual decoding
/// operations. After the final publication checkpoint the operation commits and
/// returns success even if another thread subsequently cancels this token.
#[derive(Clone, Default)]
pub struct CancelToken(Arc<AtomicBool>);
impl CancelToken {
    pub fn cancel(&self) {
        self.0.store(true, Ordering::Release);
    }
    pub fn is_cancelled(&self) -> bool {
        self.0.load(Ordering::Acquire)
    }
    pub(crate) fn checkpoint(&self) -> anyhow::Result<()> {
        if self.is_cancelled() {
            Err(Error::Cancelled.into())
        } else {
            Ok(())
        }
    }
}
