//! SCBridge C ABI: opaque sessions, JSON catalogues and owned in-memory tar blobs.
//! All exported functions contain Rust panics. The caller serializes handle use.
#![allow(non_camel_case_types)]
// The generated header states the shared pointer/lifetime safety contract.
#![allow(clippy::missing_safety_doc)]
mod dds;
mod audio;
mod audio_dsp;
pub use audio::{sc_audio_close, sc_audio_loop_region, sc_audio_open, sc_audio_read, sc_audio_seek, sc_audio_stream};
pub use audio_dsp::{
    sc_eq, sc_eq_free, sc_eq_new, sc_eq_process, sc_eq_set_band, sc_eq_set_output, sc_limiter, sc_limiter_free, sc_limiter_latency, sc_limiter_new,
    sc_limiter_process, sc_limiter_stats, sc_meter, sc_meter_free, sc_meter_new, sc_meter_process, sc_meter_set_offset,
};
pub use dds::{sc_dds_prepare, sc_dds_mean_rgb};
mod boundary;
mod cache;
mod types;
mod player;
pub use player::scbridge_export_player;
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

/// Fill defaults from the core (LOD 0, mip 2, textures, attachments/interior/lights/
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

/// Return one DataCore record as a JSON object: the record `name_or_guid` names, as `Type.Name`
/// (e.g. `EntityClassDefinition.GRIN_TractorBeam_S1`), a bare record name (exact, case-insensitive) or a
/// GUID. The `starbreaker dcb query` value shape: objects carry their struct type in `__type`, references
/// are record GUID strings (resolve them with another call), plus `__record_name` / `__record_id`.
/// No match, or a bare name several records share, is SC_STATUS_ERROR (the message lists the matches).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_datacore_record(
    h: *mut sc_handle,
    name_or_guid: *const c_char,
    out_json: *mut *mut c_char,
) -> sc_status {
    unsafe {
        run(h, || {
            json(out_json, || {
                Ok(handle(h)?.session.datacore_record(string(name_or_guid)?)?)
            })
        })
    }
}

/// Hand owned bytes to C as a blob (freed with sc_free_blob).
fn into_blob(out: &mut sc_blob, bytes: Vec<u8>) {
    let mut bytes = bytes.into_boxed_slice();
    *out = sc_blob {
        data: bytes.as_mut_ptr(),
        len: bytes.len(),
    };
    std::mem::forget(bytes);
}

/// Read the archive file at path (e.g. `Data/Libs/Particles/Gadgets/tractor_beam.xml`; either slash style,
/// case-insensitive) into an owned blob: the raw bytes as stored, so binary CryXML stays binary.
/// A missing file is SC_STATUS_ERROR. On ordinary failure out_blob is empty. Free with sc_free_blob.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_p4k_read(
    h: *mut sc_handle,
    path: *const c_char,
    out_blob: *mut sc_blob,
) -> sc_status {
    unsafe {
        run(h, || {
            let out = output(out_blob, sc_blob::default())?;
            let path = string(path)?;
            into_blob(out, handle(h)?.session.p4k_read(path)?);
            Ok(())
        })
    }
}

/// Read an archive texture (e.g. the `texture` of a particle emitter, `Data/Textures/VFX/Glows/glow4_alpha.dds`;
/// either slash style, case-insensitive) as one DDS file: the split mips (`.dds.1` …) merged, from mip `mip` down,
/// laid out like an exported package's textures, so sc_dds_prepare accepts it. A missing or non-DDS file is
/// SC_STATUS_ERROR. On ordinary failure out_blob is empty. Free with sc_free_blob.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_p4k_read_dds(
    h: *mut sc_handle,
    path: *const c_char,
    mip: u32,
    out_blob: *mut sc_blob,
) -> sc_status {
    unsafe {
        run(h, || {
            let out = output(out_blob, sc_blob::default())?;
            let path = string(path)?;
            into_blob(out, handle(h)?.session.p4k_read_dds(path, mip)?);
            Ok(())
        })
    }
}

/// Read the archive file at path as UTF-8 text XML: binary CryXML (`CryXmlB`) is converted, element text kept;
/// plain XML passes through unchanged. A file that is neither CryXML nor UTF-8, or is missing, is
/// SC_STATUS_ERROR. Owned string (free with sc_free_string); on ordinary failure *out_text is NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_p4k_read_xml(
    h: *mut sc_handle,
    path: *const c_char,
    out_text: *mut *mut c_char,
) -> sc_status {
    unsafe {
        run(h, || {
            let out = output(out_text, ptr::null_mut())?;
            let path = string(path)?;
            let text = handle(h)?.session.p4k_read_xml(path)?;
            *out = cstring(&text).into_raw();
            Ok(())
        })
    }
}

/// Return a particle effect as a JSON object. reference is `<GUID>|<library>.<effect>` (GUID optional), e.g.
/// `2c211ca4-c915-b0c7-5d0e-58fe5c503a16|Gadgets/tractor_beam.fps.beam_only`: the library is
/// `Data/Libs/Particles/<library>.xml`, split at the first '.' after the last '/'. The GUID must be the effect's
/// and wins when the name names another effect. Shape: {reference, library, library_path, effect, guid,
/// matched_by, emitters: [{path, name, parent (index, -1 first), guid, enabled, inheritance, system, spawn,
/// lifetime, size, aspect, color, alpha, speed, emit_angle_deg, focus_angle_deg, gravity_scale, air_resistance,
/// turbulence, position_offset_m, random_offset_m, facing, orient_to_velocity, blend, emissive_lighting,
/// texture (archive .dds path or null), material, geometry, tiles, anim, light, flare, spline,
/// raw (each Params attribute as written), inherited, defaulted}]}. Inheritance="Parent" emitters take omitted
/// params from their parent; the rest from system defaults inferred from the game data. Curves are [t, v...] keys
/// over particle age (age_curve) or emitter strength (strength_curve). See scviewer-core `particles` docs.
/// No such library or effect is SC_STATUS_ERROR.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_particle_effect(
    h: *mut sc_handle,
    reference: *const c_char,
    out_json: *mut *mut c_char,
) -> sc_status {
    unsafe {
        run(h, || {
            json(out_json, || {
                Ok(handle(h)?.session.particle_effect(string(reference)?)?)
            })
        })
    }
}

/// Return a MaterialEffects FXLib entry as a JSON object. library_and_effect is `<FXLib>:<effect>`, e.g.
/// `sc_ships_surface_impacts:Metal_Small` (`Data/Libs/MaterialEffects/FXLibs/<FXLib>.xml`, `<Effect name>`,
/// case-insensitive). Shape: {library, library_path, effect, attributes, particles: [{reference (for
/// sc_particle_effect), attributes (the <Particle> and <Name> attributes, e.g. minscale), direction, random_group}],
/// audio: [{trigger, ..., random_group}], random_groups}. Entries of one <RandEffect> share a random_group (one of
/// them plays); others have null. No such library or effect is SC_STATUS_ERROR.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_material_effect(
    h: *mut sc_handle,
    library_and_effect: *const c_char,
    out_json: *mut *mut c_char,
) -> sc_status {
    unsafe {
        run(h, || {
            json(out_json, || {
                Ok(handle(h)?.session.material_effect(string(library_and_effect)?)?)
            })
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

/// Return a JSON array of object-container paths (`.socpak`, slash-normalised) whose path contains every
/// whitespace-separated term of query (case-insensitive): game environments such as hangars ("hangar").
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_search_environments(
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
                    .search_environments(string(query)?, limit as usize))
            })
        })
    }
}

/// Export a game environment (object container, a path from sc_search_environments) as a decomposed package tar,
/// cached like entity packages. Same ownership, progress, cancellation and failure rules as sc_export_decomposed_mem.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_export_environment_mem(
    h: *mut sc_handle,
    socpak: *const c_char,
    options: *const sc_options,
    progress: sc_progress_cb,
    user: *mut c_void,
    out_tar: *mut sc_blob,
) -> sc_status {
    unsafe {
        run(h, || {
            let out = output(out_tar, sc_blob::default())?;
            let options = options.as_ref().copied().unwrap_or_default().core()?;
            let socpak = string(socpak)?;
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
                h.session
                    .export_environment_decomposed_mem_cached(socpak, &options, Some(&cancel))
                    .map(|cached| cached.value)
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

#[cfg(test)]
mod dds_tests;
