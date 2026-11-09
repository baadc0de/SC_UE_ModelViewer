//! Read-only layer-1 manifest statistics; eviction remains delegated to Session.
use crate::{
    boundary::{Failure, Result},
    sc_cache_info, sc_handle,
};
use serde::Deserialize;
use std::{collections::BTreeMap, fs};

#[derive(Deserialize)]
struct Manifest {
    entries: BTreeMap<String, Entry>,
}
#[derive(Deserialize)]
struct Entry {
    size: u64,
}

pub(crate) fn stats(handle: &sc_handle) -> Result<sc_cache_info> {
    let mut info = sc_cache_info {
        enabled: handle.cache_dir.is_some().into(),
        last_hit: (handle.session.cache_status() == scviewer_core::CacheStatus::Hit).into(),
        cap_bytes: scviewer_core::DEFAULT_CACHE_CAP_BYTES,
        ..Default::default()
    };
    if let Some(root) = &handle.cache_dir {
        // Core atomically replaces this file; readers see one complete snapshot.
        // Do not duplicate its recovery policy or mutate it from the FFI layer.
        let bytes = fs::read(root.join("manifest.json")).map_err(anyhow::Error::from)?;
        let manifest: Manifest = serde_json::from_slice(&bytes).map_err(anyhow::Error::from)?;
        info.entries = manifest.entries.len() as u64;
        info.bytes = manifest
            .entries
            .values()
            .try_fold(0u64, |sum, e| sum.checked_add(e.size))
            .ok_or_else(|| Failure::from(anyhow::anyhow!("cache size overflow")))?;
    }
    Ok(info)
}
