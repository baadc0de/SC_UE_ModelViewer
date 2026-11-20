//! Cached player-kit export. Player identity is isolated from entity cache keys.
use crate::{CacheStatus, CancelToken, Session, cache, package};
use anyhow::{Result, ensure};
pub use starbreaker_3d::character::player::{PlayerClipsRequest, PlayerLoadout, PlayerRequest};

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
        if let Some(bytes) = self.cached_player_tar(&key, cancel)? {
            return Ok(bytes);
        }
        self.checkpoint(cancel, 0.1, "Assembling player")?;
        let loaded = self.loaded()?;
        let mut result = loaded.index.with_dependent(|_, idx| {
            starbreaker_3d::character::player::export_player(&loaded.p4k, idx.db, request)
        })?;
        // The kit's LayerBlend_V2 armour samples the engine detail map too (WP-U2b); player textures are full mip.
        crate::engine_textures::add_to_package(&mut result, &loaded.p4k, 0)?;
        self.checkpoint(cancel, 0.94, "Building player tar")?;
        let bytes = package::to_tar(&result)?;
        self.checkpoint(cancel, 1.0, "Publishing player")?;
        if let Some(cache) = &self.cache {
            cache.write(&key, &bytes)?;
        }
        self.status.set(CacheStatus::Miss);
        Ok(bytes)
    }

    pub fn export_player_clips(&self, request: &PlayerClipsRequest, cancel: Option<&CancelToken>) -> Result<Vec<u8>> {
        self.checkpoint(cancel, 0.0, "Preparing player action clips")?;
        ensure!(!request.names().is_empty(), "no player action clips requested");
        let key = player_clips_key(&self.build_id, request)?;
        if let Some(bytes) = self.cached_player_tar(&key, cancel)? {
            return Ok(bytes);
        }
        let loaded = self.loaded()?;
        self.checkpoint(cancel, 0.1, "Reading player skeleton and clips")?;
        let export = loaded.index.with_dependent(|_, idx| {
            starbreaker_3d::character::player::export_player_clips(&loaded.p4k, idx.db, request)
        })?;
        self.checkpoint(cancel, 0.95, "Building player clip tar")?;
        let bytes = package::to_tar(&export)?;
        self.checkpoint(cancel, 1.0, "Publishing player clips")?;
        if let Some(cache) = &self.cache {
            cache.write(&key, &bytes)?;
        }
        self.status.set(CacheStatus::Miss);
        Ok(bytes)
    }

    fn cached_player_tar(&self, key: &str, cancel: Option<&CancelToken>) -> Result<Option<Vec<u8>>> {
        let Some(cache) = &self.cache else { return Ok(None) };
        let Some(bytes) = cache.read(key)? else { return Ok(None) };
        if package::validate_tar(&bytes).is_err() {
            cache.remove(key)?;
            return Ok(None);
        }
        self.checkpoint(cancel, 1.0, "Player cache hit")?;
        self.status.set(CacheStatus::Hit);
        Ok(Some(bytes))
    }
}

fn player_clips_key(build: &str, request: &PlayerClipsRequest) -> Result<String> {
    let material = serde_json::to_vec(&(
        "player-clips", 1u32, crate::CACHE_FORMAT_VERSION, cache::EXPORTER_VERSION,
        build, request.gender.to_ascii_lowercase(), request.names(),
    ))?;
    Ok(format!("player-clips-{}.tar", cache::digest(&material)))
}
fn player_key(build: &str, request: &PlayerRequest) -> Result<String> {
    player_key_for_exporter(build, request, cache::EXPORTER_VERSION)
}
fn player_key_for_exporter(
    build: &str,
    request: &PlayerRequest,
    exporter: &str,
) -> Result<String> {
    // Player-kit layout revision: 2 adds `player.flashlights` and their cookies (an uncommitted exporter change does
    // not move EXPORTER_VERSION once the vendored tree is already dirty).
    let material = serde_json::to_vec(&(
        "player",
        2u32,
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
    fn player_clip_cache_key_is_stable_for_a_set_of_names() {
        let original = PlayerClipsRequest { gender: "male".into(), clips: vec!["one".into(), "two".into()] };
        let reversed = PlayerClipsRequest { gender: "MALE".into(), clips: vec!["two".into(), " One ".into(), "one".into()] };
        let key = player_clips_key("fixture", &original).unwrap();
        assert_eq!(key, player_clips_key("fixture", &reversed).unwrap());
        assert_ne!(key, player_clips_key("other-fixture", &original).unwrap());
        assert_ne!(key, player_clips_key("fixture", &PlayerClipsRequest { gender: "female".into(), ..original }).unwrap());
    }
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
                    weapons: Some(vec!["synthetic_weapon".into()]),
                    spare_attachments: true,
                    ..a
                }
            )
            .unwrap()
        );
    }
}
