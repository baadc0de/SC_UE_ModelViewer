//! Versioned local cache, atomic publication, manifest accounting and LRU eviction.
use anyhow::{Context, Result};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
    collections::BTreeMap,
    fs,
    path::{Path, PathBuf},
    time::{SystemTime, UNIX_EPOCH},
};

pub const CACHE_FORMAT_VERSION: u32 = 2;
pub const DEFAULT_CACHE_CAP_BYTES: u64 = 20 * 1024 * 1024 * 1024;

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
pub enum CacheStatus {
    Hit,
    Miss,
}

#[derive(Debug)]
pub struct Cached<T> {
    pub value: T,
    pub cache_status: CacheStatus,
}

#[derive(Default, Serialize, Deserialize)]
struct Manifest {
    entries: BTreeMap<String, Entry>,
}
#[derive(Serialize, Deserialize)]
struct Entry {
    size: u64,
    last_use: u128,
}

pub(crate) fn digest(bytes: &[u8]) -> String {
    format!("{:x}", Sha256::digest(bytes))
}
pub(crate) fn package_key(
    build: &str,
    entity: &str,
    options: &crate::DecomposedOptions,
) -> Result<String> {
    Ok(digest(&serde_json::to_vec(&(
        CACHE_FORMAT_VERSION,
        build,
        entity,
        options,
    ))?))
}

pub(crate) struct Cache {
    root: PathBuf,
}
impl Cache {
    fn lock(&self) -> Result<fs::File> {
        let file = fs::OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .truncate(false)
            .open(self.root.join(".lock"))?;
        file.lock()?;
        Ok(file)
    }
    pub fn new(root: &Path) -> Result<Self> {
        fs::create_dir_all(root)?;
        Ok(Self {
            root: root.to_owned(),
        })
    }
    fn manifest(&self) -> Result<Manifest> {
        match fs::read(self.root.join("manifest.json")) {
            Ok(bytes) => Ok(serde_json::from_slice(&bytes).context("invalid cache manifest")?),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(Manifest::default()),
            Err(e) => Err(e.into()),
        }
    }
    fn save_manifest(&self, manifest: &Manifest) -> Result<()> {
        atomic_write(
            &self.root.join("manifest.json"),
            &serde_json::to_vec_pretty(manifest)?,
        )
    }
    fn touch(&self, name: &str, size: u64) -> Result<()> {
        let mut manifest = self.manifest()?;
        manifest.entries.insert(
            name.into(),
            Entry {
                size,
                last_use: SystemTime::now().duration_since(UNIX_EPOCH)?.as_nanos(),
            },
        );
        self.save_manifest(&manifest)
    }
    pub fn read(&self, name: &str) -> Result<Option<Vec<u8>>> {
        let _lock = self.lock()?;
        match fs::read(self.root.join(name)) {
            Ok(bytes) => {
                self.touch(name, bytes.len() as u64)?;
                Ok(Some(bytes))
            }
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
            Err(e) => Err(e.into()),
        }
    }
    pub fn write(&self, name: &str, bytes: &[u8]) -> Result<()> {
        let _lock = self.lock()?;
        atomic_write(&self.root.join(name), bytes)?;
        self.touch(name, bytes.len() as u64)?;
        self.evict_locked(DEFAULT_CACHE_CAP_BYTES)?;
        Ok(())
    }
    pub fn evict(&self, cap: u64) -> Result<u64> {
        let _lock = self.lock()?;
        self.evict_locked(cap)
    }
    fn evict_locked(&self, cap: u64) -> Result<u64> {
        let mut manifest = self.manifest()?;
        let mut entries: Vec<_> = manifest
            .entries
            .iter()
            .map(|(k, v)| (k.clone(), v.last_use, v.size))
            .collect();
        entries.sort_by_key(|e| e.1);
        let mut total: u64 = entries.iter().map(|e| e.2).sum();
        let mut removed = 0;
        for (name, _, size) in entries {
            anyhow::ensure!(
                Path::new(&name).components().count() == 1 && !name.starts_with('.'),
                "invalid cache entry path"
            );
            if total <= cap {
                break;
            }
            match fs::remove_file(self.root.join(&name)) {
                Ok(()) => {}
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
                Err(e) => return Err(e.into()),
            }
            total -= size;
            removed += size;
            manifest.entries.remove(&name);
        }
        self.save_manifest(&manifest)?;
        Ok(removed)
    }
}
pub(crate) fn atomic_write(path: &Path, bytes: &[u8]) -> Result<()> {
    use std::io::Write;
    let parent = path
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .unwrap_or(Path::new("."));
    fs::create_dir_all(parent)?;
    let mut file = tempfile::NamedTempFile::new_in(parent)?;
    file.write_all(bytes)?;
    file.persist(path)?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn cache_keys_separate_builds_options_entities_and_version() {
        let o = crate::DecomposedOptions::default();
        let a = package_key("build-a", "entity", &o).unwrap();
        assert_ne!(a, package_key("build-b", "entity", &o).unwrap());
        assert_ne!(a, package_key("build-a", "other", &o).unwrap());
        let mut b = o.clone();
        b.geometry.lod += 1;
        assert_ne!(a, package_key("build-a", "entity", &b).unwrap());
        assert_ne!(
            a,
            digest(
                &serde_json::to_vec(&(CACHE_FORMAT_VERSION + 1, "build-a", "entity", o)).unwrap()
            )
        );
    }
    #[test]
    fn eviction_respects_recent_reads_and_tracks_sizes() {
        let dir = tempfile::tempdir().unwrap();
        let c = Cache::new(dir.path()).unwrap();
        c.write("a.tar", b"aa").unwrap();
        c.write("b.tar", b"bbb").unwrap();
        assert_eq!(c.read("a.tar").unwrap().unwrap(), b"aa");
        assert_eq!(c.evict(2).unwrap(), 3);
        assert!(c.read("b.tar").unwrap().is_none());
        assert_eq!(c.manifest().unwrap().entries["a.tar"].size, 2);
        c.evict(0).unwrap();
        assert!(c.read("a.tar").unwrap().is_none());
    }
}
