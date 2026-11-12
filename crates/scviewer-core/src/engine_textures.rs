//! Engine textures the game's shaders bind by default but no material authors (WP-U2b). The paths are the
//! `shader_textures` of the extracted render fixture (`ue/Plugins/SCBridge/Resources/StarEngineRender.json`, re-checked
//! against the installed game by `scripts/verify_star_engine_render.py`); [`add_to_package`] copies each one from
//! Data.p4k into a decomposed package that has a material of the shader family sampling it, at the package's texture
//! mip, under its P4k path. The viewer's material builder reads the same fixture for the path.
use anyhow::{Context, Result};
use starbreaker_3d::{DecomposedExport, ExportedFile, ExportedFileKind};
use starbreaker_dds::{DdsFile, ReadSibling};
use starbreaker_p4k::MappedP4k;

const FIXTURE: &str = include_str!("../../../ue/Plugins/SCBridge/Resources/StarEngineRender.json");
/// Fixture texture -> the shader family whose materials sample it (LayerBlend_V2 `GetAlphaBlendDetailMaps`).
const USERS: &[(&str, &str)] = &[("WearDirtFrostDetailMap", "LayerBlend_V2")];

/// (fixture name, package-relative path with forward slashes) of every fixture texture.
pub(crate) fn fixture_textures() -> Result<Vec<(String, String)>> {
    let fixture: serde_json::Value = serde_json::from_str(FIXTURE).context("render fixture is not JSON")?;
    let section = fixture["shader_textures"].as_object().context("render fixture has no shader_textures")?;
    section
        .iter()
        .map(|(name, entry)| {
            let path = entry["value"].as_str().with_context(|| format!("shader_textures.{name} has no value"))?;
            Ok((name.clone(), path.replace('\\', "/")))
        })
        .collect()
}

/// Whether any material sidecar of the package has a submaterial of `family`.
pub(crate) fn package_uses_family(package: &DecomposedExport, family: &str) -> bool {
    package.files.iter().filter(|f| f.kind == ExportedFileKind::MaterialSidecar).any(|f| {
        serde_json::from_slice::<serde_json::Value>(&f.bytes)
            .ok()
            .and_then(|v| v["submaterials"].as_array().cloned())
            .is_some_and(|subs| subs.iter().any(|s| s["shader_family"].as_str() == Some(family)))
    })
}

struct Siblings<'a> {
    p4k: &'a MappedP4k,
    base: String,
}
impl ReadSibling for Siblings<'_> {
    fn read_sibling(&self, suffix: &str) -> Option<Vec<u8>> {
        let path = format!("{}{suffix}", self.base);
        self.p4k.entry_case_insensitive(&path).and_then(|e| self.p4k.read(e).ok())
    }
}

/// Package DDS bytes of a P4k texture (split mips merged), from `mip` down.
pub(crate) fn read_package_dds(p4k: &MappedP4k, path: &str, mip: u32) -> Result<Vec<u8>> {
    let p4k_path = path.replace('/', "\\");
    let entry = p4k.entry_case_insensitive(&p4k_path).with_context(|| format!("{path} is not in Data.p4k"))?;
    let bytes = p4k.read(entry).with_context(|| format!("reading {path}"))?;
    let dds = DdsFile::from_split(&bytes, &Siblings { p4k, base: p4k_path }).with_context(|| format!("{path} is not a DDS"))?;
    starbreaker_dds::package::write_package_dds(&dds, mip, false).with_context(|| format!("{path}: unsupported DDS layout"))
}

/// Add the fixture textures a package's materials sample (once; an existing file is kept).
pub(crate) fn add_to_package(package: &mut DecomposedExport, p4k: &MappedP4k, mip: u32) -> Result<()> {
    for (name, path) in fixture_textures()? {
        let Some((_, family)) = USERS.iter().find(|(n, _)| *n == name) else { continue };
        if !package_uses_family(package, family) || package.files.iter().any(|f| f.relative_path.eq_ignore_ascii_case(&path)) {
            continue;
        }
        let bytes = read_package_dds(p4k, &path, mip)?;
        package.files.push(ExportedFile { relative_path: path, bytes, kind: ExportedFileKind::TextureAsset });
    }
    Ok(())
}

#[cfg(test)]
#[path = "engine_textures_tests.rs"]
mod tests;
