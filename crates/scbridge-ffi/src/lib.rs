//! SCBridge C ABI: opaque sessions, JSON catalogues and owned in-memory tar blobs.
//! All exported functions contain Rust panics. The caller serializes handle use.
#![allow(non_camel_case_types)]
// The generated header states the shared pointer/lifetime safety contract.
#![allow(clippy::missing_safety_doc)]
mod boundary;
mod cache;
mod types;
use boundary::{Result, cstring, handle, output, run, string};
use scviewer_core::{CancelToken, Session};
use std::{
    ffi::{CString, c_char, c_void},
    path::{Path, PathBuf},
    ptr,
    sync::Arc,
};
pub use types::*;

/// Opaque single-owner session. Close once; never use after close.
pub struct sc_handle {
    session: Session,
    error: CString,
    poisoned: bool,
}

/// NULL p4k auto-discovers the install; explicit p4k paths must be UTF-8.
/// NULL cache disables persistence. The host must pass an absolute cache path
/// resolved from its launch directory, since UE changes cwd during startup.
/// On failure *out is NULL; read
/// sc_last_error(NULL) on the same thread for errors before a handle exists.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_open(
    p4k_or_null: *const c_char,
    cache_dir_or_null: *const c_char,
    out: *mut *mut sc_handle,
) -> sc_status {
    unsafe {
        run(ptr::null_mut(), || {
            let out = output(out, ptr::null_mut())?;
            let path = if p4k_or_null.is_null() {
                None
            } else {
                Some(Path::new(string(p4k_or_null)?))
            };
            let cache_dir = if cache_dir_or_null.is_null() {
                None
            } else {
                Some(PathBuf::from(string(cache_dir_or_null)?))
            };
            let session = Session::open(path, cache_dir.as_deref())?;
            *out = Box::into_raw(Box::new(sc_handle {
                session,
                error: CString::default(),
                poisoned: false,
            }));
            Ok(())
        })
    }
}

/// Fill defaults from the core (LOD 1, mip 2, textures, attachments/interior/lights/
/// animations enabled, shields/default pose disabled). NULL export options uses these too.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_default_options(out: *mut sc_options) -> sc_status {
    unsafe {
        run(ptr::null_mut(), || {
            output(out, sc_options::default())?;
            Ok(())
        })
    }
}

unsafe fn json<T: serde::Serialize>(
    out: *mut *mut c_char,
    f: impl FnOnce() -> Result<T>,
) -> Result<()> {
    let out = unsafe { output(out, ptr::null_mut()) }?;
    let bytes = serde_json::to_string(&f()?).map_err(anyhow::Error::from)?;
    *out = cstring(&bytes).into_raw();
    Ok(())
}

/// Return a JSON array of entity hits; query is a case-insensitive substring.
/// All JSON outputs are owned; on ordinary failure *out is NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_search_entities(
    h: *mut sc_handle,
    query: *const c_char,
    limit: u32,
    out: *mut *mut c_char,
) -> sc_status {
    unsafe {
        run(h, || {
            json(out, || {
                Ok(handle(h)?
                    .session
                    .search_entities(string(query)?, limit as usize))
            })
        })
    }
}

/// Return the core Catalogue JSON object. Entity must be an exact name (case-insensitive).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_clip_catalogue(
    h: *mut sc_handle,
    entity: *const c_char,
    out: *mut *mut c_char,
) -> sc_status {
    unsafe {
        run(h, || {
            json(out, || {
                Ok(handle(h)?.session.clip_catalogue(string(entity)?)?)
            })
        })
    }
}

/// Return the core LightCatalogue JSON object {entity, ports, lights}.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_lights(
    h: *mut sc_handle,
    entity: *const c_char,
    out: *mut *mut c_char,
) -> sc_status {
    unsafe {
        run(h, || {
            json(out, || Ok(handle(h)?.session.lights(string(entity)?)?))
        })
    }
}

/// Read core cache statistics; broken manifests recover under the cache lock.
/// Missing/disabled cache reports zero bytes/entries; last_hit is unchanged.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_cache_stats(h: *mut sc_handle, out: *mut sc_cache_info) -> sc_status {
    unsafe {
        run(h, || {
            let out = output(out, sc_cache_info::default())?;
            *out = cache::stats(handle(h)?)?;
            Ok(())
        })
    }
}

/// Evict LRU entries to cap_bytes and return bytes removed. Zero clears the disk cache.
/// This is a one-shot eviction; future core writes retain the default automatic cap.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_cache_evict(
    h: *mut sc_handle,
    cap_bytes: u64,
    removed: *mut u64,
) -> sc_status {
    unsafe {
        run(h, || {
            let removed = output(removed, 0)?;
            *removed = handle(h)?.session.cache_evict(cap_bytes)?;
            Ok(())
        })
    }
}

/// Exact core/CLI uncompressed tar bytes: scene.json first, remaining paths sorted.
/// POSIX ustar, regular typeflag '0', name/prefix splitting; PAX 'x' path only for
/// unsplittable names. No GNU longlink entries. All callbacks finish before return.
/// On ordinary failure out_tar is empty; after PANIC output is unspecified and
/// must not be used. Only PANIC poisons the handle and requires close/reopen;
/// ordinary failures leave it usable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_export_decomposed_mem(
    h: *mut sc_handle,
    entity: *const c_char,
    options: *const sc_options,
    progress: sc_progress_cb,
    user: *mut c_void,
    out_tar: *mut sc_blob,
) -> sc_status {
    unsafe {
        run(h, || {
            let out = output(out_tar, sc_blob::default())?;
            let options = options.as_ref().copied().unwrap_or_default().core()?;
            let entity = string(entity)?;
            let h = handle(h)?;
            // C explicitly promises user remains valid and thread-safe for this call.
            let user_address = user as usize;
            let cancel = CancelToken::default();
            let callback_cancel = cancel.clone();
            h.session.set_progress_callback(progress.map(|cb| {
                Arc::new(move |fraction: f32, phase: &str| {
                    let phase = cstring(phase);
                    if cb(
                        phase.as_ptr(),
                        (fraction.clamp(0.0, 1.0) * 1000.0) as u64,
                        1000,
                        user_address as *mut c_void,
                    ) != 0
                    {
                        callback_cancel.cancel();
                    }
                }) as scviewer_core::ProgressCallback
            }));
            // Clear the borrowed callback context even when the core unwinds.
            let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                h.session
                    .export_entity_decomposed_mem(entity, &options, Some(&cancel))
            }));
            h.session.set_progress_callback(None);
            let bytes = match result {
                Ok(result) => result?,
                Err(payload) => std::panic::resume_unwind(payload),
            };
            let mut bytes = bytes.into_boxed_slice();
            *out = sc_blob {
                data: bytes.as_mut_ptr(),
                len: bytes.len(),
            };
            std::mem::forget(bytes);
            Ok(())
        })
    }
}

/// Borrowed error, valid until the next status-returning call on this handle or close.
/// NULL handle reads thread-local open/argument errors, valid until the next call
/// on this thread other than sc_last_error. Never free this pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_last_error(h: *mut sc_handle) -> *const c_char {
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| unsafe {
        boundary::error_ptr(h)
    }))
    .unwrap_or(c"error retrieval panicked".as_ptr())
}

/// Free an owned JSON string once; NULL is a no-op.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_free_string(value: *mut c_char) {
    unsafe {
        run(ptr::null_mut(), || {
            if !value.is_null() {
                drop(CString::from_raw(value));
            }
            Ok(())
        });
    }
}

/// Free blob and reset data/len to zero; NULL and empty blobs are no-ops.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_free_blob(blob: *mut sc_blob) {
    unsafe {
        run(ptr::null_mut(), || {
            if let Some(blob) = blob.as_mut() {
                if !blob.data.is_null() {
                    drop(Box::from_raw(ptr::slice_from_raw_parts_mut(
                        blob.data, blob.len,
                    )));
                }
                *blob = sc_blob::default();
            }
            Ok(())
        });
    }
}

/// Destroy the handle, including a poisoned handle. NULL is a no-op.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_close(h: *mut sc_handle) {
    unsafe {
        run(ptr::null_mut(), || {
            if !h.is_null() {
                drop(Box::from_raw(h));
            }
            Ok(())
        });
    }
}

/// Test builds only: force an unwinding panic through the production boundary.
#[cfg(any(test, feature = "test-hooks"))]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_test_panic(h: *mut sc_handle) -> sc_status {
    unsafe {
        run(h, || {
            panic!("forced WP-R4 panic");
        })
    }
}

#[cfg(test)]
mod tests;
