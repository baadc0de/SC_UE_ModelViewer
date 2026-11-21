//! C ABI for listing authored music cues and reading one locally converted Ogg payload.
use crate::{
    boundary::{handle, output, run, string},
    into_blob, json, sc_blob, sc_handle, sc_status,
};
use std::ffi::c_char;

/// Return the archive's named whole-source music cues as JSON. Free with sc_free_string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_music_catalogue(
    h: *mut sc_handle,
    out_json: *mut *mut c_char,
) -> sc_status {
    unsafe {
        run(h, || {
            json(out_json, || Ok(handle(h)?.session.music_catalogue()?))
        })
    }
}

/// Return one catalogue cue as Ogg bytes. Free with sc_free_blob.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_music_read(
    h: *mut sc_handle,
    cue_id: *const c_char,
    out_blob: *mut sc_blob,
) -> sc_status {
    unsafe {
        run(h, || {
            let out = output(out_blob, sc_blob::default())?;
            into_blob(out, handle(h)?.session.music_read(string(cue_id)?)?);
            Ok(())
        })
    }
}
