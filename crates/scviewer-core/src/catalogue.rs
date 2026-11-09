//! Layer-1 clip catalogues, keyed independently of geometry export options.
use crate::{CacheStatus, Cached, Session, cache};
use anyhow::{Result, anyhow};
use serde::{Deserialize, Serialize};
pub use starbreaker_3d::animation::catalogue::{
    Catalogue, ClipInfo, ClipSource, Fragment, SkeletonInfo,
};

// Independent schema discriminator prevents collisions with browser/package entries.
const CATALOGUE_VERSION: u32 = 1;
#[derive(Serialize, Deserialize)]
struct Envelope {
    key: String,
    catalogue: Catalogue,
}
impl Envelope {
    fn decode(bytes: &[u8], key: &str, entity: &str) -> Option<Catalogue> {
        use bincode::Options;
        let envelope = bincode::DefaultOptions::new()
            .with_fixint_encoding()
            .with_limit(bytes.len() as u64)
            .reject_trailing_bytes()
            .deserialize::<Self>(bytes)
            .ok()?;
        (envelope.key == key && envelope.catalogue.entity == entity).then_some(envelope.catalogue)
    }
}
fn key(build: &str, entity: &str) -> Result<String> {
    Ok(cache::digest(&serde_json::to_vec(&(
        "clip-catalogue",
        CATALOGUE_VERSION,
        crate::CACHE_FORMAT_VERSION,
        crate::EXPORTER_VERSION,
        build,
        entity,
    ))?))
}
impl Session {
    /// List the default decomposed package's clips without exporting geometry or textures.
    pub fn clip_catalogue(&self, entity: &str) -> Result<Catalogue> {
        Ok(self.clip_catalogue_cached(entity)?.value)
    }
    pub fn clip_catalogue_cached(&self, entity: &str) -> Result<Cached<Catalogue>> {
        let entity = self.resolve_name(entity)?;
        let key = key(&self.build_id, entity)?;
        let filename = format!("clips.{key}.bin");
        if let Some(cache) = &self.cache {
            if let Some(bytes) = cache.read(&filename)? {
                if let Some(catalogue) = Envelope::decode(&bytes, &key, entity) {
                    self.status.set(CacheStatus::Hit);
                    return Ok(Cached {
                        value: catalogue,
                        cache_status: CacheStatus::Hit,
                    });
                }
                cache.remove(&filename)?;
            }
        }
        let loaded = self.loaded()?;
        let catalogue = loaded.index.with_dependent(|_, idx| -> Result<_> {
            let record = idx
                .find_record(entity)
                .ok_or_else(|| anyhow!("entity disappeared from DataCore: {entity}"))?;
            let tree = starbreaker_datacore::loadout::resolve_loadout_indexed(idx, record);
            Ok(starbreaker_3d::entity_clip_catalogue(
                idx.db,
                &loaded.p4k,
                record,
                &tree,
            )?)
        })?;
        let envelope = Envelope { key, catalogue };
        if let Some(cache) = &self.cache {
            cache.write(&filename, &bincode::serialize(&envelope)?)?;
        }
        self.status.set(CacheStatus::Miss);
        Ok(Cached {
            value: envelope.catalogue,
            cache_status: CacheStatus::Miss,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn catalogue_cache_rejects_wrong_identity_truncation_and_trailing_bytes() {
        let key = key("build", "entity").unwrap();
        let catalogue = Catalogue {
            entity: "entity".into(),
            skeletons: vec![],
            clips: vec![],
        };
        let bytes = bincode::serialize(&Envelope {
            key: key.clone(),
            catalogue: catalogue.clone(),
        })
        .unwrap();
        assert_eq!(Envelope::decode(&bytes, &key, "entity"), Some(catalogue));
        assert!(Envelope::decode(&bytes, "other-version-or-build-key", "entity").is_none());
        assert!(Envelope::decode(&bytes, &key, "other-entity").is_none());
        assert!(Envelope::decode(&bytes[..bytes.len() - 1], &key, "entity").is_none());
        let mut trailing = bytes.clone();
        trailing.push(0);
        assert!(Envelope::decode(&trailing, &key, "entity").is_none());
        assert!(Envelope::decode(&[255; 64], &key, "entity").is_none());
        let dir = tempfile::tempdir().unwrap();
        let cache = cache::Cache::new(dir.path()).unwrap();
        cache.write("clips.synthetic.bin", &bytes).unwrap();
        assert_eq!(cache.read("clips.synthetic.bin").unwrap().unwrap(), bytes);
        assert_eq!(cache.evict(0).unwrap(), bytes.len() as u64);
        assert!(cache.read("clips.synthetic.bin").unwrap().is_none());
    }
}
