//! Cached player-kit export. Player identity is isolated from entity cache keys.
use crate::{CacheStatus, CancelToken, Session, cache, package};
use anyhow::{Result, ensure};
pub use starbreaker_3d::character::player::{PlayerLoadout, PlayerRequest};

impl Session {
    pub fn player_loadouts(&self, gender: &str) -> Result<Vec<PlayerLoadout>> {
        let gender = gender.parse().map_err(anyhow::Error::msg)?;
        let loaded = self.loaded()?;
        let value = loaded.index.with_dependent(|_, idx| {
            starbreaker_3d::character::player::player_loadouts(&loaded.p4k, idx.db, gender)
        })?;
        self.status.set(CacheStatus::Miss);
        Ok(value)
    }

    pub fn export_player(
        &self,
        request: &PlayerRequest,
        cancel: Option<&CancelToken>,
    ) -> Result<Vec<u8>> {
        self.checkpoint(cancel, 0.0, "Preparing player")?;
        ensure!(
            matches!(
                request.gender.to_ascii_lowercase().as_str(),
                "male" | "female"
            ),
            "gender must be male or female"
        );
        let key = player_key(&self.build_id, request)?;
        if let Some(cache) = &self.cache {
            if let Some(bytes) = cache.read(&key)? {
                if package::validate_tar(&bytes).is_ok() {
                    self.checkpoint(cancel, 1.0, "Player cache hit")?;
                    self.status.set(CacheStatus::Hit);
                    return Ok(bytes);
                }
                cache.remove(&key)?;
            }
        }
        self.checkpoint(cancel, 0.1, "Assembling player")?;
        let loaded = self.loaded()?;
        let result = loaded.index.with_dependent(|_, idx| {
            starbreaker_3d::character::player::export_player(&loaded.p4k, idx.db, request)
        })?;
        self.checkpoint(cancel, 0.94, "Building player tar")?;
        let bytes = package::to_tar(&result)?;
        self.checkpoint(cancel, 1.0, "Publishing player")?;
        if let Some(cache) = &self.cache {
            cache.write(&key, &bytes)?;
        }
        self.status.set(CacheStatus::Miss);
        Ok(bytes)
    }
}
fn player_key(build: &str, request: &PlayerRequest) -> Result<String> {
    player_key_for_exporter(build, request, cache::EXPORTER_VERSION)
}
fn player_key_for_exporter(
    build: &str,
    request: &PlayerRequest,
    exporter: &str,
) -> Result<String> {
    let material = serde_json::to_vec(&(
        "player",
        1u32,
        crate::CACHE_FORMAT_VERSION,
        exporter,
        build,
        request,
    ))?;
    Ok(format!("player-{}.tar", cache::digest(&material)))
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn player_cache_separates_gender_loadout_and_archive() {
        let a = PlayerRequest::default();
        let key = player_key("synthetic_build", &a).unwrap();
        assert_ne!(
            player_key_for_exporter("synthetic_build", &a, "one").unwrap(),
            player_key_for_exporter("synthetic_build", &a, "two").unwrap()
        );
        assert_ne!(key, player_key("other_build", &a).unwrap());
        assert_ne!(
            key,
            player_key(
                "synthetic_build",
                &PlayerRequest {
                    gender: "female".into(),
                    ..a.clone()
                }
            )
            .unwrap()
        );
        assert_ne!(
            key,
            player_key(
                "synthetic_build",
                &PlayerRequest {
                    loadout: Some("synthetic.xml".into()),
                    ..a
                }
            )
            .unwrap()
        );
    }
}
