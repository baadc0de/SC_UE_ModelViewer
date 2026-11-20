//! Ogg Vorbis → PCM streaming for the host's sound waves (WP-A1): a package's media stay Ogg until a voice plays them.
//!
//! `sc_audio_open` copies the Ogg bytes into a stream for a number of passes and reports channels and rate;
//! `sc_audio_read` decodes the next interleaved `f32` frames (lewton's float output, no 16-bit step) on demand, looping as Wwise does;
//! `sc_audio_loop_region` reports the loop region; `sc_audio_close` frees the stream. Streams are independent of
//! sessions and may be used from any one thread at a time.
//!
//! Looping follows the media's loop points, which the WEM → Ogg conversion carries as the `LoopStart` / `LoopEnd`
//! comments (ww2ogg: the `smpl` chunk's loop, `LoopEnd` exclusive): the first pass plays from the start up to
//! `LoopEnd`, every further pass `LoopStart..LoopEnd`, and the last pass of a finite count plays on into the tail
//! after `LoopEnd`. A media without loop points loops whole. Wrapping seeks to the page before `LoopStart` and
//! decodes up to it, so every pass continues sample for sample (Vorbis packets overlap only their neighbours).
use crate::types::sc_status;
use lewton::inside_ogg::OggStreamReader;
use lewton::samples::InterleavedSamples;
use std::io::Cursor;
use std::sync::Arc;

/// How far before `LoopStart` a wrap seeks (two long blocks), so the packet that first yields `LoopStart` is decoded
/// with its predecessor.
const SEEK_LEAD: u64 = 4096;

/// One decoding stream. The Ogg bytes are shared between the stream and its reader, so a wrap (a seek, or a new
/// reader over the same bytes) copies nothing.
pub struct sc_audio_stream {
    bytes: Arc<[u8]>,
    reader: OggStreamReader<Cursor<Arc<[u8]>>>,
    /// Decoded samples not yet handed out (interleaved); `pending_pos` is the media frame of `pending[pending_at]`.
    pending: Vec<f32>,
    pending_at: usize,
    pending_pos: u64,
    channels: usize,
    /// Total frames of the media (the last page's granule position).
    frames: u64,
    loop_start: u64,
    loop_end: u64,
    /// Passes after the current one; None = forever.
    passes_left: Option<u32>,
    ended: bool,
    /// Wraps that had to decode from the start because the seek could not place `LoopStart` (tests expect none).
    restarts: u32,
}

/// The granule position of the last Ogg page in `bytes`: the stream's length in frames (one logical stream).
fn last_granule(bytes: &[u8]) -> Option<u64> {
    let mut at = bytes.len().checked_sub(27)?;
    loop {
        if &bytes[at..at + 4] == b"OggS" && bytes[at + 4] == 0 {
            let g = u64::from_le_bytes(bytes[at + 6..at + 14].try_into().ok()?);
            if g != u64::MAX {
                return Some(g);
            }
        }
        at = at.checked_sub(1)?;
    }
}

impl sc_audio_stream {
    fn new(bytes: Arc<[u8]>, loops: u32) -> Result<Self, String> {
        let reader = OggStreamReader::new(Cursor::new(Arc::clone(&bytes))).map_err(|e| format!("ogg: {e}"))?;
        let channels = usize::from(reader.ident_hdr.audio_channels);
        let frames = last_granule(&bytes).ok_or("ogg: no granule position")?;
        let comment = |key: &str| {
            reader.comment_hdr.comment_list.iter().find(|(k, _)| k.eq_ignore_ascii_case(key)).and_then(|(_, v)| v.trim().parse::<u64>().ok())
        };
        let (mut loop_start, mut loop_end) = (0, frames);
        if let (Some(s), Some(e)) = (comment("LoopStart"), comment("LoopEnd")) {
            let e = e.min(frames);
            if s < e {
                (loop_start, loop_end) = (s, e);
            }
        }
        Ok(Self {
            bytes,
            reader,
            pending: Vec::new(),
            pending_at: 0,
            pending_pos: 0,
            channels,
            frames,
            loop_start,
            loop_end,
            passes_left: if loops == 0 { None } else { Some(loops - 1) },
            ended: false,
            restarts: 0,
        })
    }

    fn more_passes(&self) -> bool {
        self.passes_left != Some(0)
    }

    /// Decode the next packet into `pending`; false at the end of the media. `pending_pos` must already be the media
    /// frame of the packet's first sample.
    fn decode(&mut self) -> Result<bool, String> {
        // lewton's own float output (no 16-bit rounding or clipping on the way), taken over without a copy.
        match self.reader.read_dec_packet_generic::<InterleavedSamples<f32>>().map_err(|e| format!("vorbis: {e}"))? {
            Some(packet) => {
                self.pending = packet.samples;
                self.pending_at = 0;
                Ok(true)
            }
            None => Ok(false),
        }
    }

    /// Continue the next pass at `loop_start`: seek to the page before it and decode up to it (or decode from the
    /// start when the seek cannot place it).
    fn wrap(&mut self) -> Result<(), String> {
        if let Some(p) = self.passes_left.as_mut() {
            *p -= 1;
        }
        self.pending.clear();
        self.pending_at = 0;
        if self.loop_start > 0 {
            if self.reader.seek_absgp_pg(self.loop_start.saturating_sub(SEEK_LEAD)).is_ok() {
                // After a seek each packet's start is its page's granule (one packet per page) less its length.
                while self.decode()? {
                    let len = (self.pending.len() / self.channels.max(1)) as u64;
                    let Some(end) = self.reader.get_last_absgp() else { break };
                    let start = end.saturating_sub(len);
                    if len == 0 || end <= self.loop_start {
                        continue;
                    }
                    if start > self.loop_start {
                        break; // the seek landed past the loop start: decode from the beginning instead
                    }
                    self.pending_at = (self.loop_start - start) as usize * self.channels;
                    self.pending_pos = self.loop_start;
                    return Ok(());
                }
            }
        }
        if self.loop_start > 0 {
            self.restarts += 1;
        }
        self.reader = OggStreamReader::new(Cursor::new(Arc::clone(&self.bytes))).map_err(|e| format!("ogg: {e}"))?;
        self.pending.clear();
        self.pending_at = 0;
        self.pending_pos = 0;
        while self.pending_pos < self.loop_start {
            if !self.decode()? {
                return Err("ogg: the loop start lies past the end of the media".into());
            }
            let len = (self.pending.len() / self.channels.max(1)) as u64;
            if self.pending_pos + len > self.loop_start {
                self.pending_at = (self.loop_start - self.pending_pos) as usize * self.channels;
                self.pending_pos = self.loop_start;
                return Ok(());
            }
            self.pending_pos += len;
            self.pending.clear();
        }
        Ok(())
    }

    /// Fill `out` (interleaved, whole frames) and return the frames written; 0 once every pass has played.
    fn read(&mut self, out: &mut [f32]) -> Result<usize, String> {
        let ch = self.channels.max(1);
        let want = out.len() / ch * ch;
        let mut written = 0;
        while written < want && !self.ended {
            // A pass that loops again stops at LoopEnd; the last one plays the media out.
            let limit = if self.more_passes() { self.loop_end } else { u64::MAX };
            if self.pending_pos >= limit {
                self.wrap()?;
                continue;
            }
            if self.pending_at < self.pending.len() {
                let available = (self.pending.len() - self.pending_at) / ch;
                let n = available.min((want - written) / ch).min((limit - self.pending_pos).min(usize::MAX as u64) as usize);
                out[written..written + n * ch].copy_from_slice(&self.pending[self.pending_at..self.pending_at + n * ch]);
                self.pending_at += n * ch;
                self.pending_pos += n as u64;
                written += n * ch;
                continue;
            }
            if !self.decode()? {
                if self.more_passes() && self.pending_pos > self.loop_start {
                    // The media ended before LoopEnd (a short last packet): loop from here.
                    self.loop_end = self.pending_pos;
                    self.wrap()?;
                } else {
                    self.ended = true;
                }
            }
        }
        Ok(written / ch)
    }
}

/// Open a stream over a copy of `len` Ogg Vorbis bytes that plays `loops` passes (0 = forever; see the module notes
/// for the loop region). On success `*out` owns the stream (close it with `sc_audio_close`) and `*channels` / `*rate`
/// describe it; on failure `*out` is NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_audio_open(
    data: *const u8,
    len: usize,
    loops: u32,
    out: *mut *mut sc_audio_stream,
    channels: *mut u32,
    rate: *mut u32,
) -> sc_status {
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| unsafe {
        if out.is_null() || (data.is_null() && len > 0) {
            return sc_status::SC_STATUS_INVALID_ARGUMENT;
        }
        *out = std::ptr::null_mut();
        let bytes: Arc<[u8]> = if len == 0 { Arc::from(Vec::new()) } else { Arc::from(std::slice::from_raw_parts(data, len)) };
        match sc_audio_stream::new(bytes, loops) {
            Ok(stream) => {
                if !channels.is_null() {
                    *channels = stream.channels as u32;
                }
                if !rate.is_null() {
                    *rate = stream.reader.ident_hdr.audio_sample_rate;
                }
                *out = Box::into_raw(Box::new(stream));
                sc_status::SC_STATUS_OK
            }
            Err(_) => sc_status::SC_STATUS_ERROR,
        }
    }))
    .unwrap_or(sc_status::SC_STATUS_PANIC)
}

/// The stream's loop region `[*loop_start, *loop_end)` and the media's length `*frames` (frames). A media without loop
/// points reports `0..frames`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_audio_loop_region(
    stream: *const sc_audio_stream,
    loop_start: *mut u64,
    loop_end: *mut u64,
    frames: *mut u64,
) -> sc_status {
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| unsafe {
        let Some(s) = stream.as_ref() else { return sc_status::SC_STATUS_INVALID_ARGUMENT };
        for (p, v) in [(loop_start, s.loop_start), (loop_end, s.loop_end), (frames, s.frames)] {
            if !p.is_null() {
                *p = v;
            }
        }
        sc_status::SC_STATUS_OK
    }))
    .unwrap_or(sc_status::SC_STATUS_PANIC)
}

/// Decode up to `max_frames` interleaved `f32` frames into `out` (room for `max_frames × channels` floats). Returns the
/// frames written (0: every pass has played), or −1 on a decoding error or bad argument.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_audio_read(stream: *mut sc_audio_stream, out: *mut f32, max_frames: usize) -> i64 {
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| unsafe {
        let Some(stream) = stream.as_mut() else { return -1 };
        if out.is_null() {
            return -1;
        }
        let buffer = std::slice::from_raw_parts_mut(out, max_frames * stream.channels.max(1));
        stream.read(buffer).map(|n| n as i64).unwrap_or(-1)
    }))
    .unwrap_or(-1)
}

/// Free a stream once; NULL is a no-op.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_audio_close(stream: *mut sc_audio_stream) {
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| unsafe {
        if !stream.is_null() {
            drop(Box::from_raw(stream));
        }
    }));
}

#[cfg(test)]
#[path = "audio_tests.rs"]
mod tests;
