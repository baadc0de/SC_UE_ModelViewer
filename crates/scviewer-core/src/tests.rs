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
        .export_entity_decomposed_mem_cached("AEGS_Gladius", &options, None)
        .unwrap();
    assert_eq!(cold.cache_status, CacheStatus::Miss);
    assert!(reports.load(Ordering::Relaxed) > 1);
    let mut scene_archive = tar::Archive::new(cold.value.as_slice());
    let mut entries = scene_archive.entries().unwrap();
    let scene: serde_json::Value = serde_json::from_reader(entries.next().unwrap().unwrap()).unwrap();
    let children = scene["children"].as_array().unwrap();
    assert!(children.iter().all(|c| c.get("item_type").is_some()));
    assert!(children.iter().any(|c| c["item_type"] == "Seat"));
    for child in children {
        let _: ChildItemIdentity = serde_json::from_value(child.clone()).unwrap();
    }
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
        .export_entity_decomposed_mem_cached("AEGS_Gladius", &options, None)
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
                .export_entity_decomposed_mem(entity, &DecomposedOptions::default(), None)
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
            let lights = session.lights(entity).unwrap();
            let exported_lights: Vec<_> = scene["root_entity"]["lights"]
                .as_array()
                .unwrap()
                .iter()
                .chain(
                    scene["children"]
                        .as_array()
                        .unwrap()
                        .iter()
                        .flat_map(|child| child["lights"].as_array().unwrap().iter()),
                )
                .chain(
                    scene["interiors"]
                        .as_array()
                        .unwrap()
                        .iter()
                        .flat_map(|container| container["lights"].as_array().unwrap().iter()),
                )
                .collect();
            assert_eq!(
                exported_lights.len(),
                lights.lights.len(),
                "light catalogue/export parity for {entity}"
            );
            let roundtrip = |value: serde_json::Value| {
                serde_json::from_str::<serde_json::Value>(&value.to_string()).unwrap()
            };
            let mut identities = BTreeSet::new();
            for light in &lights.lights {
                let id = (light.source_container_index, light.source_light_index);
                assert!(identities.insert(id), "duplicate source light");
                let matches: Vec<_> = exported_lights
                    .iter()
                    .filter(|record| {
                        record["source_container_index"] == id.0
                            && record["source_light_index"] == id.1
                    })
                    .collect();
                assert_eq!(matches.len(), 1, "source light must occur exactly once");
                let record = matches[0];
                let authored = roundtrip(serde_json::to_value(&light.light).unwrap());
                for field in [
                    "bulb_radius_m",
                    "plane_width_m",
                    "plane_height_m",
                    "shadow_casting",
                    "max_shadow_cast_distance_m",
                    "affects_this_area_only",
                    "affect_gi",
                    "attenuation_tweak",
                    "plane_softness",
                    "fade_min_distance_m",
                    "fade_max_distance_m",
                    "importance",
                    "glow_multiplier",
                    "intensity_candela_proxy_model",
                    "importance_name",
                    "affect_gi_mode",
                    "active_state",
                    "intensity_raw",
                ] {
                    assert!(record.get(field).is_some(), "missing {field}");
                    assert_eq!(record[field], authored[field], "{field}");
                }
                if let Some(default) = light.light.states.get("defaultState") {
                    assert_eq!(record["active_state"], "defaultState");
                    assert_eq!(
                        record["intensity_raw"],
                        roundtrip(serde_json::json!(default.intensity_raw))
                    );
                }
                assert_eq!(
                    record["color"],
                    roundtrip(serde_json::json!(light.light.color))
                );
                assert_eq!(
                    record["states"],
                    roundtrip(serde_json::to_value(&light.light.states).unwrap())
                );
                assert_eq!(
                    record["radius_m"],
                    roundtrip(serde_json::json!(light.light.radius_m))
                );
                if light.parent_node_name.is_some() {
                    assert_eq!(
                        record["position"],
                        roundtrip(serde_json::json!(light.light.position))
                    );
                    assert_eq!(
                        record["parent_local_position"],
                        roundtrip(serde_json::json!(light.parent_local_position))
                    );
                }
            }
            eprintln!(
                "{entity}: {} lights; catalogue/export parity verified",
                lights.lights.len()
            );
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
    let lights = session.lights(&no_geometry).unwrap();
    assert!(lights.lights.is_empty());
    eprintln!("no-geometry entity {no_geometry}: 0 lights");
    assert!(session.lights("not_an_entity").is_err());
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

#[test]
fn live_cache_statistics_missing_and_recovered_manifest() {
    let Ok(found) = starbreaker_common::discover::find_p4k() else {
        eprintln!("skip: no Data.p4k installed");
        return;
    };
    let dir = tempfile::tempdir().unwrap();
    let session = Session::open(Some(&found.path), Some(dir.path())).unwrap();
    let initial = session.cache_stats().unwrap();
    assert!(initial.enabled && initial.bytes > 0 && initial.entries > 0);
    fs::write(dir.path().join("manifest.json"), b"invalid").unwrap();
    let recovered = session.cache_stats().unwrap();
    assert_eq!(
        (initial.bytes, initial.entries),
        (recovered.bytes, recovered.entries)
    );
    assert_eq!(initial.last_status, session.cache_status());
    fs::remove_file(dir.path().join("manifest.json")).unwrap();
    let missing = session.cache_stats().unwrap();
    assert_eq!((missing.bytes, missing.entries), (0, 0));
    session.cache_evict(0).unwrap();
    assert_eq!(session.cache_stats().unwrap().bytes, 0);
}

#[test]
fn live_independent_decomposed_exports_are_byte_identical() {
    use std::{collections::BTreeMap, path::PathBuf};
    let Ok(found) = starbreaker_common::discover::find_p4k() else {
        eprintln!("skip: no Data.p4k installed");
        return;
    };
    fn files(root: &Path, dir: &Path, result: &mut BTreeMap<PathBuf, Vec<u8>>) {
        for entry in fs::read_dir(dir).unwrap() {
            let path = entry.unwrap().path();
            if path.is_dir() {
                files(root, &path, result);
            } else {
                result.insert(
                    path.strip_prefix(root).unwrap().into(),
                    fs::read(&path).unwrap(),
                );
            }
        }
    }
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../out");
    fs::create_dir_all(&root).unwrap();
    let first = tempfile::tempdir_in(&root).unwrap();
    let second = tempfile::tempdir_in(&root).unwrap();
    for dir in [&first, &second] {
        // A new session and disabled persistence ensure independent assembly.
        let session = Session::open(Some(&found.path), None).unwrap();
        session
            .export_entity_decomposed(
                "AEGS_Gladius",
                dir.path(),
                &DecomposedOptions::default(),
                None,
            )
            .unwrap();
        assert_eq!(session.cache_status(), CacheStatus::Miss);
    }
    let mut a = BTreeMap::new();
    let mut b = BTreeMap::new();
    files(first.path(), first.path(), &mut a);
    files(second.path(), second.path(), &mut b);
    assert_eq!(a.keys().collect::<Vec<_>>(), b.keys().collect::<Vec<_>>());
    for (path, bytes) in &a {
        assert!(bytes == &b[path], "different bytes: {}", path.display());
    }
    eprintln!(
        "two independent uncached exports: {} files byte-identical",
        a.len()
    );
}
