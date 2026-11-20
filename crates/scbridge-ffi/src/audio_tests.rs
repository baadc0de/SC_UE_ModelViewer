//! Ogg streaming through the C ABI: argument and format errors, and (when a decoded game media file from an export is
//! present under `out/audio/`, never in git) a full decode, a partial read and a rewind.
use super::*;
use std::path::PathBuf;

#[test]
fn bad_arguments_and_non_ogg_bytes_are_rejected() {
    let mut stream = std::ptr::null_mut();
    unsafe {
        assert!(matches!(sc_audio_open(std::ptr::null(), 4, &mut stream, std::ptr::null_mut(), std::ptr::null_mut()), sc_status::SC_STATUS_INVALID_ARGUMENT));
        let junk = b"not an ogg stream at all";
        assert!(matches!(sc_audio_open(junk.as_ptr(), junk.len(), &mut stream, std::ptr::null_mut(), std::ptr::null_mut()), sc_status::SC_STATUS_ERROR));
        assert!(stream.is_null());
        assert_eq!(sc_audio_read(std::ptr::null_mut(), std::ptr::null_mut(), 16), -1);
        sc_audio_close(std::ptr::null_mut());
    }
}

/// The C2 top lift door's open sound (2 ch, 48 kHz, 4.03 s per ATL `wwise_duration`), from a local export.
fn game_media() -> Option<Vec<u8>> {
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../out/audio");
    let found = ["manifest-test/Audio/872222338.ogg"].iter().map(|p| root.join(p)).find(|p| p.exists());
    match found {
        Some(path) => std::fs::read(path).ok(),
        None => {
            eprintln!("skip: no decoded game media under {}", root.display());
            None
        }
    }
}

#[test]
fn decodes_a_game_media_file_in_chunks_and_rewinds() {
    let Some(ogg) = game_media() else { return };
    let (mut stream, mut channels, mut rate) = (std::ptr::null_mut(), 0u32, 0u32);
    unsafe {
        assert!(matches!(sc_audio_open(ogg.as_ptr(), ogg.len(), &mut stream, &mut channels, &mut rate), sc_status::SC_STATUS_OK));
        assert_eq!((channels, rate), (2, 48_000));
        let mut buf = vec![0f32; 1000 * 2];
        let mut frames = 0i64;
        let mut peak = 0f32;
        loop {
            let n = sc_audio_read(stream, buf.as_mut_ptr(), 1000);
            assert!(n >= 0);
            if n == 0 {
                break;
            }
            peak = buf[..n as usize * 2].iter().fold(peak, |m, s| m.max(s.abs()));
            frames += n;
        }
        let seconds = frames as f32 / rate as f32;
        assert!((seconds - 4.0308).abs() < 0.05, "{seconds} s");
        assert!(peak > 0.01 && peak <= 1.0);
        assert!(matches!(sc_audio_rewind(stream), sc_status::SC_STATUS_OK));
        assert_eq!(sc_audio_read(stream, buf.as_mut_ptr(), 1000), 1000);
        sc_audio_close(stream);
    }
}
