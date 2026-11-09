//! Player export ABI with the same ownership, panic and cancellation rules as entities.
use crate::{
    boundary::{cstring, handle, output, run, string},
    sc_blob, sc_handle, sc_progress_cb, sc_status,
};
use scviewer_core::{CancelToken, PlayerRequest};
use std::{
    ffi::{c_char, c_void},
    sync::Arc,
};

/// Export a contract-v2 player kit. NULL gender means male; NULL loadout chooses
/// the first default loadout in name order with a stowed personal weapon.
/// Strings are UTF-8. Release out_tar with sc_free_blob. Progress and cancellation
/// follow sc_export_decomposed_mem; assembly is a single cancellation phase.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scbridge_export_player(
    h: *mut sc_handle,
    gender_or_null: *const c_char,
    loadout_or_null: *const c_char,
    progress: sc_progress_cb,
    user: *mut c_void,
    out_tar: *mut sc_blob,
) -> sc_status {
    unsafe {
        run(h, || {
            let out = output(out_tar, sc_blob::default())?;
            let gender = if gender_or_null.is_null() {
                "male"
            } else {
                string(gender_or_null)?
            };
            if !matches!(gender.to_ascii_lowercase().as_str(), "male" | "female") {
                return Err(crate::boundary::Failure::invalid(
                    "gender must be male or female",
                ));
            }
            let request = PlayerRequest {
                gender: gender.to_owned(),
                loadout: if loadout_or_null.is_null() {
                    None
                } else {
                    Some(string(loadout_or_null)?.to_owned())
                },
            };
            let h = handle(h)?;
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
            let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                h.session.export_player(&request, Some(&cancel))
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

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn invalid_player_arguments_clear_the_blob() {
        let mut blob = sc_blob {
            data: std::ptr::dangling_mut(),
            len: 9,
        };
        let status = unsafe {
            scbridge_export_player(
                std::ptr::null_mut(),
                c"invalid".as_ptr(),
                std::ptr::null(),
                None,
                std::ptr::null_mut(),
                &mut blob,
            )
        };
        assert_eq!(status, sc_status::SC_STATUS_INVALID_ARGUMENT);
        assert!(blob.data.is_null());
        assert_eq!(blob.len, 0);
    }
}
