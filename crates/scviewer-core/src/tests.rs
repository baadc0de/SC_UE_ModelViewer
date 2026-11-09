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

#[test]
fn live_clip_catalogue_matches_scene_and_persists() {
    use std::{collections::BTreeSet, io::Read, time::Instant};
    let Ok(found) = starbreaker_common::discover::find_p4k() else {
        eprintln!("skip: no Data.p4k installed");
        return;
    };
    let cache_root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../cache");
    fs::create_dir_all(&cache_root).unwrap();
    let cache_dir = tempfile::tempdir_in(cache_root).unwrap();
    let session = Session::open(Some(&found.path), Some(cache_dir.path())).unwrap();
    for entity in ["AEGS_Gladius", "RSI_Scorpius", "DRAK_Cutlass_Black"] {
        let start = Instant::now();
        let catalogue = session.clip_catalogue_cached(entity).unwrap();
        eprintln!(
            "{entity}: {} clips, cold catalogue {:?}",
            catalogue.value.clips.len(),
            start.elapsed()
        );
        assert_eq!(catalogue.cache_status, CacheStatus::Miss);
        assert!(catalogue.value.clips.len() >= 10);
        assert!(
            catalogue
                .value
                .clips
                .iter()
                .all(|clip| !clip.id.contains(['*', '?']))
        );
        assert!(
            catalogue
                .value
                .clips
                .iter()
                .filter(|clip| clip.bone_count > 0)
                .all(|clip| !clip.contributors.is_empty())
        );
        if entity == "RSI_Scorpius" {
            assert!(catalogue.value.clips.iter().any(|c| {
                c.fragments
                    .iter()
                    .any(|f| f.fragment.to_lowercase().contains("landing"))
            }));
        }
        let start = Instant::now();
        let warm = session.clip_catalogue_cached(entity).unwrap();
        eprintln!("{entity}: catalogue cache hit {:?}", start.elapsed());
        assert_eq!(warm.cache_status, CacheStatus::Hit);
        assert_eq!(warm.value, catalogue.value);
        if entity == "AEGS_Gladius" {
            let uncached = Session::open(Some(&found.path), None).unwrap();
            let start = Instant::now();
            let fresh = uncached.clip_catalogue(entity).unwrap();
            let elapsed = start.elapsed();
            eprintln!("Gladius warm session, uncached catalogue: {elapsed:?}");
            if std::env::var("SCVIEWER_PERF_TESTS").as_deref() == Ok("1") {
                assert!(elapsed < std::time::Duration::from_millis(500));
            }
            assert_eq!(fresh, catalogue.value);
        }
        {
            let fresh = &catalogue.value;
            let bytes = session
                .export_entity_decomposed_mem(entity, &DecomposedOptions::default())
                .unwrap();
            let mut archive = tar::Archive::new(bytes.as_slice());
            let mut entries = archive.entries().unwrap();
            let mut scene = String::new();
            entries
                .next()
                .unwrap()
                .unwrap()
                .read_to_string(&mut scene)
                .unwrap();
            let scene: serde_json::Value = serde_json::from_str(&scene).unwrap();
            let exported = scene["root_entity"]["animations"].as_array().unwrap();
            let names: BTreeSet<_> = exported
                .iter()
                .map(|c| c["name"].as_str().unwrap())
                .collect();
            let ids: BTreeSet<_> = fresh.clips.iter().map(|c| c.id.as_str()).collect();
            assert_eq!(
                ids, names,
                "catalogue and scene index must join in both directions"
            );
            for clip in &fresh.clips {
                let index = exported.iter().find(|e| e["name"] == clip.id).unwrap();
                if let Some(label) = index["display_name"].as_str() {
                    assert_eq!(label, clip.display_name);
                }
            }
        }
    }
    let no_geometry = session.loaded().unwrap().index.with_dependent(|_, idx| {
        use starbreaker_datacore::{QueryResultExt, query::value::Value};
        let query = idx
            .db
            .compile_rooted::<Value>("EntityClassDefinition.Components[SGeometryResourceParams]")
            .unwrap();
        idx.db
            .records_by_type_name("EntityClassDefinition")
            .filter(|record| idx.db.is_main_record(record))
            .find(|record| {
                match idx
                    .db
                    .query_single::<Value>(&query, record)
                    .optional()
                    .unwrap()
                    .flatten()
                {
                    None | Some(Value::Null) => true,
                    Some(Value::Array(components)) => components.is_empty(),
                    Some(_) => false,
                }
            })
            .map(|record| {
                idx.db
                    .resolve_string2(record.name_offset)
                    .rsplit('.')
                    .next()
                    .unwrap()
                    .to_owned()
            })
            .expect("archive must contain an entity without a geometry component")
    });
    let empty = session.clip_catalogue(&no_geometry).unwrap();
    eprintln!(
        "no-geometry entity {no_geometry}: {} clips",
        empty.clips.len()
    );
    assert!(empty.clips.is_empty() && empty.skeletons.is_empty());
    assert!(session.clip_catalogue("not_an_entity").is_err());
    drop(session);
    let session = Session::open(Some(&found.path), Some(cache_dir.path())).unwrap();
    let start = Instant::now();
    assert_eq!(
        session
            .clip_catalogue_cached("aegs_gladius")
            .unwrap()
            .cache_status,
        CacheStatus::Hit
    );
    eprintln!("reopened session catalogue hit: {:?}", start.elapsed());
    assert!(
        session.loaded.get().is_none(),
        "catalogue hit must not open archive/DataCore"
    );
}
