//! Player export ABI with the same ownership, panic and cancellation rules as entities.
use crate::{
    boundary::{cstring, handle, output, run, string},
    sc_blob, sc_handle, sc_progress_cb, sc_status,
};
use scviewer_core::{CancelToken, PlayerClipsRequest, PlayerRequest, Session};
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
            let request = PlayerRequest {
                gender: gender.to_owned(),
                loadout: if loadout_or_null.is_null() {
                    None
                } else {
                    Some(string(loadout_or_null)?.to_owned())
                },
                ..Default::default()
            };
            export(h, &request, progress, user, out)
        })
    }
}

/// Export a contract-v2 player kit from a JSON request: {"gender": "male"|"female",
/// "loadout": <path>|null, "weapons": [<entity class>...]|null, "spare_attachments": bool}
/// (scviewer_core::PlayerRequest; missing fields take its defaults). Same ownership,
/// progress and cancellation rules as scbridge_export_player.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scbridge_export_player_request(
    h: *mut sc_handle,
    request_json: *const c_char,
    progress: sc_progress_cb,
    user: *mut c_void,
    out_tar: *mut sc_blob,
) -> sc_status {
    unsafe {
        run(h, || {
            let out = output(out_tar, sc_blob::default())?;
            let request: PlayerRequest = serde_json::from_str(string(request_json)?)
                .map_err(|e| crate::boundary::Failure::invalid(&format!("player request: {e}")))?;
            export(h, &request, progress, user, out)
        })
    }
}

/// Export body animation sidecars from a JSON request {"gender":"male"|"female","clips":[<name>...]}.
/// The result is a clip-only tar with the same progress, cancellation and ownership rules as a player kit.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scbridge_export_player_clips(
    h: *mut sc_handle,
    request_json: *const c_char,
    progress: sc_progress_cb,
    user: *mut c_void,
    out_tar: *mut sc_blob,
) -> sc_status {
    unsafe {
        run(h, || {
            let out = output(out_tar, sc_blob::default())?;
            let request: PlayerClipsRequest = serde_json::from_str(string(request_json)?)
                .map_err(|e| crate::boundary::Failure::invalid(&format!("player clip request: {e}")))?;
            validate_gender(&request.gender)?;
            if request.names().is_empty() {
                return Err(crate::boundary::Failure::invalid("no player action clips requested"));
            }
            export_tar(h, progress, user, out, |session, cancel| session.export_player_clips(&request, Some(cancel)))
        })
    }
}

fn validate_gender(gender: &str) -> Result<(), crate::boundary::Failure> {
    if !matches!(gender.to_ascii_lowercase().as_str(), "male" | "female") {
        return Err(crate::boundary::Failure::invalid("gender must be male or female"));
    }
    Ok(())
}

unsafe fn export(
    h: *mut sc_handle,
    request: &PlayerRequest,
    progress: sc_progress_cb,
    user: *mut c_void,
    out: &mut sc_blob,
) -> Result<(), crate::boundary::Failure> {
    validate_gender(&request.gender)?;
    unsafe { export_tar(h, progress, user, out, |session, cancel| session.export_player(request, Some(cancel))) }
}

unsafe fn export_tar(
    h: *mut sc_handle,
    progress: sc_progress_cb,
    user: *mut c_void,
    out: &mut sc_blob,
    build: impl FnOnce(&Session, &CancelToken) -> anyhow::Result<Vec<u8>>,
) -> Result<(), crate::boundary::Failure> {
    unsafe {
        {
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
                build(&h.session, &cancel)
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
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn invalid_player_clip_arguments_clear_the_blob() {
        let mut blob = sc_blob { data: std::ptr::dangling_mut(), len: 9 };
        let status = unsafe {
            scbridge_export_player_clips(std::ptr::null_mut(), c"{\"gender\":\"male\",\"clips\":[]}".as_ptr(),
                None, std::ptr::null_mut(), &mut blob)
        };
        assert_eq!(status, sc_status::SC_STATUS_INVALID_ARGUMENT);
        assert!(blob.data.is_null());
        assert_eq!(blob.len, 0);
    }
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
