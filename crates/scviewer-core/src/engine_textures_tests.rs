//! Fixture parsing, family detection and (with game data) the copied detail texture.
use super::*;

fn sidecar(family: &str) -> ExportedFile {
    ExportedFile {
        relative_path: "Data/m.materials.json".into(),
        bytes: serde_json::json!({"submaterials": [{"shader_family": family}]}).to_string().into_bytes(),
        kind: ExportedFileKind::MaterialSidecar,
    }
}

#[test]
fn fixture_names_the_layerblend_detail_map() {
    let textures = fixture_textures().unwrap();
    let (_, path) = textures.iter().find(|(n, _)| n == "WearDirtFrostDetailMap").expect("fixture entry");
    assert!(path.ends_with(".dds") && !path.contains('\\'), "{path}");
}

#[test]
fn only_packages_with_the_family_need_it() {
    let with = DecomposedExport { files: vec![sidecar("HardSurface"), sidecar("LayerBlend_V2")] };
    let without = DecomposedExport { files: vec![sidecar("HardSurface")] };
    assert!(package_uses_family(&with, "LayerBlend_V2"));
    assert!(!package_uses_family(&without, "LayerBlend_V2"));
}

#[test]
fn the_detail_map_is_copied_from_the_installed_game() {
    let Ok(found) = starbreaker_common::discover::find_p4k() else {
        eprintln!("skip: no Data.p4k installed");
        return;
    };
    let p4k = MappedP4k::open(&found.path).unwrap();
    let mut package = DecomposedExport { files: vec![sidecar("LayerBlend_V2")] };
    add_to_package(&mut package, &p4k, 0).unwrap();
    add_to_package(&mut package, &p4k, 0).unwrap(); // idempotent
    let added: Vec<_> = package.files.iter().filter(|f| f.kind == ExportedFileKind::TextureAsset).collect();
    assert_eq!(added.len(), 1);
    assert_eq!(&added[0].bytes[..4], b"DDS ");
    let mut clean = DecomposedExport { files: vec![sidecar("HardSurface")] };
    add_to_package(&mut clean, &p4k, 0).unwrap();
    assert_eq!(clean.files.len(), 1, "no LayerBlend_V2 material, no copy");
}
