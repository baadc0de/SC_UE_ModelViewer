//! Metadata-only authored vehicle and interior lights, sharing exporter discovery.
use crate::{CacheStatus, Session};
use anyhow::{Result, anyhow};
pub use starbreaker_3d::types::{LightAttachment, LightInfo, LightStateInfo};
pub use starbreaker_3d::{EntityLight, LightCatalogue, VehicleLightPort};

impl Session {
    /// List authored lights without exporting meshes or textures. Exact entity name required.
    /// Positions are owner-entity-local SC metres; optional attachment-local transforms
    /// follow animated helpers. This lightweight operation currently reports a cache miss.
    pub fn lights(&self, entity: &str) -> Result<LightCatalogue> {
        let entity = self.resolve_name(entity)?;
        let loaded = self.loaded()?;
        let catalogue = loaded.index.with_dependent(|_, idx| -> Result<_> {
            let record = idx
                .find_record(entity)
                .ok_or_else(|| anyhow!("entity disappeared from DataCore: {entity}"))?;
            let tree = starbreaker_datacore::loadout::resolve_loadout_indexed(idx, record);
            Ok(starbreaker_3d::entity_light_catalogue(
                idx.db,
                &loaded.p4k,
                record,
                &tree,
            )?)
        })?;
        self.status.set(CacheStatus::Miss);
        Ok(catalogue)
    }
}
