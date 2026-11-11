//! C ABI value types and strict conversion to the core decomposed options.
#![allow(non_camel_case_types)]
use crate::boundary::{Failure, Result};
use scviewer_core::{DecomposedOptions, GlbOptions, Materials};
use std::ffi::{c_char, c_int, c_void};

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum sc_status {
    SC_STATUS_OK = 0,
    SC_STATUS_INVALID_ARGUMENT = 1,
    SC_STATUS_ERROR = 2,
    SC_STATUS_PANIC = 3,
    SC_STATUS_UNSUPPORTED = 4,
    SC_STATUS_CANCELLED = 5,
}

/// Material mode constants; sc_options uses an integer so invalid C input is safe to reject.
#[repr(C)]
pub enum sc_material_mode {
    SC_MATERIAL_TEXTURES = 0,
    SC_MATERIAL_COLORS = 1,
    SC_MATERIAL_NONE = 2,
}

/// Use sc_default_options rather than zero initialization. Boolean fields accept 0 or 1.
/// animations=0 omits animation clips and sidecars.
/// collision_reserved must be 0; collision is built by the UE importer.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct sc_options {
    pub lod: u32,
    pub mip: u32,
    pub material_mode: u32,
    pub attachments: u32,
    pub interior: u32,
    pub lights: u32,
    pub shields: u32,
    pub animations: u32,
    pub default_pose: u32,
    pub collision_reserved: u32,
}
impl Default for sc_options {
    fn default() -> Self {
        let o = DecomposedOptions::default().geometry;
        Self {
            lod: o.lod,
            mip: o.mip,
            material_mode: match o.materials {
                Materials::Textures => 0,
                Materials::Colors => 1,
                Materials::None => 2,
            },
            attachments: o.attachments.into(),
            interior: o.interior.into(),
            lights: o.lights.into(),
            shields: o.shields.into(),
            animations: o.include_animations.into(),
            default_pose: o.apply_default_pose.into(),
            collision_reserved: 0,
        }
    }
}
impl sc_options {
    pub(crate) fn core(self) -> Result<DecomposedOptions> {
        fn flag(value: u32) -> Result<bool> {
            match value {
                0 => Ok(false),
                1 => Ok(true),
                _ => Err(Failure::invalid("boolean option must be 0 or 1")),
            }
        }
        let geometry = GlbOptions {
            lod: self.lod,
            mip: self.mip,
            materials: match self.material_mode {
                0 => Materials::Textures,
                1 => Materials::Colors,
                2 => Materials::None,
                _ => return Err(Failure::invalid("unknown material_mode")),
            },
            attachments: flag(self.attachments)?,
            interior: flag(self.interior)?,
            lights: flag(self.lights)?,
            shields: flag(self.shields)?,
            apply_default_pose: flag(self.default_pose)?,
            include_animations: flag(self.animations)?,
            ..Default::default()
        };
        if self.collision_reserved != 0 {
            return Err(Failure(
                sc_status::SC_STATUS_UNSUPPORTED,
                "collision_reserved must be 0".into(),
            ));
        }
        Ok(DecomposedOptions { geometry })
    }
}

/// Owned uncompressed tar. Do not change data/len; free exactly once via sc_free_blob.
#[repr(C)]
pub struct sc_blob {
    pub data: *mut u8,
    pub len: usize,
}
impl Default for sc_blob {
    fn default() -> Self {
        Self {
            data: std::ptr::null_mut(),
            len: 0,
        }
    }
}

/// Manifest snapshot (payload bytes only, excluding manifest/lock files).
/// last_hit reports the last successful core operation, including open/search.
/// cap_bytes is core's automatic write cap, not the last eviction request.
#[repr(C)]
#[derive(Default)]
pub struct sc_cache_info {
    pub bytes: u64,
    pub entries: u64,
    pub cap_bytes: u64,
    pub enabled: u32,
    pub last_hit: u32,
}

/// May fire from worker/rayon threads as well as the calling thread.
/// phase is borrowed UTF-8 valid only during this call; done/total is normalized
/// progress (0..1000), not a file count. Calls need not be monotonic across phases.
/// user must remain live until export returns; no callbacks occur after return.
/// Return nonzero to cancel: export returns SC_STATUS_CANCELLED, an empty blob,
/// and no new package cache entry. Cancellation is cooperative at phase boundaries.
/// No callback occurs after the final publication checkpoint (commit point).
/// Callback must be thread-safe, must not reenter the handle, throw, or unwind.
pub type sc_progress_cb = Option<
    unsafe extern "C" fn(phase: *const c_char, done: u64, total: u64, user: *mut c_void) -> c_int,
>;

/// Validated DDS payload description after adaptation; offsets are from data.
#[repr(C)]
#[derive(Debug, Default, Clone, Copy)]
pub struct sc_dds_info {
    pub dxgi_format: u32,
    pub width: u32,
    pub height: u32,
    pub mip_count: u32,
    pub data_offset: u64,
    pub data_size: u64,
}
