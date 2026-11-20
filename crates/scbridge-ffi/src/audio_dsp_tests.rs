//! Bus DSP through the C ABI, on synthetic signals: the Peak Limiter holds its ceiling (threshold, ratio) on steps
//! and sharp transients with its look-ahead as the attack, passes quiet audio untouched after its latency, recovers
//! over its release and links or separates channels as authored; the Meter's ballistics (attack, hold, release,
//! range); the Parametric EQ bands' responses at their defining frequencies.
use super::*;

const RATE: u32 = 48_000;

fn db(x: f32) -> f32 {
    20.0 * x.abs().max(1e-12).log10()
}

fn limiter(threshold_db: f32, ratio: f32, look_ahead_s: f32, release_s: f32, link: bool, channels: u32) -> *mut sc_limiter {
    let l = unsafe { sc_limiter_new(RATE, channels, threshold_db, ratio, look_ahead_s, release_s, 0.0, link, -1) };
    assert!(!l.is_null());
    l
}

fn run(l: *mut sc_limiter, data: &mut [f32], channels: usize, block: usize) -> sc_limiter_stats {
    let mut all = sc_limiter_stats { gain_min_db: 0.0, input_peak_db: -240.0, output_peak_db: -240.0 };
    for chunk in data.chunks_mut(block * channels) {
        let mut s = sc_limiter_stats { gain_min_db: 0.0, input_peak_db: 0.0, output_peak_db: 0.0 };
        unsafe { sc_limiter_process(l, chunk.as_mut_ptr(), chunk.len() / channels, &mut s) };
        all.gain_min_db = all.gain_min_db.min(s.gain_min_db);
        all.input_peak_db = all.input_peak_db.max(s.input_peak_db);
        all.output_peak_db = all.output_peak_db.max(s.output_peak_db);
    }
    all
}

/// The ceiling a ratio limiter allows for an input peak `x` (linear) over `thr` (linear).
fn ceiling(thr: f32, ratio: f32, x: f32) -> f32 {
    if x <= thr { x } else { thr * (x / thr).powf(1.0 / ratio) }
}

#[test]
fn a_step_twenty_db_over_the_threshold_is_held_at_the_ratio_ceiling_from_its_first_sample() {
    // The bug this replaces: an attack-smoothed envelope lets a transient through until it catches up (the UE port
    // put 72 % of its output blocks over the threshold). Silence, then a 1 kHz sine at +20 dB over -0.1 dBFS.
    let (thr_db, ratio) = (-0.1, 40.0);
    let thr = 10f32.powf(thr_db / 20.0);
    let amp = thr * 10.0;
    let mut x: Vec<f32> = (0..RATE as usize).map(|n| if n < 4800 { 0.0 } else { amp * (2.0 * std::f32::consts::PI * 1000.0 * n as f32 / RATE as f32).sin() }).collect();
    let l = limiter(thr_db, ratio, 0.01, 0.05, true, 1);
    let s = run(l, &mut x, 1, 512);
    let bound = ceiling(thr, ratio, amp) * 1.000_01;
    let peak = x.iter().fold(0f32, |m, v| m.max(v.abs()));
    assert!(peak <= bound, "peak {} dB over the ceiling {} dB", db(peak), db(bound));
    assert!((s.output_peak_db - db(peak)).abs() < 1e-3);
    assert!(s.gain_min_db < -19.0, "reduced by about 20 dB: {}", s.gain_min_db);
    unsafe { sc_limiter_free(l) };
}

#[test]
fn sharp_transients_over_a_loud_bed_never_pass_the_ceiling_whatever_the_block_size() {
    // A door-like click train (one-sample attack, 30 ms decay) over a bed at the threshold, both channels.
    let (thr_db, ratio) = (-0.1, 40.0);
    let thr = 10f32.powf(thr_db / 20.0);
    let frames = RATE as usize * 2;
    let mut seed = 0x1234_5678u32;
    let mut noise = || {
        seed = seed.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
        (seed >> 8) as f32 / (1u32 << 24) as f32 * 2.0 - 1.0
    };
    let mut x = vec![0f32; frames * 2];
    let mut loudest = 0f32;
    for n in 0..frames {
        let t = n % 9_600;
        let click = if t < 1_440 { 6.0 * (-(t as f32) / 300.0).exp() } else { 0.0 };
        for c in 0..2 {
            let v = thr * (0.9 * noise() + click * noise().signum());
            x[n * 2 + c] = v;
            loudest = loudest.max(v.abs());
        }
    }
    for block in [64, 480, 1024] {
        let mut y = x.clone();
        let l = limiter(thr_db, ratio, 0.01, 0.05, true, 2);
        run(l, &mut y, 2, block);
        let peak = y.iter().fold(0f32, |m, v| m.max(v.abs()));
        assert!(peak <= ceiling(thr, ratio, loudest) * 1.000_01, "block {block}: {} dB", db(peak));
        unsafe { sc_limiter_free(l) };
    }
}

#[test]
fn audio_under_the_threshold_passes_unchanged_after_the_look_ahead() {
    let l = limiter(-0.1, 40.0, 0.01, 0.05, true, 1);
    assert_eq!(unsafe { sc_limiter_latency(l) }, 479, "the look-ahead less one frame: a peak leaves the delay line as the ramp ends");
    let input: Vec<f32> = (0..4800).map(|n| 0.5 * (n as f32 * 0.01).sin()).collect();
    let mut y = input.clone();
    let s = run(l, &mut y, 1, 256);
    assert_eq!(s.gain_min_db, 0.0);
    for n in 479..4800 {
        assert!((y[n] - input[n - 479]).abs() < 1e-6, "frame {n}");
    }
    assert!(y[..479].iter().all(|v| *v == 0.0), "the delay line starts silent");
    unsafe { sc_limiter_free(l) };
}

#[test]
fn the_gain_recovers_over_the_release_time() {
    // A burst 12 dB over, then quiet: 1 - 1/e of the way back after one release time constant, all the way after six.
    let (thr_db, release) = (-6.0, 0.1);
    let thr = 10f32.powf(thr_db / 20.0);
    let l = limiter(thr_db, 1000.0, 0.005, release, true, 1);
    let mut burst = vec![thr * 4.0; 4800];
    run(l, &mut burst, 1, 480);
    let quiet_level = thr * 0.5;
    let gain_after = |frames: usize| {
        let mut q = vec![quiet_level; frames];
        run(l, &mut q, 1, 480);
        q[frames - 1] / quiet_level
    };
    let start = 0.25f32; // -12 dB of reduction
    let g1 = gain_after((release * RATE as f32) as usize);
    let expected = start + (1.0 - start) * (1.0 - (-1f32).exp());
    assert!((g1 - expected).abs() < 0.05, "after one release time {g1} (expected about {expected})");
    let g6 = gain_after((5.0 * release * RATE as f32) as usize);
    assert!(g6 > 0.99, "recovered: {g6}");
    unsafe { sc_limiter_free(l) };
}

#[test]
fn unlinked_channels_limit_independently_and_linked_ones_together() {
    let thr = 10f32.powf(-6.0 / 20.0);
    let make = || {
        let mut x = vec![0f32; 9600 * 2];
        for n in 0..9600 {
            x[n * 2] = thr * 4.0; // left over
            x[n * 2 + 1] = thr * 0.5; // right under
        }
        x
    };
    let (mut a, mut b) = (make(), make());
    let unlinked = limiter(-6.0, 1000.0, 0.005, 0.05, false, 2);
    run(unlinked, &mut a, 2, 480);
    let linked = limiter(-6.0, 1000.0, 0.005, 0.05, true, 2);
    run(linked, &mut b, 2, 480);
    assert!((a[9599 * 2 + 1] - thr * 0.5).abs() < 1e-5, "the quiet channel is untouched when unlinked");
    assert!(b[9599 * 2 + 1] < thr * 0.2, "and reduced with the loud one when linked");
    unsafe {
        sc_limiter_free(unlinked);
        sc_limiter_free(linked);
    }
}

#[test]
fn the_meter_rises_over_its_attack_holds_then_falls_over_its_release_within_its_range() {
    // Attack 0 follows at once; hold 0.2 s keeps the value; release 0.1 s falls by 1 - 1/e per time constant.
    let m = unsafe { sc_meter_new(RATE, 1, 0.0, 0.1, 0.2, -60.0, 0.0, 0, false) };
    let mut loud = vec![0.5f32; 4800];
    let v = unsafe { sc_meter_process(m, loud.as_mut_ptr(), 4800) };
    assert!((v - db(0.5)).abs() < 1e-3, "peak at once: {v}");
    let mut silence = vec![0f32; 4800];
    let held = unsafe { sc_meter_process(m, silence.as_mut_ptr(), 4800) };
    assert!((held - db(0.5)).abs() < 1e-3, "held for 0.1 s of the 0.2 s hold: {held}");
    let mut more = vec![0f32; 4800 * 3];
    let mut last = held;
    for chunk in more.chunks_mut(480) {
        last = unsafe { sc_meter_process(m, chunk.as_mut_ptr(), 480) };
    }
    assert!(last < held - 10.0 && last >= -60.0, "released toward the floor: {last}");
    let mut long = vec![0f32; RATE as usize * 2];
    let floor = unsafe { sc_meter_process(m, long.as_mut_ptr(), long.len()) };
    assert_eq!(floor, -60.0, "clamped to the meter's minimum");
    unsafe { sc_meter_free(m) };

    // A 1 s attack reaches 1 - 1/e of the step in 1 s (in dB); RMS mode reads a sine 3 dB under its peak.
    let slow = unsafe { sc_meter_new(RATE, 1, 1.0, 1.0, 0.0, -60.0, 0.0, 1, false) };
    let mut sine: Vec<f32> = (0..RATE as usize).map(|n| (2.0 * std::f32::consts::PI * 100.0 * n as f32 / RATE as f32).sin()).collect();
    let mut v = 0.0;
    for chunk in sine.chunks_mut(480) {
        v = unsafe { sc_meter_process(slow, chunk.as_mut_ptr(), 480) };
    }
    let target = -3.0103f32;
    let expected = -60.0 + (target + 60.0) * (1.0 - (-1f32).exp());
    assert!((v - expected).abs() < 1.0, "{v} vs {expected}");
    unsafe { sc_meter_free(slow) };
}

/// The magnitude response of the EQ at `freq` (steady state of a sine through it).
fn response_db(eq: *mut sc_eq, freq: f32) -> f32 {
    let n = RATE as usize;
    let mut x: Vec<f32> = (0..n).map(|i| (2.0 * std::f32::consts::PI * freq * i as f32 / RATE as f32).sin()).collect();
    unsafe { sc_eq_process(eq, x.as_mut_ptr(), n) };
    db(x[n / 2..].iter().fold(0f32, |m, v| m.max(v.abs())))
}

#[test]
fn eq_bands_shape_the_response_at_their_frequencies() {
    let cases: [(u32, f32, f32, f32, f32, f32); 6] = [
        // (Wwise filter type, frequency, Q, gain dB, probe Hz, expected dB)
        (0, 400.0, 0.7071, 0.0, 400.0, -3.01),  // lowpass: -3 dB at the cutoff (Butterworth Q)
        (0, 400.0, 0.7071, 0.0, 4000.0, -40.0), // and 12 dB / octave above it
        (1, 90.0, 0.7071, 0.0, 90.0, -3.01),    // highpass
        (6, 500.0, 6.8, -3.0, 500.0, -3.0),     // peaking: its gain at its centre
        (4, 390.0, 0.7071, -5.0, 30.0, -5.0),   // low shelf: its gain well below
        (5, 5000.0, 0.7071, 6.0, 18000.0, 6.0), // high shelf: its gain well above
    ];
    for (kind, freq, q, gain, probe, want) in cases {
        let eq = unsafe { sc_eq_new(RATE, 1, -1) };
        unsafe { sc_eq_set_band(eq, 0, kind, freq, q, gain, true) };
        let got = response_db(eq, probe);
        let tol = if want <= -30.0 { 1.5 } else { 0.25 };
        assert!((got - want).abs() < tol, "type {kind} at {probe} Hz: {got} dB, expected {want}");
        unsafe { sc_eq_free(eq) };
    }
    // A band that is off, and the output level.
    let eq = unsafe { sc_eq_new(RATE, 1, -1) };
    unsafe {
        sc_eq_set_band(eq, 2, 0, 400.0, 1.0, 0.0, false);
        sc_eq_set_output(eq, -6.0);
    }
    assert!((response_db(eq, 4000.0) - -6.0).abs() < 0.05);
    unsafe { sc_eq_free(eq) };
}

/// The same audio gives the same meter value whatever the host's buffer size, including a lone transient (the meter
/// measures in fixed 10 ms blocks carried across calls, not per call).
#[test]
fn the_meter_does_not_depend_on_the_buffer_size() {
    let mut pcm = vec![0f32; RATE as usize];
    pcm[1000] = 1.0; // one full-scale sample
    for (n, v) in pcm.iter_mut().enumerate().skip(24_000) {
        *v = 0.3 * (n as f32 * 0.05).sin();
    }
    let run_with = |block: usize, end: usize| {
        let m = unsafe { sc_meter_new(RATE, 1, 0.02, 0.15, 0.16, -60.0, 0.0, 0, false) };
        let mut v = 0.0;
        for chunk in pcm[..end].chunks(block) {
            v = unsafe { sc_meter_process(m, chunk.as_ptr(), chunk.len()) };
        }
        unsafe { sc_meter_free(m) };
        v
    };
    for end in [1_440, 4_800, 30_000, 48_000] {
        let reference = run_with(480, end);
        for block in [1, 64, 333, 1024] {
            assert!((run_with(block, end) - reference).abs() < 1e-4, "block {block} at {end}: {} vs {reference}", run_with(block, end));
        }
    }
}

#[test]
fn an_infinite_hold_meter_never_falls() {
    let m = unsafe { sc_meter_new(RATE, 1, 0.0, 0.1, 0.0, -60.0, 0.0, 0, true) };
    let loud = vec![0.5f32; 4800];
    let quiet = vec![0f32; 48_000];
    unsafe {
        let a = sc_meter_process(m, loud.as_ptr(), loud.len());
        let b = sc_meter_process(m, quiet.as_ptr(), quiet.len());
        assert_eq!(a, b);
        sc_meter_free(m);
    }
}

/// Invalid parameters are refused at construction (no panic across the C ABI), and an LFE channel the effect does not
/// process passes the EQ untouched and the limiter at unity, still delayed with the others.
#[test]
fn invalid_parameters_are_refused_and_the_lfe_channel_is_left_alone() {
    unsafe {
        assert!(sc_eq_new(2, 1, -1).is_null(), "a 2 Hz rate");
        assert!(sc_eq_new(RATE, 0, -1).is_null());
        assert!(sc_limiter_new(RATE, 2, -0.1, 40.0, 30.0, 0.05, 0.0, true, -1).is_null(), "a 30 s look-ahead");
        assert!(sc_limiter_new(RATE, 2, f32::NAN, 40.0, 0.01, 0.05, 0.0, true, -1).is_null());
        assert!(sc_meter_new(RATE, 1, f32::INFINITY, 0.1, 0.0, -60.0, 0.0, 0, false).is_null());
        let eq = sc_eq_new(RATE, 6, 3);
        sc_eq_set_band(eq, 0, 0, 100.0, 0.7071, 0.0, true);
        let mut x: Vec<f32> = (0..4800 * 6).map(|i| if i % 6 == 3 { 0.25 } else { ((i / 6) as f32 * 0.9).sin() }).collect();
        sc_eq_process(eq, x.as_mut_ptr(), 4800);
        assert!((0..4800).all(|f| x[f * 6 + 3] == 0.25), "LFE unfiltered");
        assert!(x[4799 * 6].abs() < 0.05, "the others low-passed");
        sc_eq_free(eq);
        let l = sc_limiter_new(RATE, 6, -6.0, 1000.0, 0.005, 0.05, 0.0, true, 3);
        let mut y: Vec<f32> = (0..4800 * 6).map(|i| if i % 6 == 3 { 4.0 } else { 0.1 }).collect();
        sc_limiter_process(l, y.as_mut_ptr(), 4800, std::ptr::null_mut());
        assert_eq!(y[4799 * 6 + 3], 4.0, "LFE at unity");
        assert!((y[4799 * 6] - 0.1).abs() < 1e-6, "a loud LFE does not pull the mains down");
        assert_eq!(y[6 + 3], 0.0, "LFE delayed with the others");
        sc_limiter_free(l);
    }
}

#[test]
fn a_meter_offset_shifts_the_measured_level() {
    unsafe {
        let m = sc_meter_new(RATE, 1, 0.0, 0.1, 0.0, -60.0, 0.0, 0, false);
        sc_meter_set_offset(m, -10.0);
        let x = vec![0.5f32; 480];
        let v = sc_meter_process(m, x.as_ptr(), 480);
        assert!((v - (-6.0206 - 10.0)).abs() < 1e-3, "{v}");
        sc_meter_free(m);
    }
}
