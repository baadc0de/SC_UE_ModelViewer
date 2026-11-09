//! ABI regression tests: invalid input, ownership, option validation and panic containment.
use super::*;
use std::ffi::CStr;

#[test]
fn invalid_inputs_and_open_errors_are_reported_without_allocations() {
    unsafe {
        assert_eq!(
            sc_open(ptr::null(), ptr::null(), ptr::null_mut()),
            sc_status::SC_STATUS_INVALID_ARGUMENT
        );
        assert!(
            CStr::from_ptr(sc_last_error(ptr::null_mut()))
                .to_str()
                .unwrap()
                .contains("output")
        );
        let mut h = ptr::null_mut();
        assert_eq!(
            sc_open(c"/missing/synthetic/Data.p4k".as_ptr(), ptr::null(), &mut h),
            sc_status::SC_STATUS_ERROR
        );
        assert!(h.is_null());
        assert!(
            CStr::from_ptr(sc_last_error(h))
                .to_str()
                .unwrap()
                .contains("opening")
        );
        let invalid = [0xffu8, 0];
        assert_eq!(
            sc_open(invalid.as_ptr().cast(), ptr::null(), &mut h),
            sc_status::SC_STATUS_INVALID_ARGUMENT
        );
        let mut json = ptr::null_mut();
        assert_eq!(
            sc_search_entities(h, c"x".as_ptr(), 1, &mut json),
            sc_status::SC_STATUS_INVALID_ARGUMENT
        );
        assert!(json.is_null());
        sc_free_string(ptr::null_mut());
        sc_free_blob(ptr::null_mut());
        sc_close(ptr::null_mut());
        let mut blob = sc_blob::default();
        sc_free_blob(&mut blob);
        sc_free_blob(&mut blob);
        assert!(blob.data.is_null() && blob.len == 0);
    }
}
#[test]
fn options_reject_invalid_and_unsupported_values() {
    let o = sc_options::default();
    assert_eq!(
        o.core().unwrap_or_else(|_| panic!("defaults rejected")),
        scviewer_core::DecomposedOptions::default()
    );
    let status = |o: sc_options| o.core().err().unwrap().0;
    assert_eq!(
        status(sc_options {
            material_mode: 99,
            ..o
        }),
        sc_status::SC_STATUS_INVALID_ARGUMENT
    );
    assert_eq!(
        status(sc_options { lights: 2, ..o }),
        sc_status::SC_STATUS_INVALID_ARGUMENT
    );
    assert_eq!(
        status(sc_options { animations: 0, ..o }),
        sc_status::SC_STATUS_UNSUPPORTED
    );
    assert_eq!(
        status(sc_options {
            collision_reserved: 1,
            ..o
        }),
        sc_status::SC_STATUS_UNSUPPORTED
    );
}
#[test]
fn forced_panic_returns_status_and_message() {
    unsafe {
        assert_eq!(sc_test_panic(ptr::null_mut()), sc_status::SC_STATUS_PANIC);
        let error = CStr::from_ptr(sc_last_error(ptr::null_mut()))
            .to_str()
            .unwrap();
        assert_eq!(error, "forced WP-R4 panic");
        println!("panic test: SC_STATUS_PANIC (3), message={error}; process survived");
    }
}
