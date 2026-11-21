//! Archive-only soundtrack catalogue and on-demand conversion of intact Wwise music sources.
use anyhow::{Context, Result};
use serde::Serialize;
use starbreaker_wwise::{
    AtlIndex, HircObject,
    hirc::music::TrackPlaylistItem,
    library::{AudioLibrary, LoadedBank},
};

use crate::Session;

const BANK_DIR: &str = r"data\sounds\wwise\";
const SCORE_BANK_PREFIX: &str = "mus_music_";
// Wwise clip positions and durations are floating-point milliseconds.
const TIMELINE_EPSILON_MS: f64 = 0.01;

#[derive(Debug, Clone, Serialize)]
pub struct MusicCue {
    pub id: String,
    pub title: String,
    pub authored_name: String,
    pub bank: String,
    pub segment_id: u32,
    pub media_id: u32,
    pub seconds: f64,
}

fn cue_title(name: &str) -> Option<String> {
    let mut parts = name.split('_');
    if !matches!(parts.next()?, "MX" | "MXGS") {
        return None;
    }
    let terms: Vec<_> = parts.filter(|part| !part.is_empty()).collect();
    if terms.iter().any(|term| {
        let term = term.to_ascii_lowercase();
        term.ends_with("test") || term == "silence"
    }) {
        return None;
    }
    let title = terms
        .into_iter()
        .skip_while(|term| matches!(*term, "PU" | "SC" | "Cine" | "DL" | "Location"))
        .collect::<Vec<_>>()
        .join(" ");
    (!title.is_empty()).then_some(title)
}

fn spans_segment(clip: &TrackPlaylistItem, duration_ms: f64) -> bool {
    clip.track_id == 0
        && clip.event_id == 0
        && duration_ms.is_finite()
        && duration_ms > 0.0
        && clip.play_at.abs() <= TIMELINE_EPSILON_MS
        && clip.begin_trim_offset.abs() <= TIMELINE_EPSILON_MS
        && clip.end_trim_offset.abs() <= TIMELINE_EPSILON_MS
        && (clip.src_duration - duration_ms).abs() <= TIMELINE_EPSILON_MS
}

fn bank_cues(bank: &LoadedBank) -> impl Iterator<Item = MusicCue> + '_ {
    bank.hierarchy.objects().filter_map(move |object| {
        let HircObject::MusicSegment(segment) = object else {
            return None;
        };
        let [track_id] = segment.music_params.children.as_slice() else {
            return None;
        };
        let HircObject::MusicTrack(track) = bank.hierarchy.get(*track_id)? else {
            return None;
        };
        let ([source], [clip]) = (track.sources.as_slice(), track.playlist.as_slice()) else {
            return None;
        };
        // Wwise track type 0 is Normal. Select full sources that can play independently
        // of the interactive soundtrack's transition graph.
        if track.track_type != 0
            || track.num_sub_track != Some(1)
            || !source.is_codec()
            || source.media_id != clip.source_id
            || !spans_segment(clip, segment.duration)
        {
            return None;
        }
        let mut names = segment
            .markers
            .iter()
            .filter_map(|marker| cue_title(&marker.name).map(|title| (&marker.name, title)));
        let (name, title) = names.next()?;
        if names.any(|(other, _)| other != name) {
            return None;
        }
        Some(MusicCue {
            id: format!("{}:{}", bank.name, segment.id),
            title,
            authored_name: name.clone(),
            bank: bank.name.clone(),
            segment_id: segment.id,
            media_id: source.media_id,
            seconds: segment.duration / 1000.0,
        })
    })
}

impl Session {
    /// Retrieve the soundtrack cues with authored names and one complete playable source.
    pub fn music_catalogue(&self) -> Result<&[MusicCue]> {
        let cues = self.music_cues.get_or_try_init(|| {
            let p4k = &self.loaded()?.p4k;
            let audio = AudioLibrary::with_atl(p4k, AtlIndex::default());
            let mut cues = Vec::new();
            for entry in p4k.entries() {
                let name = entry.name.to_ascii_lowercase();
                let Some(bank_name) = name
                    .strip_prefix(BANK_DIR)
                    .filter(|file| file.starts_with(SCORE_BANK_PREFIX) && file.ends_with(".bnk"))
                else {
                    continue;
                };
                if let Some(bank) = audio.bank(bank_name) {
                    cues.extend(
                        bank_cues(&bank)
                            .filter(|cue| audio.media_exists(cue.media_id, Some(&bank))),
                    );
                }
            }
            cues.sort_by(|a, b| {
                a.title
                    .to_ascii_lowercase()
                    .cmp(&b.title.to_ascii_lowercase())
                    .then_with(|| a.authored_name.cmp(&b.authored_name))
                    .then(a.media_id.cmp(&b.media_id))
                    .then(a.id.cmp(&b.id))
            });
            cues.dedup_by(|a, b| a.authored_name == b.authored_name && a.media_id == b.media_id);
            Ok::<_, anyhow::Error>(cues)
        })?;
        Ok(cues)
    }

    /// Fetch and convert the selected cue only. The Ogg belongs to the caller and stays in memory.
    pub fn music_read(&self, cue_id: &str) -> Result<Vec<u8>> {
        let cue = self
            .music_catalogue()?
            .iter()
            .find(|cue| cue.id == cue_id)
            .with_context(|| format!("Unknown music cue {cue_id}"))?;
        let p4k = &self.loaded()?.p4k;
        let audio = AudioLibrary::with_atl(p4k, AtlIndex::default());
        let bank = audio.bank(&cue.bank);
        let (wem, _) = audio
            .media(cue.media_id, bank.as_deref(), false)
            .with_context(|| format!("Music media {} is missing", cue.media_id))?;
        starbreaker_wem::vorbis::wem_to_ogg(&wem)
            .with_context(|| format!("Decoding music media {}", cue.media_id))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn music_cue_names_keep_authored_words() {
        assert_eq!(
            cue_title("MXGS_PU_Cine_Location_Testwell_Orbit").as_deref(),
            Some("Testwell Orbit")
        );
        assert!(cue_title("MX_PU_Cine_ProjectTEST_A").is_none());
        assert!(cue_title("MX_PU_Cine_Finite_Silence").is_none());
        assert!(cue_title("_Entry").is_none());
    }

    #[test]
    fn music_player_excludes_partial_or_event_only_sources() {
        let mut clip = TrackPlaylistItem {
            track_id: 0,
            source_id: 42,
            cache_id: 0,
            event_id: 0,
            play_at: 0.0,
            begin_trim_offset: 0.0,
            end_trim_offset: 0.0,
            src_duration: 10_000.0,
        };
        assert!(spans_segment(&clip, 10_000.001));
        assert!(!spans_segment(&clip, 9_000.0));
        clip.begin_trim_offset = 100.0;
        assert!(!spans_segment(&clip, 10_000.0));
        clip.begin_trim_offset = 0.0;
        clip.event_id = 12;
        assert!(!spans_segment(&clip, 10_000.0));
    }
}
