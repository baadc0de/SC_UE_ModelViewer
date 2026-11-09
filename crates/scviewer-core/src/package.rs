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
        ensure!(!file.relative_path.contains('\0'), "NUL in package path");
        let mut header = tar::Header::new_ustar();
        if header.set_path(&file.relative_path).is_err() {
            // Only paths that cannot use ustar name/prefix splitting need PAX.
            // append() below writes the prepared header verbatim, never GNU 'L'.
            tar.append_pax_extensions([("path", file.relative_path.as_bytes())])?;
            header = tar::Header::new_ustar();
            header.set_path("PaxPayload")?;
        }
        header.set_entry_type(tar::EntryType::Regular);
        header.set_size(file.bytes.len() as u64);
        header.set_mode(0o644);
        header.set_mtime(0);
        header.set_cksum();
        tar.append(&header, file.bytes.as_slice())?;
    }
    Ok(tar.into_inner()?)
}
/// Reject truncated or structurally invalid packages even after manifest recovery.
pub(crate) fn validate_tar(bytes: &[u8]) -> Result<()> {
    ensure!(
        bytes.len() >= 1024 && bytes.len() % 512 == 0,
        "truncated tar"
    );
    ensure!(
        bytes[bytes.len() - 1024..].iter().all(|b| *b == 0),
        "missing tar terminator"
    );
    let mut archive = tar::Archive::new(bytes);
    let mut count = 0;
    for entry in archive.entries()? {
        let entry = entry?;
        ensure!(
            entry.header().entry_type() == tar::EntryType::Regular,
            "non-regular package entry"
        );
        let path = entry.path()?;
        ensure!(
            path.components().all(|c| matches!(c, Component::Normal(_))),
            "unsafe package path"
        );
        if count == 0 {
            ensure!(
                path.file_name().is_some_and(|n| n == "scene.json"),
                "scene.json must be first"
            );
        }
        ensure!(
            entry
                .raw_file_position()
                .checked_add(entry.size())
                .is_some_and(|end| end <= bytes.len() as u64 - 1024),
            "truncated tar entry"
        );
        count += 1;
    }
    ensure!(count > 0, "empty package");
    Ok(())
}
pub(crate) fn unpack(bytes: &[u8], root: &Path) -> Result<Vec<std::path::PathBuf>> {
    std::fs::create_dir_all(root)?;
    let mut archive = tar::Archive::new(bytes);
    archive.set_preserve_mtime(false);
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
pub(crate) mod tests {
    use super::*;
    use starbreaker_3d::{ExportedFile, ExportedFileKind};
    /// Deliberately minimal reader: only POSIX ustar name/prefix, size and type.
    pub(crate) fn minimal_ustar_paths(bytes: &[u8]) -> Vec<String> {
        let mut paths = Vec::new();
        let mut offset = 0;
        let text = |field: &[u8]| {
            String::from_utf8(field.iter().copied().take_while(|b| *b != 0).collect()).unwrap()
        };
        while bytes[offset..offset + 512].iter().any(|b| *b != 0) {
            let header = &bytes[offset..offset + 512];
            assert_eq!(
                header[156], b'0',
                "every Gladius entry must be explicitly Regular"
            );
            assert_eq!(&header[257..263], b"ustar\0");
            let name = text(&header[..100]);
            let prefix = text(&header[345..500]);
            paths.push(if prefix.is_empty() {
                name
            } else {
                format!("{prefix}/{name}")
            });
            let size = usize::from_str_radix(text(&header[124..136]).trim(), 8).unwrap();
            offset += 512 + size.div_ceil(512) * 512;
        }
        paths
    }
    #[test]
    fn long_split_paths_use_plain_ustar_entries() {
        let path = format!("Data/{}/{}.glb", "directory".repeat(8), "mesh".repeat(15));
        let p = DecomposedExport {
            files: vec![
                ExportedFile {
                    relative_path: "Packages/a/scene.json".into(),
                    bytes: b"{}".to_vec(),
                    kind: ExportedFileKind::PackageManifest,
                },
                ExportedFile {
                    relative_path: path.clone(),
                    bytes: vec![1],
                    kind: ExportedFileKind::MeshAsset,
                },
            ],
        };
        let bytes = to_tar(&p).unwrap();
        assert_eq!(
            minimal_ustar_paths(&bytes),
            vec!["Packages/a/scene.json".to_owned(), path]
        );
    }
    #[test]
    fn unsplittable_names_use_pax_path_without_gnu_longlink() {
        let path = format!("Data/{}.glb", "a".repeat(110));
        let p = DecomposedExport {
            files: vec![
                ExportedFile {
                    relative_path: "Packages/a/scene.json".into(),
                    bytes: b"{}".to_vec(),
                    kind: ExportedFileKind::PackageManifest,
                },
                ExportedFile {
                    relative_path: path.clone(),
                    bytes: vec![1],
                    kind: ExportedFileKind::MeshAsset,
                },
            ],
        };
        let bytes = to_tar(&p).unwrap();
        validate_tar(&bytes).unwrap();
        let mut raw = tar::Archive::new(bytes.as_slice());
        let types: Vec<_> = raw
            .entries()
            .unwrap()
            .raw(true)
            .map(|e| e.unwrap().header().entry_type().as_byte())
            .collect();
        assert_eq!(types, vec![b'0', b'x', b'0']);
        let dir = tempfile::tempdir().unwrap();
        unpack(&bytes, dir.path()).unwrap();
        assert_eq!(std::fs::read(dir.path().join(path)).unwrap(), vec![1]);
    }
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
