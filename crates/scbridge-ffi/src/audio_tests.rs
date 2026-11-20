//! Ogg streaming through the C ABI: argument and format errors, and (when decoded game media from an export are present
//! under `out/audio/`, never in git) a full decode, a partial read, and the Wwise loop semantics: a media's loop region
//! (`LoopStart` / `LoopEnd` comments) repeats sample-accurately, the intro plays once, the tail only after the last pass.
use super::*;
use std::path::PathBuf;

#[test]
fn bad_arguments_and_non_ogg_bytes_are_rejected() {
    let mut stream = std::ptr::null_mut();
    unsafe {
        assert!(matches!(
            sc_audio_open(std::ptr::null(), 4, 1, &mut stream, std::ptr::null_mut(), std::ptr::null_mut()),
            sc_status::SC_STATUS_INVALID_ARGUMENT
        ));
        let junk = b"not an ogg stream at all";
        assert!(matches!(
            sc_audio_open(junk.as_ptr(), junk.len(), 1, &mut stream, std::ptr::null_mut(), std::ptr::null_mut()),
            sc_status::SC_STATUS_ERROR
        ));
        assert!(stream.is_null());
        assert_eq!(sc_audio_read(std::ptr::null_mut(), std::ptr::null_mut(), 16), -1);
        let (mut a, mut b, mut c) = (0u64, 0u64, 0u64);
        assert!(matches!(sc_audio_loop_region(std::ptr::null_mut(), &mut a, &mut b, &mut c), sc_status::SC_STATUS_INVALID_ARGUMENT));
        sc_audio_close(std::ptr::null_mut());
    }
}

/// A decoded game media file from a local export (`out/audio/manifest-test/Audio/<id>.ogg`), or None (test skipped).
fn game_media(id: &str) -> Option<Vec<u8>> {
    let path = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join(format!("../../out/audio/manifest-test/Audio/{id}.ogg"));
    match std::fs::read(&path) {
        Ok(b) => Some(b),
        Err(_) => {
            eprintln!("skip: no decoded game media at {}", path.display());
            None
        }
    }
}

/// The media decoded straight through, once (the reference the stream's passes are compared with).
fn reference(ogg: &[u8]) -> Vec<f32> {
    let mut r = OggStreamReader::new(Cursor::new(ogg.to_vec())).unwrap();
    let mut out = Vec::new();
    while let Some(p) = r.read_dec_packet_generic::<lewton::samples::InterleavedSamples<f32>>().unwrap() {
        out.extend(p.samples);
    }
    out
}

/// Open `ogg` for `loops` passes and read everything it plays (at most `cap` frames), in odd-sized chunks.
fn play(ogg: &[u8], loops: u32, cap: usize) -> (Vec<f32>, u32, (u64, u64, u64)) {
    play_counting(ogg, loops, cap).0
}

/// `play`, and the wraps that decoded from the start instead of seeking.
fn play_counting(ogg: &[u8], loops: u32, cap: usize) -> ((Vec<f32>, u32, (u64, u64, u64)), u32) {
    let (mut stream, mut channels, mut rate) = (std::ptr::null_mut(), 0u32, 0u32);
    let mut out = Vec::new();
    let mut region = (0u64, 0u64, 0u64);
    unsafe {
        assert!(matches!(sc_audio_open(ogg.as_ptr(), ogg.len(), loops, &mut stream, &mut channels, &mut rate), sc_status::SC_STATUS_OK));
        assert!(matches!(sc_audio_loop_region(stream, &mut region.0, &mut region.1, &mut region.2), sc_status::SC_STATUS_OK));
        let ch = channels as usize;
        let mut buf = vec![0f32; 1531 * ch];
        while out.len() / ch < cap {
            let want = 1531.min(cap - out.len() / ch);
            let n = sc_audio_read(stream, buf.as_mut_ptr(), want);
            assert!(n >= 0);
            if n == 0 {
                break;
            }
            out.extend_from_slice(&buf[..n as usize * ch]);
        }
        let restarts = (*stream).restarts;
        sc_audio_close(stream);
        ((out, channels, region), restarts)
    }
}

#[test]
fn decodes_a_game_media_file_once() {
    // The C2 top lift door's open sound (2 ch, 48 kHz, 4.03 s per ATL `wwise_duration`).
    let Some(ogg) = game_media("872222338") else { return };
    let (out, channels, region) = play(&ogg, 1, usize::MAX);
    assert_eq!(channels, 2);
    let frames = out.len() / 2;
    assert!((frames as f32 / 48_000.0 - 4.0308).abs() < 0.05, "{frames} frames");
    assert_eq!(region, (0, frames as u64, frames as u64), "no loop points: the region is the whole media");
    assert_eq!(out, reference(&ogg));
    let peak = out.iter().fold(0f32, |m, s| m.max(s.abs()));
    assert!(peak > 0.01 && peak <= 1.5, "{peak}");
}

#[test]
fn a_looping_media_repeats_its_loop_region_not_the_whole_file() {
    // A C2 ambience loop whose WEM carries loop points (intro 0..LoopStart, loop LoopStart..LoopEnd, 872-frame tail).
    let Some(ogg) = game_media("142743790") else { return };
    let r = reference(&ogg);
    let n = r.len() / 2;
    let (start, end) = (100_988usize, 616_911usize);
    // Forever: the intro and the loop once, then the loop region again from LoopStart, sample for sample.
    let after = 30_000;
    let ((out, _, region), restarts) = play_counting(&ogg, 0, end + (end - start) + after);
    assert_eq!(restarts, 0, "each wrap seeks to LoopStart (no decode from the start)");
    assert_eq!(region, (start as u64, end as u64, n as u64));
    assert_eq!(out.len() / 2, end + (end - start) + after);
    assert!(out[..end * 2] == r[..end * 2], "first pass: the media from its start up to LoopEnd");
    assert!(out[end * 2..(2 * end - start) * 2] == r[start * 2..end * 2], "second pass: LoopStart..LoopEnd");
    assert!(out[(2 * end - start) * 2..] == r[start * 2..(start + after) * 2], "third pass starts at LoopStart again");
    // Two passes: intro, loop, loop, then the tail after LoopEnd.
    let (two, _, _) = play(&ogg, 2, usize::MAX);
    assert_eq!(two.len() / 2, n + (end - start));
    assert!(two[(2 * end - start) * 2..] == r[end * 2..], "the last pass plays on into the tail");
    // One pass: the media as it is.
    let (one, _, _) = play(&ogg, 1, usize::MAX);
    assert!(one == r);
}

#[test]
fn a_media_without_loop_points_loops_whole() {
    // The C2 lift travel loop (no loop points: the whole media is the loop).
    let Some(ogg) = game_media("52843347") else { return };
    let r = reference(&ogg);
    let n = r.len() / 2;
    let (out, _, region) = play(&ogg, 3, usize::MAX);
    assert_eq!(region, (0, n as u64, n as u64));
    assert_eq!(out.len() / 2, 3 * n);
    for pass in 0..3 {
        assert!(out[pass * n * 2..(pass + 1) * n * 2] == r[..], "pass {pass}");
    }
}

/// Seeking to a play time lands where reading straight through would be, across loop passes (Wwise's virtual voices
/// resume "from elapsed time": a voice posted with others plays in step with them however late it becomes audible).
#[test]
fn a_seek_to_a_play_time_matches_reading_through() {
    let Some(ogg) = game_media("142743790") else { return };
    let (start, end) = (100_988u64, 616_911u64);
    // Within the intro, in later loop passes, across the last pass into the tail, in a single pass.
    for (loops, at) in [(0u32, 5_000u64), (0, end + 1_234), (0, end + 3 * (end - start) + 77), (2, end + (end - start) - 1_000), (1, 50_000)] {
        let (through, _, _) = play(&ogg, loops, (at + 4_000) as usize);
        let want = (through.len() / 2 - at as usize).min(4_000);
        let (mut stream, mut channels, mut rate) = (std::ptr::null_mut(), 0u32, 0u32);
        unsafe {
            assert!(matches!(sc_audio_open(ogg.as_ptr(), ogg.len(), loops, &mut stream, &mut channels, &mut rate), sc_status::SC_STATUS_OK));
            assert!(matches!(sc_audio_seek(stream, at), sc_status::SC_STATUS_OK));
            let mut buf = vec![0f32; 4_000 * 2];
            let mut got = 0usize;
            while got < want {
                let n = sc_audio_read(stream, buf[got * 2..].as_mut_ptr(), want - got);
                assert!(n > 0, "loops {loops} at {at}: ended after {got}");
                got += n as usize;
            }
            assert!(buf[..want * 2] == through[at as usize * 2..(at as usize + want) * 2], "loops {loops} at {at}");
            sc_audio_close(stream);
        }
    }
    // Past the end of a finite play there is nothing left.
    let (mut stream, mut channels, mut rate) = (std::ptr::null_mut(), 0u32, 0u32);
    unsafe {
        sc_audio_open(ogg.as_ptr(), ogg.len(), 1, &mut stream, &mut channels, &mut rate);
        assert!(matches!(sc_audio_seek(stream, 10_000_000), sc_status::SC_STATUS_OK));
        let mut buf = vec![0f32; 64];
        assert_eq!(sc_audio_read(stream, buf.as_mut_ptr(), 32), 0);
        sc_audio_close(stream);
    }
}
