//! Versioned local cache, corruption recovery, atomic publication and LRU eviction.
use anyhow::Result;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
    collections::BTreeMap,
    fs,
    io::Read,
    path::{Component, Path, PathBuf},
    time::{SystemTime, UNIX_EPOCH},
};

pub const CACHE_FORMAT_VERSION: u32 = 6;
pub const DEFAULT_CACHE_CAP_BYTES: u64 = 20 * 1024 * 1024 * 1024;
pub const EXPORTER_REVISION: &str = env!("SCVIEWER_EXPORTER_REVISION");
pub const EXPORTER_DIRTY: &str = env!("SCVIEWER_EXPORTER_DIRTY");
pub const EXPORTER_VERSION: &str = env!("SCVIEWER_EXPORTER_VERSION");

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
    checksum: String,
}

pub(crate) fn digest(bytes: &[u8]) -> String {
    format!("{:x}", Sha256::digest(bytes))
}
pub(crate) fn package_key(
    build: &str,
    entity: &str,
    options: &crate::DecomposedOptions,
) -> Result<String> {
    package_key_for_exporter(build, entity, options, EXPORTER_VERSION)
}
fn package_key_for_exporter(
    build: &str,
    entity: &str,
    options: &crate::DecomposedOptions,
    exporter: &str,
) -> Result<String> {
    Ok(digest(&serde_json::to_vec(&(
        CACHE_FORMAT_VERSION,
        exporter,
        build,
        entity,
        options,
    ))?))
}
fn entry_name(name: &str) -> bool {
    matches!(
        Path::new(name).components().next(),
        Some(Component::Normal(_))
    ) && Path::new(name).components().count() == 1
        && !name.starts_with('.')
        && (name.ends_with(".tar") || name.ends_with(".bin"))
}
fn remove_if_present(path: &Path) -> Result<()> {
    match fs::remove_file(path) {
        Ok(()) => Ok(()),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(e) => Err(e.into()),
    }
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
        let cache = Self {
            root: root.to_owned(),
        };
        let _lock = cache.lock()?;
        cache.remove_orphans()?;
        Ok(cache)
    }
    fn remove_orphans(&self) -> Result<u64> {
        let mut removed = 0;
        for entry in fs::read_dir(&self.root)? {
            let entry = entry?;
            if entry.file_name().to_string_lossy().starts_with(".tmp")
                && entry.file_type()?.is_file()
            {
                removed += entry.metadata()?.len();
                fs::remove_file(entry.path())?;
            }
        }
        Ok(removed)
    }
    fn manifest(&self) -> Result<Manifest> {
        match fs::read(self.root.join("manifest.json")) {
            Ok(bytes) => {
                if let Ok(manifest) = serde_json::from_slice::<Manifest>(&bytes) {
                    if manifest.entries.keys().all(|n| entry_name(n))
                        && manifest
                            .entries
                            .values()
                            .try_fold(0u64, |sum, e| sum.checked_add(e.size))
                            .is_some()
                    {
                        return Ok(manifest);
                    }
                }
            }
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
            Err(e) => return Err(e.into()),
        }
        // Recover only cache-owned regular files. Sizes, checksums and approximate
        // last use come from the files, never from a broken manifest.
        let mut manifest = Manifest::default();
        for entry in fs::read_dir(&self.root)? {
            let entry = entry?;
            let name = entry.file_name().to_string_lossy().into_owned();
            if !entry_name(&name) || !entry.file_type()?.is_file() {
                continue;
            }
            let metadata = entry.metadata()?;
            let mut file = fs::File::open(entry.path())?;
            let mut hasher = Sha256::new();
            let mut chunk = [0; 64 * 1024];
            loop {
                let n = file.read(&mut chunk)?;
                if n == 0 {
                    break;
                }
                hasher.update(&chunk[..n]);
            }
            manifest.entries.insert(
                name,
                Entry {
                    size: metadata.len(),
                    last_use: metadata.modified()?.duration_since(UNIX_EPOCH)?.as_nanos(),
                    checksum: format!("{hasher:x}", hasher = hasher.finalize()),
                },
            );
        }
        self.save_manifest(&manifest)?;
        Ok(manifest)
    }
    fn save_manifest(&self, manifest: &Manifest) -> Result<()> {
        atomic_write(
            &self.root.join("manifest.json"),
            &serde_json::to_vec_pretty(manifest)?,
        )
    }
    pub fn remove(&self, name: &str) -> Result<()> {
        let _lock = self.lock()?;
        let mut manifest = self.manifest()?;
        remove_if_present(&self.root.join(name))?;
        manifest.entries.remove(name);
        self.save_manifest(&manifest)
    }
    pub fn read(&self, name: &str) -> Result<Option<Vec<u8>>> {
        let _lock = self.lock()?;
        let mut manifest = self.manifest()?;
        let bytes = match fs::read(self.root.join(name)) {
            Ok(bytes) => bytes,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
                if manifest.entries.remove(name).is_some() {
                    self.save_manifest(&manifest)?;
                }
                return Ok(None);
            }
            Err(e) => return Err(e.into()),
        };
        if let Some(entry) = manifest.entries.get_mut(name) {
            if entry.size == bytes.len() as u64 && entry.checksum == digest(&bytes) {
                entry.last_use = SystemTime::now().duration_since(UNIX_EPOCH)?.as_nanos();
                self.save_manifest(&manifest)?;
                return Ok(Some(bytes));
            }
        }
        remove_if_present(&self.root.join(name))?;
        manifest.entries.remove(name);
        self.save_manifest(&manifest)?;
        Ok(None)
    }
    pub fn write(&self, name: &str, bytes: &[u8]) -> Result<()> {
        let _lock = self.lock()?;
        let mut manifest = self.manifest()?;
        atomic_write(&self.root.join(name), bytes)?;
        manifest.entries.insert(
            name.into(),
            Entry {
                size: bytes.len() as u64,
                last_use: SystemTime::now().duration_since(UNIX_EPOCH)?.as_nanos(),
                checksum: digest(bytes),
            },
        );
        self.save_manifest(&manifest)?;
        self.evict_locked(DEFAULT_CACHE_CAP_BYTES)?;
        Ok(())
    }
    pub fn evict(&self, cap: u64) -> Result<u64> {
        let _lock = self.lock()?;
        self.evict_locked(cap)
    }
    fn evict_locked(&self, cap: u64) -> Result<u64> {
        let mut removed = self.remove_orphans()?;
        let mut manifest = self.manifest()?;
        let mut entries: Vec<_> = manifest
            .entries
            .iter()
            .map(|(k, v)| (k.clone(), v.last_use, v.size))
            .collect();
        entries.sort_by_key(|e| e.1);
        let mut total: u64 = entries.iter().map(|e| e.2).sum();
        for (name, _, size) in entries {
            if total <= cap {
                break;
            }
            remove_if_present(&self.root.join(&name))?;
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
    file.as_file().sync_all()?;
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
    fn exporter_revision_and_dirty_flag_change_package_keys() {
        let options = crate::DecomposedOptions::default();
        let key =
            |identity| package_key_for_exporter("build", "entity", &options, identity).unwrap();
        assert_ne!(key("revision-a:dirty=false"), key("revision-b:dirty=false"));
        assert_ne!(key("revision-a:dirty=false"), key("revision-a:dirty=true"));
    }
    #[test]
    fn same_length_corruption_is_a_miss() {
        let dir = tempfile::tempdir().unwrap();
        let c = Cache::new(dir.path()).unwrap();
        c.write("package.tar", b"good").unwrap();
        fs::write(dir.path().join("package.tar"), b"evil").unwrap();
        assert!(c.read("package.tar").unwrap().is_none());
    }
    #[test]
    fn truncated_entry_is_removed_and_reported_as_a_miss() {
        let dir = tempfile::tempdir().unwrap();
        let c = Cache::new(dir.path()).unwrap();
        c.write("package.tar", b"complete package").unwrap();
        fs::write(dir.path().join("package.tar"), b"truncated").unwrap();
        assert!(c.read("package.tar").unwrap().is_none());
        assert!(!dir.path().join("package.tar").exists());
    }
    #[test]
    fn broken_manifest_recovers_entries_and_evicts_orphan_tempfiles() {
        let dir = tempfile::tempdir().unwrap();
        let c = Cache::new(dir.path()).unwrap();
        c.write("package.tar", b"package").unwrap();
        fs::write(dir.path().join("manifest.json"), b"broken JSON").unwrap();
        fs::write(dir.path().join(".tmp-orphan"), b"incomplete").unwrap();
        assert_eq!(c.read("package.tar").unwrap().unwrap(), b"package");
        c.evict(1024).unwrap();
        assert!(!dir.path().join(".tmp-orphan").exists());
        assert_eq!(c.manifest().unwrap().entries["package.tar"].size, 7);
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
