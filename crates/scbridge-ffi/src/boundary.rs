//! Panic containment, per-handle/thread errors and checked C pointer adapters.
use crate::{sc_handle, sc_status};
use std::{
    cell::RefCell,
    ffi::{CStr, CString, c_char},
    panic::{AssertUnwindSafe, catch_unwind},
};

pub(crate) struct Failure(pub sc_status, pub String);
pub(crate) type Result<T> = std::result::Result<T, Failure>;
impl Failure {
    pub(crate) fn invalid(message: &str) -> Self {
        Self(sc_status::SC_STATUS_INVALID_ARGUMENT, message.into())
    }
}
impl From<anyhow::Error> for Failure {
    fn from(error: anyhow::Error) -> Self {
        let status = if matches!(
            error.downcast_ref::<scviewer_core::Error>(),
            Some(scviewer_core::Error::Cancelled)
        ) {
            sc_status::SC_STATUS_CANCELLED
        } else {
            sc_status::SC_STATUS_ERROR
        };
        Self(status, format!("{error:#}"))
    }
}
thread_local! { static ERROR: RefCell<CString> = RefCell::new(CString::default()); }
pub(crate) fn cstring(message: &str) -> CString {
    CString::new(message.replace('\0', "\\0")).expect("NUL escaped")
}

// Callers guarantee any non-NULL handle remains exclusively accessible and live.
pub(crate) unsafe fn run(h: *mut sc_handle, f: impl FnOnce() -> Result<()>) -> sc_status {
    let result = catch_unwind(AssertUnwindSafe(|| {
        if let Some(handle) = unsafe { h.as_ref() } {
            if handle.poisoned {
                return Err(Failure(
                    sc_status::SC_STATUS_PANIC,
                    "handle panicked; close and reopen it".into(),
                ));
            }
        }
        f()
    }));
    // Contain error formatting/storage as well. Avoid dropping an arbitrary panic
    // payload, whose destructor could itself panic outside the original catch.
    catch_unwind(AssertUnwindSafe(|| {
        let failure = match result {
            Ok(Ok(())) => None,
            Ok(Err(e)) => Some(e),
            Err(payload) => {
                let payload = std::mem::ManuallyDrop::new(payload);
                let message = payload
                    .downcast_ref::<String>()
                    .map(String::as_str)
                    .or_else(|| payload.downcast_ref::<&str>().copied())
                    .unwrap_or("Rust panic");
                if let Some(handle) = unsafe { h.as_mut() } {
                    handle.poisoned = true;
                }
                Some(Failure(sc_status::SC_STATUS_PANIC, message.into()))
            }
        };
        let (status, message) =
            failure.map_or((sc_status::SC_STATUS_OK, String::new()), |e| (e.0, e.1));
        let error = cstring(&message);
        if let Some(handle) = unsafe { h.as_mut() } {
            handle.error = error;
        } else {
            ERROR.with(|slot| *slot.borrow_mut() = error);
        }
        status
    }))
    .unwrap_or(sc_status::SC_STATUS_PANIC)
}

pub(crate) unsafe fn handle<'a>(h: *mut sc_handle) -> Result<&'a mut sc_handle> {
    unsafe { h.as_mut() }.ok_or_else(|| Failure::invalid("NULL handle"))
}
pub(crate) unsafe fn string<'a>(value: *const c_char) -> Result<&'a str> {
    if value.is_null() {
        return Err(Failure::invalid("NULL string"));
    }
    unsafe { CStr::from_ptr(value) }
        .to_str()
        .map_err(|_| Failure::invalid("string is not UTF-8"))
}
pub(crate) unsafe fn output<'a, T>(out: *mut T, empty: T) -> Result<&'a mut T> {
    if out.is_null() {
        return Err(Failure::invalid("NULL output"));
    }
    // C callers may pass uninitialized output storage. Initialize through the
    // raw pointer before constructing a reference to a valid Rust value.
    unsafe { out.write(empty) };
    Ok(unsafe { &mut *out })
}
pub(crate) unsafe fn error_ptr(h: *mut sc_handle) -> *const c_char {
    if let Some(handle) = unsafe { h.as_ref() } {
        handle.error.as_ptr()
    } else {
        ERROR.with(|slot| slot.borrow().as_ptr())
    }
}
