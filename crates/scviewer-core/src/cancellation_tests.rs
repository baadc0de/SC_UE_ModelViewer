//! Cancellation and animation-switch regressions, with optional live archive checks.
use super::*;
use std::sync::atomic::{AtomicUsize, Ordering};

fn scratch(name: &str) -> tempfile::TempDir {
    let root = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../..")
        .join(name);
    fs::create_dir_all(&root).unwrap();
    tempfile::tempdir_in(root).unwrap()
}

#[test]
fn cancellation_token_is_shared_across_threads() {
    let token = CancelToken::default();
    let worker = token.clone();
    std::thread::spawn(move || worker.cancel()).join().unwrap();
    assert!(token.is_cancelled());
    assert!(matches!(
        token.checkpoint().unwrap_err().downcast_ref::<Error>(),
        Some(Error::Cancelled)
    ));
}

#[test]
fn live_cancelled_exports_publish_nothing_and_session_remains_usable() {
    let Ok(found) = starbreaker_common::discover::find_p4k() else {
        eprintln!("skip cancellation: no archive");
        return;
    };
    let cache = scratch("cache");
    let out = scratch("out");
    fs::write(out.path().join("existing.txt"), b"keep").unwrap();
    let mut session = Session::open(Some(&found.path), Some(cache.path())).unwrap();
    let before = session.cache_stats().unwrap();
    for phase in [
        "Preparing export",
        "Resolving loadout",
        "Writing root assets",
        "Building tar",
        "Publishing package",
    ] {
        let token = CancelToken::default();
        let control = token.clone();
        let calls = Arc::new(AtomicUsize::new(0));
        let count = calls.clone();
        session.set_progress_callback(Some(Arc::new(move |_, stage| {
            count.fetch_add(1, Ordering::Relaxed);
            if stage == phase {
                control.cancel();
            }
        })));
        let error = session
            .export_entity_decomposed(
                "AEGS_Gladius",
                out.path(),
                &DecomposedOptions::default(),
                Some(&token),
            )
            .unwrap_err();
        assert!(
            matches!(error.downcast_ref::<Error>(), Some(Error::Cancelled)),
            "{phase}: {error}"
        );
        assert!(calls.load(Ordering::Relaxed) > 0);
        let after = session.cache_stats().unwrap();
        assert_eq!(
            (before.entries, before.bytes),
            (after.entries, after.bytes),
            "{phase}"
        );
        assert_eq!(fs::read_dir(out.path()).unwrap().count(), 1);
        assert_eq!(fs::read(out.path().join("existing.txt")).unwrap(), b"keep");
        eprintln!("cancelled at {phase}: no new cache entry or output files");
    }
    session.set_progress_callback(None);
    let token = CancelToken::default();
    token.cancel();
    let error = session
        .export_entity_glb("AEGS_Gladius", &GlbOptions::default(), Some(&token))
        .unwrap_err();
    assert!(matches!(
        error.downcast_ref::<Error>(),
        Some(Error::Cancelled)
    ));
    assert!(!session.search_entities("gladius", 1).is_empty());
}

#[test]
fn live_animation_switch_changes_package_and_cache_key() {
    let Ok(found) = starbreaker_common::discover::find_p4k() else {
        eprintln!("skip animations: no archive");
        return;
    };
    let cache = scratch("cache");
    let session = Session::open(Some(&found.path), Some(cache.path())).unwrap();
    let enabled = DecomposedOptions::default();
    let mut disabled = enabled.clone();
    disabled.geometry.include_animations = false;
    let on = session
        .export_entity_decomposed_mem("AEGS_Gladius", &enabled, None)
        .unwrap();
    let before = session.cache_stats().unwrap();
    let off = session
        .export_entity_decomposed_mem("AEGS_Gladius", &disabled, None)
        .unwrap();
    assert_eq!(session.cache_status(), CacheStatus::Miss);
    assert_eq!(session.cache_stats().unwrap().entries, before.entries + 1);
    let inspect = |bytes: &[u8]| {
        use std::io::Read;
        let mut archive = tar::Archive::new(bytes);
        let mut clips = 0;
        let mut sidecars = 0;
        for entry in archive.entries().unwrap() {
            let mut entry = entry.unwrap();
            let path = entry.path().unwrap().to_string_lossy().into_owned();
            if path.contains("/animations/") {
                sidecars += 1;
            }
            if path.ends_with("/scene.json") {
                let mut data = String::new();
                entry.read_to_string(&mut data).unwrap();
                let scene: serde_json::Value = serde_json::from_str(&data).unwrap();
                clips = scene["root_entity"]["animations"]
                    .as_array()
                    .map_or(0, Vec::len);
            }
        }
        (clips, sidecars)
    };
    let (clips, sidecars) = inspect(&on);
    assert!(clips > 0 && sidecars > 0);
    assert_eq!(inspect(&off), (0, 0));
    assert_eq!(
        session
            .export_entity_decomposed_mem("AEGS_Gladius", &disabled, None)
            .unwrap(),
        off
    );
    assert_eq!(session.cache_status(), CacheStatus::Hit);
    let token = CancelToken::default();
    token.cancel();
    assert!(matches!(
        session
            .export_entity_decomposed_mem("AEGS_Gladius", &disabled, Some(&token))
            .unwrap_err()
            .downcast_ref::<Error>(),
        Some(Error::Cancelled)
    ));
    eprintln!("animations on/off: distinct cache entries; disabled package has no clips/sidecars");
}
