//! UI-independent StarBreaker sessions, geometry browsing, clip catalogues, lights and exports.
//!
//! Decomposed mesh materials retain `<mtl stem>_<submaterial name>_<NN>`:
//! `NN` is the zero-based sidecar `submaterials[]` index and the GLB primitive's
//! material index. Sidecars are named `<mtl stem>_TEX<m>.materials.json`.
//!
//! `open` loads DataCore and indexes once on a cold start. A persisted browser
//! index defers archive/DataCore loading until the first uncached export or raw
//! geometry read. The upstream `MappedP4k` uses positional reads (not mmap).
//! Sessions are single-owner, not Sync; upstream's entity index contains RefCell.
//! Exact brief-compatible Vec APIs expose status via `cache_status()`. The
//! `_cached` methods return status alongside data for callers that need it owned.
mod cache;
mod catalogue;
mod lights;
pub use catalogue::{Catalogue, ClipInfo, ClipSource, Fragment, SkeletonInfo};
pub use lights::{
    EntityLight, LightAttachment, LightCatalogue, LightInfo, LightStateInfo, VehicleLightPort,
};
mod index;
mod options;
mod package;

use anyhow::{Context, Result, anyhow, ensure};
pub use cache::{
    CACHE_FORMAT_VERSION, CacheStats, CacheStatus, Cached, DEFAULT_CACHE_CAP_BYTES, EXPORTER_DIRTY,
    EXPORTER_REVISION, EXPORTER_VERSION,
};
use index::{BrowserIndex, Loaded, normalize};
pub use index::{DirEntry, EntityHit};
use once_cell::unsync::OnceCell;
pub use options::{DecomposedOptions, GlbOptions, Materials};
use starbreaker_3d::{ExportKind, ExportOptions};
pub use starbreaker_3d::{ValidationReport, validate_decomposed_export};
use starbreaker_datacore::loadout::resolve_loadout_indexed;
use std::{
    cell::Cell,
    fs,
    path::{Path, PathBuf},
    sync::Arc,
    time::{Duration, UNIX_EPOCH},
};

pub type ProgressCallback = Arc<dyn Fn(f32, &str) + Send + Sync>;
#[derive(Debug)]
pub struct ExportResult {
    pub files: Vec<PathBuf>,
    pub cache_status: CacheStatus,
}

pub struct Session {
    path: PathBuf,
    build_id: String,
    branch: Option<String>,
    browser: BrowserIndex,
    loaded: OnceCell<Loaded>,
    cache: Option<cache::Cache>,
    index_status: CacheStatus,
    status: Cell<CacheStatus>,
    progress: Option<ProgressCallback>,
}
impl Session {
    pub fn open(path: Option<&Path>, cache_dir: Option<&Path>) -> Result<Self> {
        let path = match path {
            Some(p) => p.to_owned(),
            None => starbreaker_common::discover::find_p4k()?.path,
        };
        let (build_id, branch) = build_identity(&path)?;
        let cache = cache_dir.map(cache::Cache::new).transpose()?;
        let safe_id = if build_id
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || c == '-')
        {
            build_id.clone()
        } else {
            cache::digest(build_id.as_bytes())
        };
        let index_name = format!("entities.v{CACHE_FORMAT_VERSION}.{safe_id}.bin");
        let loaded = OnceCell::new();
        let cached = cache
            .as_ref()
            .map(|c| c.read(&index_name))
            .transpose()?
            .flatten();
        let cached_index = cached
            .as_deref()
            .and_then(|bytes| BrowserIndex::decode(bytes, &build_id));
        let (browser, index_status) = if let Some(index) = cached_index {
            (index, CacheStatus::Hit)
        } else {
            if cached.is_some() {
                if let Some(c) = &cache {
                    c.remove(&index_name)?;
                }
            }
            let index =
                BrowserIndex::build(loaded.get_or_try_init(|| Loaded::open(&path))?, &build_id)?;
            if let Some(c) = &cache {
                c.write(&index_name, &bincode::serialize(&index)?)?;
            }
            (index, CacheStatus::Miss)
        };
        Ok(Self {
            path,
            build_id,
            branch,
            browser,
            loaded,
            cache,
            index_status,
            status: Cell::new(index_status),
            progress: None,
        })
    }
    pub fn p4k_path(&self) -> &Path {
        &self.path
    }
    pub fn build_branch(&self) -> Option<&str> {
        self.branch.as_deref()
    }
    pub fn build_id(&self) -> &str {
        &self.build_id
    }
    /// Status of the most recent successful operation (uncached operations are Miss).
    pub fn cache_status(&self) -> CacheStatus {
        self.status.get()
    }
    pub fn set_progress_callback(&mut self, callback: Option<ProgressCallback>) {
        self.progress = callback;
    }
    pub fn search_entities(&self, query: &str, limit: usize) -> Vec<EntityHit> {
        self.search_entities_cached(query, limit).value
    }
    pub fn search_entities_cached(&self, query: &str, limit: usize) -> Cached<Vec<EntityHit>> {
        let query = query.to_lowercase();
        let value = self
            .browser
            .entities
            .iter()
            .filter(|e| {
                e.name.to_lowercase().contains(&query)
                    || e.display_name
                        .as_ref()
                        .is_some_and(|n| n.to_lowercase().contains(&query))
            })
            .take(limit)
            .cloned()
            .collect();
        self.status.set(self.index_status);
        Cached {
            value,
            cache_status: self.index_status,
        }
    }
    /// Immediate geometry files and directories containing geometry, using forward slashes.
    pub fn list_dir(&self, p4k_dir: &str) -> Vec<DirEntry> {
        let dir = normalize(p4k_dir).to_lowercase();
        self.status.set(self.index_status);
        self.browser
            .paths
            .iter()
            .filter(|e| {
                e.path
                    .rsplit_once('/')
                    .map_or("", |(p, _)| p)
                    .to_lowercase()
                    == dir
            })
            .cloned()
            .collect()
    }
    pub fn list_dir_cached(&self, p4k_dir: &str) -> Cached<Vec<DirEntry>> {
        Cached {
            value: self.list_dir(p4k_dir),
            cache_status: self.index_status,
        }
    }
    /// Case-insensitive slash-normalized glob over .cga/.cgf/.skin/.chr/.cdf files.
    pub fn search_paths(&self, glob: &str, limit: usize) -> Vec<DirEntry> {
        let glob = normalize(glob).to_lowercase();
        self.status.set(self.index_status);
        self.browser
            .paths
            .iter()
            .filter(|e| !e.is_dir && glob_match::glob_match(&glob, &e.path.to_lowercase()))
            .take(limit)
            .cloned()
            .collect()
    }
    pub fn search_paths_cached(&self, glob: &str, limit: usize) -> Cached<Vec<DirEntry>> {
        Cached {
            value: self.search_paths(glob, limit),
            cache_status: self.index_status,
        }
    }
    fn loaded(&self) -> Result<&Loaded> {
        self.loaded.get_or_try_init(|| Loaded::open(&self.path))
    }
    fn resolve_name(&self, query: &str) -> Result<&str> {
        let query = query.to_lowercase();
        self.browser
            .entities
            .iter()
            .find(|e| e.name.to_lowercase() == query)
            .map(|e| e.name.as_str())
            .ok_or_else(|| anyhow!("no entity matching {query:?}"))
    }
    fn assemble(
        &self,
        name: &str,
        options: &ExportOptions,
    ) -> Result<starbreaker_3d::ExportResult> {
        let loaded = self.loaded()?;
        let name = self.resolve_name(name)?;
        let progress = starbreaker_common::Progress::new();
        std::thread::scope(|scope| {
            let (sender, receiver) = std::sync::mpsc::channel::<()>();
            if let Some(callback) = &self.progress {
                let progress = &progress;
                scope.spawn(move || {
                    loop {
                        let (fraction, stage) = progress.get();
                        callback(fraction, &stage);
                        if receiver.recv_timeout(Duration::from_millis(20))
                            != Err(std::sync::mpsc::RecvTimeoutError::Timeout)
                        {
                            break;
                        }
                    }
                });
            }
            let result = loaded.index.with_dependent(|_, idx| -> Result<_> {
                let record = idx
                    .find_record(name)
                    .ok_or_else(|| anyhow!("entity disappeared from DataCore: {name}"))?;
                let tree = resolve_loadout_indexed(idx, record);
                Ok(starbreaker_3d::assemble_glb_with_loadout_with_progress(
                    idx.db,
                    &loaded.p4k,
                    record,
                    &tree,
                    options,
                    Some(&progress),
                    None,
                    None,
                    None,
                )?)
            });
            drop(sender);
            result
        })
    }
    pub fn export_entity_glb(&self, name: &str, options: &GlbOptions) -> Result<Vec<u8>> {
        let result = self.assemble(name, &options.upstream(ExportKind::Bundled))?;
        ensure!(
            !result.glb.is_empty(),
            "upstream returned an empty bundled GLB"
        );
        self.status.set(CacheStatus::Miss);
        if let Some(cb) = &self.progress {
            cb(1.0, "GLB ready");
        }
        Ok(result.glb)
    }
    pub fn geometry_file_glb(&self, p4k_path: &str) -> Result<Vec<u8>> {
        let p4k = &self.loaded()?.p4k;
        let mut primary = p4k_path.replace('/', "\\");
        let lower = primary.to_lowercase();
        if [".skinm", ".cgfm", ".cgam"]
            .iter()
            .any(|e| lower.ends_with(e))
        {
            primary.pop();
        }
        let metadata = p4k.read_file(&primary)?;
        let companion = format!("{primary}m");
        let streams = if p4k.entry_case_insensitive(&companion).is_some() {
            p4k.read_file(&companion)?
        } else {
            metadata.clone()
        };
        let bytes = starbreaker_3d::skin_to_glb(&streams, Some(&metadata))?;
        self.status.set(CacheStatus::Miss);
        Ok(bytes)
    }
    pub fn export_entity_glb_cached(
        &self,
        name: &str,
        options: &GlbOptions,
    ) -> Result<Cached<Vec<u8>>> {
        Ok(Cached {
            value: self.export_entity_glb(name, options)?,
            cache_status: CacheStatus::Miss,
        })
    }
    pub fn geometry_file_glb_cached(&self, p4k_path: &str) -> Result<Cached<Vec<u8>>> {
        Ok(Cached {
            value: self.geometry_file_glb(p4k_path)?,
            cache_status: CacheStatus::Miss,
        })
    }
    pub fn export_entity_decomposed_mem(
        &self,
        name: &str,
        options: &DecomposedOptions,
    ) -> Result<Vec<u8>> {
        Ok(self
            .export_entity_decomposed_mem_cached(name, options)?
            .value)
    }
    pub fn export_entity_decomposed_mem_cached(
        &self,
        name: &str,
        options: &DecomposedOptions,
    ) -> Result<Cached<Vec<u8>>> {
        let name = self.resolve_name(name)?;
        let filename = format!("{}.tar", cache::package_key(&self.build_id, name, options)?);
        if let Some(cache) = &self.cache {
            if let Some(bytes) = cache.read(&filename)? {
                if package::validate_tar(&bytes).is_ok() {
                    self.status.set(CacheStatus::Hit);
                    if let Some(cb) = &self.progress {
                        cb(1.0, "Package cache hit");
                    }
                    return Ok(Cached {
                        value: bytes,
                        cache_status: CacheStatus::Hit,
                    });
                }
                cache.remove(&filename)?;
            }
        }
        let result = self.assemble(name, &options.geometry.upstream(ExportKind::Decomposed))?;
        let package = result
            .decomposed
            .context("upstream returned no decomposed files")?;
        let bytes = package::to_tar(&package)?;
        if let Some(cache) = &self.cache {
            cache.write(&filename, &bytes)?;
        }
        self.status.set(CacheStatus::Miss);
        if let Some(cb) = &self.progress {
            cb(1.0, "Package ready");
        }
        Ok(Cached {
            value: bytes,
            cache_status: CacheStatus::Miss,
        })
    }
    pub fn export_entity_decomposed(
        &self,
        name: &str,
        out_root: &Path,
        options: &DecomposedOptions,
    ) -> Result<ExportResult> {
        let result = self.export_entity_decomposed_mem_cached(name, options)?;
        Ok(ExportResult {
            files: package::unpack(&result.value, out_root)?,
            cache_status: result.cache_status,
        })
    }
    /// Evict least recently used index/package files; return bytes removed.
    pub fn cache_evict(&self, cap_bytes: u64) -> Result<u64> {
        self.cache
            .as_ref()
            .map_or(Ok(0), |cache| cache.evict(cap_bytes))
    }
}
fn build_identity(path: &Path) -> Result<(String, Option<String>)> {
    let metadata = fs::metadata(path).with_context(|| format!("opening {}", path.display()))?;
    let manifest = path.with_file_name("build_manifest.id");
    match fs::read(&manifest) {
        Ok(bytes) => {
            let doc: serde_json::Value =
                serde_json::from_slice(&bytes).context("parsing build_manifest.id")?;
            let id = doc["Data"]["BuildId"]
                .as_str()
                .context("build_manifest.id missing Data.BuildId")?;
            let branch = doc["Data"]["Branch"].as_str().map(str::to_owned);
            Ok((id.to_owned(), branch))
        }
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok((
            format!(
                "{}-{}",
                metadata.len(),
                metadata.modified()?.duration_since(UNIX_EPOCH)?.as_nanos()
            ),
            None,
        )),
        Err(e) => Err(e.into()),
    }
}

#[cfg(test)]
mod tests;
