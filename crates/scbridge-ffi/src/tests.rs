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
fn datacore_record_rejects_invalid_arguments_without_allocating() {
    unsafe {
        let mut json = ptr::null_mut();
        assert_eq!(
            sc_datacore_record(ptr::null_mut(), c"x".as_ptr(), &mut json),
            sc_status::SC_STATUS_INVALID_ARGUMENT
        );
        assert!(json.is_null());
        assert_eq!(
            sc_datacore_record(ptr::null_mut(), c"x".as_ptr(), ptr::null_mut()),
            sc_status::SC_STATUS_INVALID_ARGUMENT
        );
    }
}

#[test]
fn live_datacore_record_by_name_and_guid_and_missing_is_an_error() {
    let Ok(found) = starbreaker_common::discover::find_p4k() else {
        eprintln!("skip: no Data.p4k installed");
        return;
    };
    let path = CString::new(found.path.to_str().unwrap()).unwrap();
    unsafe {
        let mut h = ptr::null_mut();
        assert_eq!(sc_open(path.as_ptr(), ptr::null(), &mut h), sc_status::SC_STATUS_OK);
        let record = |query: &CStr| -> std::result::Result<serde_json::Value, String> {
            let mut json = ptr::null_mut();
            match sc_datacore_record(h, query.as_ptr(), &mut json) {
                sc_status::SC_STATUS_OK => {
                    let value = serde_json::from_str(CStr::from_ptr(json).to_str().unwrap()).unwrap();
                    sc_free_string(json);
                    Ok(value)
                }
                _ => {
                    assert!(json.is_null());
                    Err(CStr::from_ptr(sc_last_error(h)).to_string_lossy().into_owned())
                }
            }
        };
        // Any main entity record serves; its name comes from the archive, not this test.
        let mut search = ptr::null_mut();
        assert_eq!(sc_search_entities(h, c"".as_ptr(), 1, &mut search), sc_status::SC_STATUS_OK);
        let hits: serde_json::Value = serde_json::from_str(CStr::from_ptr(search).to_str().unwrap()).unwrap();
        sc_free_string(search);
        // Hits carry the bare entity name and the record GUID.
        let full_name = format!("EntityClassDefinition.{}", hits[0]["name"].as_str().unwrap());
        let by_name = record(&CString::new(full_name.as_str()).unwrap()).unwrap();
        assert_eq!(by_name["__type"], "EntityClassDefinition");
        assert_eq!(by_name["__record_name"], full_name.as_str());
        let guid = CString::new(hits[0]["guid"].as_str().unwrap()).unwrap();
        let by_guid = record(&guid).unwrap();
        assert_eq!(by_guid["__record_name"], by_name["__record_name"]);
        assert_eq!(by_guid["__record_id"].as_str(), guid.to_str().ok());
        let missing = record(c"Synthetic.NoSuchRecord_K1").unwrap_err();
        assert!(missing.contains("no DataCore record"), "{missing}");
        sc_close(h);
    }
}

#[test]
fn ffi_defaults_match_core_contract_and_options_validate() {
    let mut output = std::mem::MaybeUninit::<sc_options>::uninit();
    assert_eq!(
        unsafe { sc_default_options(output.as_mut_ptr()) },
        sc_status::SC_STATUS_OK
    );
    let o = unsafe { output.assume_init() };
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
    assert!(
        !sc_options { animations: 0, ..o }
            .core()
            .unwrap_or_else(|_| panic!("animations=0 rejected"))
            .geometry
            .include_animations
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

#[test]
fn cancellation_is_a_distinct_nonpanic_status() {
    let error: anyhow::Error = scviewer_core::Error::Cancelled.into();
    let failure = boundary::Failure::from(error.context("export phase"));
    assert_eq!(failure.0, sc_status::SC_STATUS_CANCELLED);
    assert!(failure.1.contains("cancelled"));
}

#[test]
fn archive_build_id_clears_output_on_error() {
    let mut value = ptr::dangling_mut();
    unsafe {
        assert_eq!(
            sc_archive_build_id(ptr::null_mut(), &mut value),
            sc_status::SC_STATUS_INVALID_ARGUMENT
        );
    }
    assert!(value.is_null());
}

#[test]
fn live_cache_stats_recovers_broken_manifest_and_handles_missing_manifest() {
    let Ok(found) = starbreaker_common::discover::find_p4k() else {
        eprintln!("skip: no Data.p4k installed");
        return;
    };
    let cache = tempfile::tempdir().unwrap();
    let path = CString::new(found.path.to_str().unwrap()).unwrap();
    let cache_path = CString::new(cache.path().to_str().unwrap()).unwrap();
    unsafe {
        let mut h = ptr::null_mut();
        assert_eq!(
            sc_open(path.as_ptr(), cache_path.as_ptr(), &mut h),
            sc_status::SC_STATUS_OK
        );
        let mut build_id = ptr::null_mut();
        assert_eq!(sc_archive_build_id(h, &mut build_id), sc_status::SC_STATUS_OK);
        assert_eq!(
            CStr::from_ptr(build_id).to_str().unwrap(),
            (*h).session.build_id()
        );
        sc_free_string(build_id);
        let mut stats = sc_cache_info::default();
        assert_eq!(sc_cache_stats(h, &mut stats), sc_status::SC_STATUS_OK);
        let bytes = stats.bytes;
        let hit = stats.last_hit;
        assert!(bytes > 0);
        std::fs::write(cache.path().join("manifest.json"), b"broken").unwrap();
        assert_eq!(sc_cache_stats(h, &mut stats), sc_status::SC_STATUS_OK);
        assert_eq!(stats.bytes, bytes);
        assert_eq!(stats.last_hit, hit);
        std::fs::remove_file(cache.path().join("manifest.json")).unwrap();
        assert_eq!(sc_cache_stats(h, &mut stats), sc_status::SC_STATUS_OK);
        assert_eq!((stats.bytes, stats.entries), (0, 0));
        sc_close(h);
    }
}

#[test]
fn archive_and_effect_calls_reject_invalid_arguments_without_allocating() {
    unsafe {
        let mut blob = sc_blob::default();
        assert_eq!(sc_p4k_read(ptr::null_mut(), c"Data/x.xml".as_ptr(), &mut blob), sc_status::SC_STATUS_INVALID_ARGUMENT);
        assert!(blob.data.is_null() && blob.len == 0);
        assert_eq!(sc_p4k_read(ptr::null_mut(), c"Data/x.xml".as_ptr(), ptr::null_mut()), sc_status::SC_STATUS_INVALID_ARGUMENT);
        assert_eq!(sc_p4k_read_dds(ptr::null_mut(), c"Data/x.dds".as_ptr(), 0, &mut blob), sc_status::SC_STATUS_INVALID_ARGUMENT);
        assert!(blob.data.is_null() && blob.len == 0);
        assert_eq!(sc_p4k_read_dds(ptr::null_mut(), c"Data/x.dds".as_ptr(), 0, ptr::null_mut()), sc_status::SC_STATUS_INVALID_ARGUMENT);
        type JsonCall = unsafe extern "C" fn(*mut sc_handle, *const c_char, *mut *mut c_char) -> sc_status;
        let calls: [JsonCall; 3] = [sc_p4k_read_xml, sc_particle_effect, sc_material_effect];
        for call in calls {
            let mut out = ptr::null_mut();
            assert_eq!(call(ptr::null_mut(), c"x".as_ptr(), &mut out), sc_status::SC_STATUS_INVALID_ARGUMENT);
            assert!(out.is_null());
            assert_eq!(call(ptr::null_mut(), c"x".as_ptr(), ptr::null_mut()), sc_status::SC_STATUS_INVALID_ARGUMENT);
        }
    }
}

#[test]
#[ignore = "needs the installed Data.p4k"]
fn live_archive_reads_and_effects() {
    let found = starbreaker_common::discover::find_p4k().expect("Data.p4k");
    let path = CString::new(found.path.to_str().unwrap()).unwrap();
    unsafe {
        let mut h = ptr::null_mut();
        assert_eq!(sc_open(path.as_ptr(), ptr::null(), &mut h), sc_status::SC_STATUS_OK);
        let library = c"data/libs/particles/gadgets/TRACTOR_BEAM.xml";
        let mut blob = sc_blob::default();
        assert_eq!(sc_p4k_read(h, library.as_ptr(), &mut blob), sc_status::SC_STATUS_OK);
        assert!(std::slice::from_raw_parts(blob.data, blob.len).starts_with(b"CryXmlB\0"));
        sc_free_blob(&mut blob);
        assert_eq!(sc_p4k_read_dds(h, c"data/textures/vfx/glows/GLOW4_ALPHA.dds".as_ptr(), 0, &mut blob), sc_status::SC_STATUS_OK);
        let mut info = std::mem::zeroed::<sc_dds_info>();
        assert_eq!(sc_dds_prepare(blob.data, blob.len, false, &mut info), sc_status::SC_STATUS_OK, "a package DDS");
        assert!(info.width > 0 && info.mip_count > 1);
        sc_free_blob(&mut blob);
        let text = |call: unsafe extern "C" fn(*mut sc_handle, *const c_char, *mut *mut c_char) -> sc_status, arg: &CStr| {
            let mut out = ptr::null_mut();
            let status = call(h, arg.as_ptr(), &mut out);
            if status != sc_status::SC_STATUS_OK {
                assert!(out.is_null());
                return Err(CStr::from_ptr(sc_last_error(h)).to_string_lossy().into_owned());
            }
            let value = CStr::from_ptr(out).to_str().unwrap().to_owned();
            sc_free_string(out);
            Ok(value)
        };
        let xml = text(sc_p4k_read_xml, library).unwrap();
        assert!(xml.starts_with("<ParticleLibrary"), "{}", &xml[..60]);
        let missing = text(sc_p4k_read_xml, c"Data/Libs/Particles/none.xml").unwrap_err();
        assert!(missing.contains("none.xml"), "{missing}");

        let effect: serde_json::Value = serde_json::from_str(
            &text(sc_particle_effect, c"2c211ca4-c915-b0c7-5d0e-58fe5c503a16|Gadgets/tractor_beam.fps.beam_only").unwrap(),
        )
        .unwrap();
        assert_eq!(effect["guid"], "2c211ca4-c915-b0c7-5d0e-58fe5c503a16");
        assert!(effect["emitters"].as_array().unwrap().iter().any(|e| e["path"] == "fps.beam_only.beam_core"));
        let material: serde_json::Value =
            serde_json::from_str(&text(sc_material_effect, c"sc_ships_surface_impacts:Metal_Small").unwrap()).unwrap();
        assert!(material["particles"][0]["reference"].as_str().unwrap().ends_with("Metal.small_impact_gpu"));
        assert!(text(sc_particle_effect, c"Gadgets/tractor_beam.no_such_effect").is_err());
        sc_close(h);
    }
}
