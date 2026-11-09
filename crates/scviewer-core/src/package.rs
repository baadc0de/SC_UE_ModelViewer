//! Deterministic uncompressed tar serialization of upstream exported files.
use anyhow::{Result, ensure};
use starbreaker_3d::DecomposedExport;
use std::path::{Component, Path};

pub(crate) fn to_tar(package: &DecomposedExport) -> Result<Vec<u8>> {
    let mut files: Vec<_> = package.files.iter().collect();
    files.sort_by_key(|f| (!f.relative_path.ends_with("/scene.json"), &f.relative_path));
    ensure!(
        files
            .first()
            .is_some_and(|f| f.relative_path.ends_with("/scene.json")),
        "package has no scene.json"
    );
    let mut tar = tar::Builder::new(Vec::new());
    for file in files {
        ensure!(
            Path::new(&file.relative_path)
                .components()
                .all(|c| matches!(c, Component::Normal(_))),
            "unsafe package path: {}",
            file.relative_path
        );
        let mut header = tar::Header::new_gnu();
        header.set_size(file.bytes.len() as u64);
        header.set_mode(0o644);
        header.set_mtime(0);
        header.set_cksum();
        tar.append_data(&mut header, &file.relative_path, file.bytes.as_slice())?;
    }
    Ok(tar.into_inner()?)
}
pub(crate) fn unpack(bytes: &[u8], root: &Path) -> Result<Vec<std::path::PathBuf>> {
    std::fs::create_dir_all(root)?;
    let mut archive = tar::Archive::new(bytes);
    let mut paths = Vec::new();
    for entry in archive.entries()? {
        let mut entry = entry?;
        ensure!(
            entry.header().entry_type().is_file(),
            "package contains a non-file entry"
        );
        let relative = entry.path()?.into_owned();
        ensure!(
            relative
                .components()
                .all(|c| matches!(c, Component::Normal(_))),
            "unsafe package entry"
        );
        ensure!(entry.unpack_in(root)?, "package entry escaped output root");
        paths.push(root.join(relative));
    }
    Ok(paths)
}
#[cfg(test)]
mod tests {
    use super::*;
    use starbreaker_3d::{ExportedFile, ExportedFileKind};
    #[test]
    fn scene_first_deterministic_and_byte_identical() {
        let p = DecomposedExport {
            files: vec![
                ExportedFile {
                    relative_path: "Data/a.glb".into(),
                    bytes: vec![0, 1, 2],
                    kind: ExportedFileKind::MeshAsset,
                },
                ExportedFile {
                    relative_path: "Packages/a/scene.json".into(),
                    bytes: b"{}".to_vec(),
                    kind: ExportedFileKind::PackageManifest,
                },
            ],
        };
        let bytes = to_tar(&p).unwrap();
        assert_eq!(bytes, to_tar(&p).unwrap());
        let mut archive = tar::Archive::new(bytes.as_slice());
        assert_eq!(
            archive
                .entries()
                .unwrap()
                .next()
                .unwrap()
                .unwrap()
                .path()
                .unwrap(),
            Path::new("Packages/a/scene.json")
        );
        let dir = tempfile::tempdir().unwrap();
        unpack(&bytes, dir.path()).unwrap();
        for f in p.files {
            assert_eq!(
                std::fs::read(dir.path().join(f.relative_path)).unwrap(),
                f.bytes
            );
        }
    }
}
