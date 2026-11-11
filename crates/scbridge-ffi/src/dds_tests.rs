//! Handle-free DDS boundary checks, output initialization, and thread-local errors.
use crate::*;
use std::{ffi::CStr, ptr};
#[test]
fn dds_nulls_and_invalid_bytes_report_thread_local_errors() {
    unsafe {
        let mut info = sc_dds_info::default();
        assert_eq!(
            sc_dds_prepare(ptr::null_mut(), 0, false, &mut info),
            sc_status::SC_STATUS_INVALID_ARGUMENT
        );
        assert!(
            !CStr::from_ptr(sc_last_error(ptr::null_mut()))
                .to_bytes()
                .is_empty()
        );
        let mut bytes = [0_u8; 148];
        assert_eq!(
            sc_dds_prepare(bytes.as_mut_ptr(), bytes.len(), false, ptr::null_mut()),
            sc_status::SC_STATUS_INVALID_ARGUMENT
        );
        assert_eq!(
            sc_dds_prepare(bytes.as_mut_ptr(), bytes.len(), false, &mut info),
            sc_status::SC_STATUS_ERROR
        );
        assert_eq!(info.data_size, 0);
        let mut rgb = [42.0; 3];
        assert_eq!(
            sc_dds_mean_rgb(bytes.as_ptr(), bytes.len(), rgb.as_mut_ptr()),
            sc_status::SC_STATUS_ERROR
        );
        assert_eq!(rgb, [0.0; 3]);
        assert_eq!(
            sc_dds_mean_rgb(ptr::null(), 0, rgb.as_mut_ptr()),
            sc_status::SC_STATUS_INVALID_ARGUMENT
        );
        assert_eq!(
            sc_dds_mean_rgb(bytes.as_ptr(), bytes.len(), ptr::null_mut()),
            sc_status::SC_STATUS_INVALID_ARGUMENT
        );
    }
}
#[test]
fn dds_valid_calls_clear_error_and_update_header() {
    let mut bytes = vec![0_u8; 156];
    bytes[..4].copy_from_slice(b"DDS ");
    for (o, v) in [
        (4, 124_u32),
        (8, 0xA1007),
        (12, 4),
        (16, 4),
        (20, 8),
        (28, 1),
        (76, 32),
        (80, 4),
        (108, 0x1000),
        (128, 81),
        (132, 3),
        (140, 1),
    ] {
        bytes[o..o + 4].copy_from_slice(&v.to_le_bytes());
    }
    bytes[84..88].copy_from_slice(b"DX10");
    bytes[148] = 0x81;
    bytes[149] = 0x80;
    bytes[150..].fill(0xff);
    unsafe {
        let mut info = sc_dds_info::default();
        assert_eq!(
            sc_dds_prepare(bytes.as_mut_ptr(), bytes.len(), true, &mut info),
            sc_status::SC_STATUS_ERROR
        );
        assert_eq!(
            sc_dds_prepare(bytes.as_mut_ptr(), bytes.len(), false, &mut info),
            sc_status::SC_STATUS_OK
        );
        assert_eq!(
            (
                info.dxgi_format,
                info.width,
                info.height,
                info.mip_count,
                info.data_offset,
                info.data_size
            ),
            (80, 4, 4, 1, 148, 8)
        );
        assert_eq!(&bytes[128..132], &80_u32.to_le_bytes());
        assert!(
            CStr::from_ptr(sc_last_error(ptr::null_mut()))
                .to_bytes()
                .is_empty()
        );
        let mut rgb = [0.0; 3];
        assert_eq!(
            sc_dds_mean_rgb(bytes.as_ptr(), bytes.len(), rgb.as_mut_ptr()),
            sc_status::SC_STATUS_OK
        );
        assert_eq!(rgb, [1.0, 0.0, 0.0]);
    }
}
