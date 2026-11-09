//! Translate the core's recovered cache statistics to C ABI fields.
use crate::{boundary::Result, sc_cache_info, sc_handle};

pub(crate) fn stats(handle: &sc_handle) -> Result<sc_cache_info> {
    let stats = handle.session.cache_stats()?;
    Ok(sc_cache_info {
        enabled: stats.enabled.into(),
        last_hit: (stats.last_status == scviewer_core::CacheStatus::Hit).into(),
        cap_bytes: stats.cap_bytes,
        entries: stats.entries,
        bytes: stats.bytes,
    })
}
