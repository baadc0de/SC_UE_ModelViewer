//! Handle-free package DDS adaptation and linear mean C entry points.
use crate::{
    boundary::{self, Failure, output},
    sc_dds_info, sc_status,
};
use std::ptr;

/// Validate and adapt a package DDS in place. SNORM becomes UNORM; normal_map
/// requires BC5 and flips green. On failure data is unchanged and *out is zero.
/// data is writable for len bytes; out must not overlap data. Errors are read
/// with sc_last_error(NULL) on this thread. Call once per imported texture.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_dds_prepare(
    data: *mut u8,
    len: usize,
    normal_map: bool,
    out: *mut sc_dds_info,
) -> sc_status {
    unsafe {
        boundary::run(ptr::null_mut(), || {
            let out = output(out, sc_dds_info::default())?;
            if data.is_null() || len > isize::MAX as usize {
                return Err(Failure::invalid("NULL DDS data or invalid length"));
            }
            let info =
                scviewer_core::dds::prepare(std::slice::from_raw_parts_mut(data, len), normal_map)?;
            *out = sc_dds_info {
                dxgi_format: info.dxgi_format,
                width: info.width,
                height: info.height,
                mip_count: info.mip_count,
                data_offset: info.data_offset,
                data_size: info.data_size,
            };
            Ok(())
        })
    }
}

/// Linear-light mean RGB of the top mip, linearizing sRGB texels before averaging.
/// data is readable for len bytes; out_rgb points to three writable floats and
/// must not overlap data. On failure RGB is zero. Errors use sc_last_error(NULL).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_dds_mean_rgb(
    data: *const u8,
    len: usize,
    out_rgb: *mut f32,
) -> sc_status {
    unsafe {
        boundary::run(ptr::null_mut(), || {
            let out = output(out_rgb.cast::<[f32; 3]>(), [0.0; 3])?;
            if data.is_null() || len > isize::MAX as usize {
                return Err(Failure::invalid("NULL DDS data or invalid length"));
            }
            *out = scviewer_core::dds::mean_rgb(std::slice::from_raw_parts(data, len))?;
            Ok(())
        })
    }
}
