//! Ogg Vorbis → PCM streaming for the host's sound waves (WP-A1): a package's media stay Ogg until a voice plays them.
//!
//! `sc_audio_open` copies the Ogg bytes into a stream and reports channels and rate; `sc_audio_read` decodes the next
//! interleaved `f32` frames (−1..1) on demand; `sc_audio_rewind` restarts the stream (a looping voice);
//! `sc_audio_close` frees it. Streams are independent of sessions and may be used from any one thread at a time.
use crate::types::sc_status;
use lewton::inside_ogg::OggStreamReader;
use std::io::Cursor;
use std::sync::Arc;

/// One decoding stream. The Ogg bytes are shared between the stream and its reader, so a rewind (a new reader over
/// the same bytes) copies nothing.
pub struct sc_audio_stream {
    bytes: Arc<[u8]>,
    reader: OggStreamReader<Cursor<Arc<[u8]>>>,
    /// Decoded samples not yet handed out (interleaved).
    pending: Vec<f32>,
    pending_at: usize,
    channels: usize,
    ended: bool,
}

impl sc_audio_stream {
    fn new(bytes: Arc<[u8]>) -> Result<Self, String> {
        let reader = OggStreamReader::new(Cursor::new(Arc::clone(&bytes))).map_err(|e| format!("ogg: {e}"))?;
        let channels = usize::from(reader.ident_hdr.audio_channels);
        Ok(Self { bytes, reader, pending: Vec::new(), pending_at: 0, channels, ended: false })
    }

    fn rewind(&mut self) -> Result<(), String> {
        *self = Self::new(Arc::clone(&self.bytes))?;
        Ok(())
    }

    /// Fill `out` (interleaved, whole frames) and return the frames written; 0 at the end of the stream.
    fn read(&mut self, out: &mut [f32]) -> Result<usize, String> {
        let want = out.len() / self.channels.max(1) * self.channels.max(1);
        let mut written = 0;
        while written < want {
            if self.pending_at < self.pending.len() {
                let n = (self.pending.len() - self.pending_at).min(want - written);
                out[written..written + n].copy_from_slice(&self.pending[self.pending_at..self.pending_at + n]);
                self.pending_at += n;
                written += n;
                continue;
            }
            if self.ended {
                break;
            }
            match self.reader.read_dec_packet_itl().map_err(|e| format!("vorbis: {e}"))? {
                Some(packet) => {
                    self.pending.clear();
                    self.pending.extend(packet.iter().map(|&s| f32::from(s) / 32768.0));
                    self.pending_at = 0;
                }
                None => self.ended = true,
            }
        }
        Ok(written / self.channels.max(1))
    }
}

/// Open a stream over a copy of `len` Ogg Vorbis bytes. On success `*out` owns the stream (close it with
/// `sc_audio_close`) and `*channels` / `*rate` describe it; on failure `*out` is NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_audio_open(
    data: *const u8,
    len: usize,
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
        match sc_audio_stream::new(bytes) {
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

/// Decode up to `max_frames` interleaved `f32` frames into `out` (room for `max_frames × channels` floats). Returns the
/// frames written (0: the stream ended), or −1 on a decoding error or bad argument.
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

/// Restart the stream from its first sample.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_audio_rewind(stream: *mut sc_audio_stream) -> sc_status {
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| unsafe {
        match stream.as_mut() {
            Some(s) => s.rewind().map_or(sc_status::SC_STATUS_ERROR, |()| sc_status::SC_STATUS_OK),
            None => sc_status::SC_STATUS_INVALID_ARGUMENT,
        }
    }))
    .unwrap_or(sc_status::SC_STATUS_PANIC)
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
