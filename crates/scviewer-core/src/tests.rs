//! Session regression tests, with live archive checks gated on discovery.

use super::*;
#[test]
fn live_package_cache_round_trip_and_progress() {
    use std::sync::atomic::{AtomicUsize, Ordering};
    let Ok(found) = starbreaker_common::discover::find_p4k() else {
        eprintln!("skip: no Data.p4k installed");
        return;
    };
    let dir = tempfile::tempdir().unwrap();
    let mut session = Session::open(Some(&found.path), Some(dir.path())).unwrap();
    let reports = Arc::new(AtomicUsize::new(0));
    let counter = reports.clone();
    session.set_progress_callback(Some(Arc::new(move |fraction, _| {
        assert!((0.0..=1.0).contains(&fraction));
        counter.fetch_add(1, Ordering::Relaxed);
    })));
    let options = DecomposedOptions::default();
    let cold = session
        .export_entity_decomposed_mem_cached("AEGS_Gladius", &options)
        .unwrap();
    assert_eq!(cold.cache_status, CacheStatus::Miss);
    assert!(reports.load(Ordering::Relaxed) > 1);
    let names = package::tests::minimal_ustar_paths(&cold.value);
    let mut archive = tar::Archive::new(cold.value.as_slice());
    let standard: Vec<_> = archive
        .entries()
        .unwrap()
        .map(|e| e.unwrap().path().unwrap().to_string_lossy().into_owned())
        .collect();
    assert_eq!(names, standard);
    drop(session);
    let session = Session::open(Some(&found.path), Some(dir.path())).unwrap();
    let warm = session
        .export_entity_decomposed_mem_cached("AEGS_Gladius", &options)
        .unwrap();
    assert_eq!(warm.cache_status, CacheStatus::Hit);
    assert_eq!(cold.value, warm.value);
    assert!(
        session.loaded.get().is_none(),
        "package hit must avoid archive processing"
    );
    assert!(session.cache_evict(0).unwrap() >= cold.value.len() as u64);
}
#[test]
fn live_corrupt_index_is_rebuilt_as_a_miss() {
    let Ok(found) = starbreaker_common::discover::find_p4k() else {
        return;
    };
    let dir = tempfile::tempdir().unwrap();
    drop(Session::open(Some(&found.path), Some(dir.path())).unwrap());
    let filename = fs::read_dir(dir.path())
        .unwrap()
        .map(|e| e.unwrap().file_name().to_string_lossy().into_owned())
        .find(|n| n.starts_with("entities.") && n.ends_with(".bin"))
        .unwrap();
    assert!(filename.starts_with(&format!("entities.v{CACHE_FORMAT_VERSION}.")));
    cache::Cache::new(dir.path())
        .unwrap()
        .write(&filename, b"invalid bincode")
        .unwrap();
    let session = Session::open(Some(&found.path), Some(dir.path())).unwrap();
    assert_eq!(session.cache_status(), CacheStatus::Miss);
    assert!(!session.search_entities("", 1).is_empty());
}
#[test]
fn build_identity_uses_manifest_and_falls_back_to_archive_metadata() {
    let dir = tempfile::tempdir().unwrap();
    let p4k = dir.path().join("Data.p4k");
    fs::write(&p4k, b"synthetic archive").unwrap();
    let (first, branch) = build_identity(&p4k).unwrap();
    assert!(branch.is_none());
    fs::write(&p4k, b"different synthetic archive size").unwrap();
    assert_ne!(first, build_identity(&p4k).unwrap().0);
    fs::write(
        dir.path().join("build_manifest.id"),
        br#"{"Data":{"BuildId":"synthetic-build","Branch":"test"}}"#,
    )
    .unwrap();
    assert_eq!(
        build_identity(&p4k).unwrap(),
        ("synthetic-build".into(), Some("test".into()))
    );
}
#[test]
fn live_session_reuses_index_and_resolves_geometry_companions() {
    let Ok(found) = starbreaker_common::discover::find_p4k() else {
        eprintln!("skip: no Data.p4k installed");
        return;
    };
    let dir = tempfile::tempdir().unwrap();
    let first = Session::open(Some(&found.path), Some(dir.path())).unwrap();
    assert_eq!(first.cache_status(), CacheStatus::Miss);
    let hit = first.search_entities("gLaDiUs", 1).remove(0);
    assert!(first.resolve_name("Gladius").is_err());
    assert_eq!(first.resolve_name("aegs_gladius").unwrap(), "AEGS_Gladius");
    assert_eq!(hit.name, "AEGS_Gladius");
    assert!(hit.geometry_path.is_some());
    let paths = first.search_paths("**/gladius/*.cga", 1);
    assert!(!paths.is_empty());
    let bytes = first.geometry_file_glb(&paths[0].path).unwrap();
    assert!(bytes.starts_with(b"glTF"));
    let parent = paths[0].path.rsplit_once('/').unwrap().0;
    assert!(
        first
            .list_dir(parent)
            .iter()
            .any(|e| e.path == paths[0].path)
    );
    drop(first);
    let second = Session::open(Some(&found.path), Some(dir.path())).unwrap();
    assert_eq!(second.cache_status(), CacheStatus::Hit);
    assert!(
        second.loaded.get().is_none(),
        "index hit must avoid archive and DataCore processing"
    );
    assert_eq!(second.search_entities("gladius", 1)[0].guid, hit.guid);
    assert!(second.search_entities("", 0).is_empty());
}
