//! Bus DSP for the host's submix effects (WP-A1 A1-2): the game's Wwise bus effects, from their exported parameters,
//! over interleaved `f32` buffers. Every handle is used by one thread at a time (the host's audio render thread).
//!
//! - [`sc_limiter`] — the Wwise Peak Limiter (`CAkPeakLimiterFXParams`: threshold, ratio, look-ahead, release, output
//!   level, channel link). Its parameter block has no attack: the look-ahead is the time the limiter has to reach a
//!   peak's gain. The detector looks `L` = look-ahead frames ahead: each frame's target gain (the ratio applied above
//!   the threshold) is held as the minimum over the next `L` frames, released toward unity with the release time
//!   constant, and smoothed by an `L`-frame moving average, so the gain ramps down over exactly the look-ahead and has
//!   reached a peak's target when that peak leaves the `L - 1`-frame delay line. The output therefore never exceeds
//!   `threshold * (peak / threshold)^(1 / ratio)`, whatever the buffer size. Channels share one gain when linked; an
//!   LFE channel the effect does not process is left out of the detector and passed at unity, still delayed.
//! - [`sc_meter`] — the Wwise Meter's ballistics (`CAkMeterFXParams`: attack, release, hold, infinite hold, min, max,
//!   peak / RMS): per 10 ms block (carried across calls, so the host's buffer size does not matter), the level in
//!   dBFS; the value rises toward it with the attack time constant, holds for the hold time after its last rise (for
//!   ever with infinite hold), then falls with the release time constant, clamped to [min, max]. This is the value
//!   the meter writes to its game parameter.
//! - [`sc_eq`] — the Wwise Parametric EQ: three bands (low pass, high pass, band pass, notch, low shelf, high shelf,
//!   peaking: `AkFilterType` 0–6) as RBJ biquads (frequency, Q, gain), each on or off, and an output level; an LFE
//!   channel it does not process passes untouched.
//!
//! Constructors validate their parameters (rate 8 kHz–384 kHz, 1–64 channels, finite values, look-ahead up to 1 s)
//! and return NULL otherwise; process calls check the sample count before touching the buffer.
use std::f64::consts::PI;

const MAX_CHANNELS: u32 = 64;

fn valid_format(rate: u32, channels: u32) -> bool {
    (8_000..=384_000).contains(&rate) && (1..=MAX_CHANNELS).contains(&channels)
}

/// `lfe_channel` as an index below `channels`, or None (-1 or out of range).
fn lfe_index(lfe_channel: i32, channels: u32) -> Option<usize> {
    (lfe_channel >= 0 && (lfe_channel as u32) < channels).then_some(lfe_channel as usize)
}

/// A mutable interleaved buffer of `frames` x `channels` samples, or None for NULL or a count that overflows.
unsafe fn frames_mut<'a>(data: *mut f32, frames: usize, channels: usize) -> Option<&'a mut [f32]> {
    let n = frames.checked_mul(channels)?;
    (!data.is_null()).then(|| unsafe { std::slice::from_raw_parts_mut(data, n) })
}

/// One buffer's limiter statistics (dB; -240 for silence).
#[repr(C)]
pub struct sc_limiter_stats {
    /// The lowest gain applied in the buffer (0 when nothing was reduced).
    pub gain_min_db: f32,
    pub input_peak_db: f32,
    pub output_peak_db: f32,
}

fn to_db(x: f64) -> f32 {
    if x <= 1e-12 { -240.0 } else { (20.0 * x.log10()) as f32 }
}

/// A sliding-window minimum over the last `len` values (monotonic deque in a ring of (index, value)).
struct WindowMin {
    len: u64,
    ring: Vec<(u64, f64)>,
    head: usize,
    count: usize,
}

impl WindowMin {
    fn new(len: usize) -> Self {
        Self { len: len as u64, ring: vec![(0, 0.0); len + 1], head: 0, count: 0 }
    }
    fn push(&mut self, index: u64, value: f64) -> f64 {
        let cap = self.ring.len();
        while self.count > 0 {
            let back = (self.head + self.count - 1) % cap;
            if self.ring[back].1 >= value { self.count -= 1 } else { break }
        }
        let at = (self.head + self.count) % cap;
        self.ring[at] = (index, value);
        self.count += 1;
        while self.ring[self.head].0 + self.len <= index {
            self.head = (self.head + 1) % cap;
            self.count -= 1;
        }
        self.ring[self.head].1
    }
}

/// One detector: the gain path shared by its channels.
struct GainPath {
    hold: WindowMin,
    released: f64,
    box_ring: Vec<f64>,
    box_sum: f64,
    box_at: usize,
}

impl GainPath {
    fn new(look: usize) -> Self {
        Self { hold: WindowMin::new(look), released: 1.0, box_ring: vec![1.0; look], box_sum: look as f64, box_at: 0 }
    }
}

pub struct sc_limiter {
    channels: usize,
    look: usize,
    threshold: f64,
    /// 1 - 1 / ratio: the exponent of the gain reduction above the threshold.
    slope: f64,
    release_coeff: f64,
    output: f64,
    link: bool,
    /// The LFE channel left unprocessed (not detected, unity gain, still delayed).
    lfe: Option<usize>,
    paths: Vec<GainPath>,
    /// Delay line, `look - 1` frames per channel (interleaved ring).
    delay: Vec<f32>,
    delay_at: usize,
    frame: u64,
    summed: u64,
    /// Per-path gains of the current frame (allocated once: no allocation on the render thread).
    gains: Vec<f64>,
}

impl sc_limiter {
    #[allow(clippy::too_many_arguments)]
    fn new(rate: u32, channels: u32, threshold_db: f32, ratio: f32, look_ahead_s: f32, release_s: f32, output_db: f32, link: bool, lfe_channel: i32) -> Option<Self> {
        let finite = [threshold_db, ratio, look_ahead_s, release_s, output_db].iter().all(|v| v.is_finite());
        if !valid_format(rate, channels) || !finite || ratio < 1.0 || !(0.0..=1.0).contains(&look_ahead_s) || release_s < 0.0 {
            return None;
        }
        let lfe = lfe_index(lfe_channel, channels);
        let channels = channels as usize;
        let look = ((f64::from(look_ahead_s) * f64::from(rate)).round() as usize).max(1);
        let release_frames = f64::from(release_s.max(0.0)) * f64::from(rate);
        let paths = if link { 1 } else { channels };
        Some(Self {
            channels,
            look,
            threshold: 10f64.powf(f64::from(threshold_db) / 20.0),
            slope: 1.0 - 1.0 / f64::from(ratio),
            release_coeff: if release_frames > 0.0 { 1.0 - (-1.0 / release_frames).exp() } else { 1.0 },
            output: 10f64.powf(f64::from(output_db) / 20.0),
            link,
            lfe,
            paths: (0..paths).map(|_| GainPath::new(look)).collect(),
            delay: vec![0.0; (look - 1).max(1) * channels],
            delay_at: 0,
            frame: 0,
            summed: 0,
            gains: vec![1.0; paths],
        })
    }

    fn target(&self, x: f64) -> f64 {
        if x <= self.threshold { 1.0 } else { (self.threshold / x).powf(self.slope) }
    }

    fn step(path: &mut GainPath, release: f64, look: usize, frame: u64, target: f64) -> f64 {
        let held = path.hold.push(frame, target);
        path.released = if held <= path.released { held } else { path.released + (held - path.released) * release };
        path.box_sum += path.released - path.box_ring[path.box_at];
        path.box_ring[path.box_at] = path.released;
        path.box_at = (path.box_at + 1) % look;
        path.box_sum / look as f64
    }

    fn process(&mut self, data: &mut [f32], stats: &mut sc_limiter_stats) {
        let ch = self.channels;
        let delay_frames = self.look - 1;
        let (mut gain_min, mut in_peak, mut out_peak) = (1.0f64, 0.0f64, 0.0f64);
        let mut gains = std::mem::take(&mut self.gains);
        for frame in data.chunks_exact_mut(ch) {
            if self.link {
                let x = frame.iter().enumerate().filter(|(c, _)| Some(*c) != self.lfe).fold(0f64, |m, (_, v)| m.max(f64::from(v.abs())));
                in_peak = in_peak.max(x);
                let t = self.target(x);
                gains[0] = Self::step(&mut self.paths[0], self.release_coeff, self.look, self.frame, t);
            } else {
                for c in 0..ch {
                    if Some(c) == self.lfe {
                        continue;
                    }
                    let x = f64::from(frame[c].abs());
                    in_peak = in_peak.max(x);
                    let t = self.target(x);
                    gains[c] = Self::step(&mut self.paths[c], self.release_coeff, self.look, self.frame, t);
                }
            }
            for c in 0..ch {
                let g = if Some(c) == self.lfe { 1.0 } else { gains[if self.link { 0 } else { c }] };
                gain_min = gain_min.min(g);
                let delayed = if delay_frames == 0 {
                    frame[c]
                } else {
                    let slot = self.delay_at * ch + c;
                    std::mem::replace(&mut self.delay[slot], frame[c])
                };
                let y = (f64::from(delayed) * g * self.output) as f32;
                out_peak = out_peak.max(f64::from(y.abs()));
                frame[c] = y;
            }
            if delay_frames > 0 {
                self.delay_at = (self.delay_at + 1) % delay_frames;
            }
            self.frame += 1;
            // Re-sum the moving average now and then so rounding cannot drift.
            self.summed += 1;
            if self.summed >= 1 << 20 {
                self.summed = 0;
                for p in &mut self.paths {
                    p.box_sum = p.box_ring.iter().sum();
                }
            }
        }
        self.gains = gains;
        *stats = sc_limiter_stats { gain_min_db: if gain_min >= 1.0 { 0.0 } else { to_db(gain_min) }, input_peak_db: to_db(in_peak), output_peak_db: to_db(out_peak) };
    }
}

/// A limiter for `channels` interleaved channels at `rate`; `lfe_channel` (or -1) is an LFE channel the limiter does
/// not process. NULL for invalid parameters. Free with [`sc_limiter_free`].
#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn sc_limiter_new(rate: u32, channels: u32, threshold_db: f32, ratio: f32, look_ahead_s: f32, release_s: f32, output_db: f32, channel_link: bool,
                                        lfe_channel: i32) -> *mut sc_limiter {
    std::panic::catch_unwind(|| sc_limiter::new(rate, channels, threshold_db, ratio, look_ahead_s, release_s, output_db, channel_link, lfe_channel))
        .ok()
        .flatten()
        .map_or(std::ptr::null_mut(), |l| Box::into_raw(Box::new(l)))
}

/// The limiter's latency in frames (its look-ahead less one: the gain ramp spans the look-ahead and ends on the peak).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_limiter_latency(limiter: *const sc_limiter) -> u32 {
    unsafe { limiter.as_ref() }.map_or(0, |l| (l.look - 1) as u32)
}

/// Limit `frames` interleaved frames in place; `stats` (may be NULL) receives the buffer's statistics.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_limiter_process(limiter: *mut sc_limiter, data: *mut f32, frames: usize, stats: *mut sc_limiter_stats) {
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| unsafe {
        let Some(l) = limiter.as_mut() else { return };
        let Some(buffer) = frames_mut(data, frames, l.channels) else { return };
        let mut s = sc_limiter_stats { gain_min_db: 0.0, input_peak_db: -240.0, output_peak_db: -240.0 };
        l.process(buffer, &mut s);
        if let Some(out) = stats.as_mut() {
            *out = s;
        }
    }));
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_limiter_free(limiter: *mut sc_limiter) {
    if !limiter.is_null() {
        drop(unsafe { Box::from_raw(limiter) });
    }
}

// ------------------------------------------------------------------------------------------------ meter
pub struct sc_meter {
    channels: usize,
    /// Frames per measuring block (10 ms) and that block's duration.
    block: usize,
    dt: f64,
    attack_k: f64,
    release_k: f64,
    hold: f64,
    infinite_hold: bool,
    min: f64,
    max: f64,
    rms: bool,
    /// Added to each block's level (dB): the downstream volume a meter that applies it measures after.
    offset_db: f64,
    value: f64,
    held: f64,
    /// The block being measured: frames so far, peak, sum of squares.
    filled: usize,
    peak: f64,
    sum_sq: f64,
}

impl sc_meter {
    fn close_block(&mut self) {
        let level = if self.rms { (self.sum_sq / (self.block * self.channels) as f64).sqrt() } else { self.peak };
        let level_db = (f64::from(to_db(level)) + self.offset_db).clamp(self.min, self.max);
        if level_db >= self.value {
            self.value += (level_db - self.value) * self.attack_k;
            self.held = 0.0;
        } else if self.infinite_hold {
        } else if self.held + self.dt <= self.hold {
            self.held += self.dt;
        } else {
            self.held += self.dt;
            self.value += (level_db - self.value) * self.release_k;
        }
        self.value = self.value.clamp(self.min, self.max);
        self.filled = 0;
        self.peak = 0.0;
        self.sum_sq = 0.0;
    }
}

/// A meter at `rate` over `channels` interleaved channels: attack, release and hold in seconds, the range in dB,
/// `mode` 0 peak, 1 RMS, `infinite_hold` (the value never falls). It measures in 10 ms blocks carried across calls, so
/// its value does not depend on the host's buffer size; it starts at `min_db`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_meter_new(rate: u32, channels: u32, attack_s: f32, release_s: f32, hold_s: f32, min_db: f32, max_db: f32, mode: u32, infinite_hold: bool) -> *mut sc_meter {
    let finite = [attack_s, release_s, hold_s, min_db, max_db].iter().all(|v| v.is_finite());
    if !valid_format(rate, channels) || !finite || !(min_db <= max_db) {
        return std::ptr::null_mut();
    }
    let block = (rate / 100) as usize;
    let dt = block as f64 / f64::from(rate);
    let k = |tc: f32| if tc > 0.0 { 1.0 - (-dt / f64::from(tc)).exp() } else { 1.0 };
    Box::into_raw(Box::new(sc_meter {
        channels: channels as usize,
        block,
        dt,
        attack_k: k(attack_s),
        release_k: k(release_s),
        hold: f64::from(hold_s.max(0.0)),
        infinite_hold,
        min: f64::from(min_db),
        max: f64::from(max_db),
        rms: mode == 1,
        offset_db: 0.0,
        value: f64::from(min_db),
        held: 0.0,
        filled: 0,
        peak: 0.0,
        sum_sq: 0.0,
    }))
}

/// Measure `frames` interleaved frames (not modified) and return the meter's value (dB) after the last complete block.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_meter_process(meter: *mut sc_meter, data: *const f32, frames: usize) -> f32 {
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| unsafe {
        let Some(m) = meter.as_mut() else { return 0.0 };
        let Some(buffer) = frames_mut(data.cast_mut(), frames, m.channels) else { return m.value as f32 };
        for frame in buffer.chunks_exact(m.channels) {
            for v in frame {
                let x = f64::from(*v);
                m.peak = m.peak.max(x.abs());
                m.sum_sq += x * x;
            }
            m.filled += 1;
            if m.filled == m.block {
                m.close_block();
            }
        }
        m.value as f32
    }))
    .unwrap_or(0.0)
}

/// A gain (dB) added to every measured level from now on: the downstream volume of a meter that applies it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_meter_set_offset(meter: *mut sc_meter, offset_db: f32) {
    if let Some(m) = unsafe { meter.as_mut() } {
        if offset_db.is_finite() {
            m.offset_db = f64::from(offset_db);
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_meter_free(meter: *mut sc_meter) {
    if !meter.is_null() {
        drop(unsafe { Box::from_raw(meter) });
    }
}

// ------------------------------------------------------------------------------------------------ parametric EQ
#[derive(Clone, Copy)]
struct Biquad {
    b: [f64; 3],
    a: [f64; 2],
}

impl Biquad {
    const IDENTITY: Self = Self { b: [1.0, 0.0, 0.0], a: [0.0, 0.0] };

    /// RBJ Audio-EQ-Cookbook coefficients for Wwise `AkFilterType` `kind`.
    fn design(rate: f64, kind: u32, freq: f64, q: f64, gain_db: f64) -> Self {
        let f = freq.clamp(1.0, rate * 0.499);
        let w0 = 2.0 * PI * f / rate;
        let (sin, cos) = w0.sin_cos();
        let alpha = sin / (2.0 * q.max(1e-3));
        let a = 10f64.powf(gain_db / 40.0);
        let (b, a_) = match kind {
            0 => ([(1.0 - cos) / 2.0, 1.0 - cos, (1.0 - cos) / 2.0], [1.0 + alpha, -2.0 * cos, 1.0 - alpha]),
            1 => ([(1.0 + cos) / 2.0, -(1.0 + cos), (1.0 + cos) / 2.0], [1.0 + alpha, -2.0 * cos, 1.0 - alpha]),
            2 => ([alpha, 0.0, -alpha], [1.0 + alpha, -2.0 * cos, 1.0 - alpha]),
            3 => ([1.0, -2.0 * cos, 1.0], [1.0 + alpha, -2.0 * cos, 1.0 - alpha]),
            4 => {
                let s = 2.0 * a.sqrt() * alpha;
                (
                    [a * ((a + 1.0) - (a - 1.0) * cos + s), 2.0 * a * ((a - 1.0) - (a + 1.0) * cos), a * ((a + 1.0) - (a - 1.0) * cos - s)],
                    [(a + 1.0) + (a - 1.0) * cos + s, -2.0 * ((a - 1.0) + (a + 1.0) * cos), (a + 1.0) + (a - 1.0) * cos - s],
                )
            }
            5 => {
                let s = 2.0 * a.sqrt() * alpha;
                (
                    [a * ((a + 1.0) + (a - 1.0) * cos + s), -2.0 * a * ((a - 1.0) + (a + 1.0) * cos), a * ((a + 1.0) + (a - 1.0) * cos - s)],
                    [(a + 1.0) - (a - 1.0) * cos + s, 2.0 * ((a - 1.0) - (a + 1.0) * cos), (a + 1.0) - (a - 1.0) * cos - s],
                )
            }
            6 => ([1.0 + alpha * a, -2.0 * cos, 1.0 - alpha * a], [1.0 + alpha / a, -2.0 * cos, 1.0 - alpha / a]),
            _ => return Self::IDENTITY,
        };
        Self { b: [b[0] / a_[0], b[1] / a_[0], b[2] / a_[0]], a: [a_[1] / a_[0], a_[2] / a_[0]] }
    }
}

pub struct sc_eq {
    rate: f64,
    channels: usize,
    /// The LFE channel left unfiltered.
    lfe: Option<usize>,
    bands: [Option<Biquad>; 3],
    output: f64,
    /// Transposed direct form II state per band and channel.
    state: Vec<[f64; 2]>,
}

/// A three-band parametric EQ (all bands off, output 0 dB) over `channels` interleaved channels; `lfe_channel` (or
/// -1) is an LFE channel the EQ does not process. NULL for an invalid format.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_eq_new(rate: u32, channels: u32, lfe_channel: i32) -> *mut sc_eq {
    if !valid_format(rate, channels) {
        return std::ptr::null_mut();
    }
    let lfe = lfe_index(lfe_channel, channels);
    Box::into_raw(Box::new(sc_eq { rate: f64::from(rate), channels: channels as usize, lfe, bands: [None; 3], output: 1.0, state: vec![[0.0; 2]; 3 * channels as usize] }))
}

/// Set band `band` (0–2): Wwise filter type, frequency (Hz), Q, gain (dB, shelves and peaking), on. A band keeps
/// its filter state across changes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_eq_set_band(eq: *mut sc_eq, band: u32, kind: u32, frequency_hz: f32, q: f32, gain_db: f32, on: bool) {
    let Some(e) = (unsafe { eq.as_mut() }) else { return };
    if ![frequency_hz, q, gain_db].iter().all(|v| v.is_finite()) {
        return;
    }
    let Some(slot) = e.bands.get_mut(band as usize) else { return };
    *slot = on.then(|| Biquad::design(e.rate, kind, f64::from(frequency_hz), f64::from(q), f64::from(gain_db)));
}

/// The EQ's output level (dB).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_eq_set_output(eq: *mut sc_eq, output_db: f32) {
    if let Some(e) = unsafe { eq.as_mut() } {
        if !output_db.is_finite() {
            return;
        }
        e.output = 10f64.powf(f64::from(output_db) / 20.0);
    }
}

/// Filter `frames` interleaved frames in place.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_eq_process(eq: *mut sc_eq, data: *mut f32, frames: usize) {
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| unsafe {
        let Some(e) = eq.as_mut() else { return };
        let ch = e.channels;
        let Some(buffer) = frames_mut(data, frames, ch) else { return };
        for frame in buffer.chunks_exact_mut(ch) {
            for (c, sample) in frame.iter_mut().enumerate() {
                if Some(c) == e.lfe {
                    continue;
                }
                let mut x = f64::from(*sample);
                for (b, band) in e.bands.iter().enumerate() {
                    let Some(f) = band else { continue };
                    let s = &mut e.state[b * ch + c];
                    let y = f.b[0] * x + s[0];
                    s[0] = f.b[1] * x - f.a[0] * y + s[1];
                    s[1] = f.b[2] * x - f.a[1] * y;
                    x = y;
                }
                *sample = (x * e.output) as f32;
            }
        }
    }));
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn sc_eq_free(eq: *mut sc_eq) {
    if !eq.is_null() {
        drop(unsafe { Box::from_raw(eq) });
    }
}

#[cfg(test)]
#[path = "audio_dsp_tests.rs"]
mod tests;
