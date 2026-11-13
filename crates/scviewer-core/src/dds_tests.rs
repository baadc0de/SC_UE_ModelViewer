//! Decoder-oracle coverage of package adaptation, validation, and linear means.
use super::*;
fn put(bytes: &mut [u8], offset: usize, value: u32) {
    bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}
fn package(format: u32, width: u32, height: u32, payload: &[u8]) -> Vec<u8> {
    let mut bytes = vec![0; 148];
    bytes[..4].copy_from_slice(b"DDS ");
    for (o, v) in [
        (4, 124),
        (8, 0xA1007),
        (12, height),
        (16, width),
        (20, payload.len() as u32),
        (28, 1),
        (76, 32),
        (80, 4),
        (108, 0x1000),
        (128, format),
        (132, 3),
        (140, 1),
    ] {
        put(&mut bytes, o, v);
    }
    bytes[84..88].copy_from_slice(b"DX10");
    bytes.extend_from_slice(payload);
    bytes
}
#[test]
fn snorm_bc4_bc5_random_blocks_match_decoder_in_both_modes() {
    let mut rng = 0x12345678_u32;
    for channels in [1, 2] {
        for trial in 0..4096 {
            let mut block = vec![0_u8; channels * 8];
            for b in &mut block {
                rng ^= rng << 13;
                rng ^= rng >> 17;
                rng ^= rng << 5;
                *b = rng as u8;
            }
            // Guarantee -128 coverage, including the formerly mode-changing pair.
            if trial % 16 == 0 {
                for channel in block.chunks_exact_mut(8) {
                    channel[0] = 0x81;
                    channel[1] = 0x80;
                }
            }
            for flip in [false, true] {
                if channels == 1 && flip {
                    continue;
                }
                let mut data = package(if channels == 1 { 81 } else { 84 }, 4, 4, &block);
                let info = prepare(&mut data, flip).unwrap();
                assert_eq!(info.dxgi_format, if channels == 1 { 80 } else { 83 });
                let mut before = [0.0; 32];
                let mut after = [0.0; 32];
                if channels == 1 {
                    bcdec_rs::bc4_float(&block, &mut before, 4, true);
                    bcdec_rs::bc4_float(&data[148..], &mut after, 4, false);
                } else {
                    bcdec_rs::bc5_float(&block, &mut before, 8, true);
                    bcdec_rs::bc5_float(&data[148..], &mut after, 8, false);
                }
                for i in 0..16 * channels {
                    let expected = if flip && i % 2 == 1 {
                        -before[i]
                    } else {
                        before[i]
                    };
                    assert!(
                        (after[i] * 2.0 - 1.0 - expected).abs() <= 1.0 / 127.0 + 1e-6,
                        "trial {trial}, channels {channels}, flip {flip}, texel {i}"
                    );
                }
                if flip {
                    let mut unflipped = package(84, 4, 4, &block);
                    prepare(&mut unflipped, false).unwrap();
                    assert_eq!(
                        &data[148..156],
                        &unflipped[148..156],
                        "red block must be unchanged by flip"
                    );
                }
            }
        }
    }
}
#[test]
fn unorm_normal_flip_preserves_red_and_negates_green() {
    for a in [0, 1, 64, 128, 254, 255] {
        for b in [0, 1, 64, 128, 254, 255] {
            let mut block = [0_u8; 16];
            block[..8].copy_from_slice(&[19, 200, 0x88, 0xc6, 0xfa, 0x88, 0xc6, 0xfa]);
            block[8..].copy_from_slice(&[a, b, 0x88, 0xc6, 0xfa, 0x88, 0xc6, 0xfa]);
            let mut data = package(83, 4, 4, &block);
            prepare(&mut data, true).unwrap();
            let mut before = [0_u8; 32];
            let mut after = [0_u8; 32];
            bcdec_rs::bc5(&block, &mut before, 8, false);
            bcdec_rs::bc5(&data[148..], &mut after, 8, false);
            for i in 0..16 {
                assert_eq!(before[2 * i], after[2 * i]);
                assert!((255 - before[2 * i + 1] as i16 - after[2 * i + 1] as i16).abs() <= 1);
            }
        }
    }
}
#[test]
fn mean_linearizes_each_texel_before_averaging() {
    // Synthetic BC1: alternating mid-red (RGB565 red=16) and black endpoints.
    let block = [0, 0x80, 0, 0, 0x44, 0x44, 0x44, 0x44];
    let red = 132.0_f32 / 255.0;
    for (format, expected) in [
        (71, red / 2.0),
        (72, ((red + 0.055) / 1.055).powf(2.4) / 2.0),
    ] {
        let mean = mean_rgb(&package(format, 4, 4, &block)).unwrap();
        assert!((mean[0] - expected).abs() < 1e-6);
        assert_eq!(mean[1..], [0.0, 0.0]);
    }
    let mut mip_chain = package(71, 4, 4, &block);
    put(&mut mip_chain, 28, 2);
    mip_chain.extend_from_slice(&[0xff; 8]);
    assert!((mean_rgb(&mip_chain).unwrap()[0] - red / 2.0).abs() < 1e-6);
    // Even a complete payload cannot make an unaligned compressed top valid.
    assert!(mean_rgb(&package(71, 1, 1, &block)).is_err());
}
#[test]
fn malformed_packages_fail_before_mutation() {
    let valid = package(84, 4, 4, &[0; 16]);
    for (offset, value) in [
        (4, 0),
        (8, 0),
        (12, 0),
        (16, 0),
        (20, 15),
        (24, 2),
        (28, 0),
        (28, 33),
        (76, 0),
        (80, 0),
        (112, 0x200),
        (128, 95),
        (132, 4),
        (136, 4),
        (140, 2),
    ] {
        let mut bytes = valid.clone();
        put(&mut bytes, offset, value);
        let before = bytes.clone();
        assert!(prepare(&mut bytes, false).is_err(), "offset {offset}");
        assert_eq!(bytes, before);
    }
    let mut truncated = valid[..valid.len() - 1].to_vec();
    assert!(prepare(&mut truncated, false).is_err());
    let mut trailing = valid.clone();
    trailing.push(0);
    assert!(prepare(&mut trailing, false).is_err());
    assert!(prepare(&mut package(80, 4, 4, &[0; 8]), true).is_err());
}

#[test]
fn preparation_transforms_every_mip_and_reports_full_payload() {
    let block = [0x81, 0x80, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff];
    let mut data = package(81, 4, 4, &block);
    put(&mut data, 28, 3);
    data.extend_from_slice(&block);
    data.extend_from_slice(&block);
    let info = prepare(&mut data, false).unwrap();
    assert_eq!(
        (
            info.width,
            info.height,
            info.mip_count,
            info.data_offset,
            info.data_size
        ),
        (4, 4, 3, 148, 24)
    );
    for mip in data[148..].chunks_exact(8) {
        assert_eq!(&mip[..2], &[1, 1]);
        assert_eq!(&mip[2..], &block[2..]);
    }
}

#[test]
fn mip_counts_zero_one_and_excessive_are_validated() {
    for count in [0, 1, 3, 4] {
        let mut data = package(71, 4, 4, &vec![0; 8 * count as usize]);
        put(&mut data, 20, 8);
        put(&mut data, 28, count);
        assert_eq!(
            prepare(&mut data, false).is_ok(),
            matches!(count, 1 | 3),
            "count={count}"
        );
    }
}

#[test]
fn compressed_top_mips_must_be_block_aligned() {
    for format in [71, 72, 74, 75, 77, 78, 80, 81, 83, 84, 98, 99] {
        for (w, h) in [(1, 1), (2, 2), (3, 4), (4, 5), (5, 7)] {
            let mut data = package(format, w, h, &vec![0; mip_byte_size(format, w, h).unwrap()]);
            let before = data.clone();
            assert!(
                prepare(&mut data, false).is_err(),
                "format={format}, top={w}x{h}"
            );
            assert_eq!(data, before);
            assert!(mean_rgb(&data).is_err());
        }
    }
    assert!(prepare(&mut package(61, 1, 1, &[127]), false).is_ok());
}

#[test]
fn r8_mean_has_zero_green_and_blue() {
    assert_eq!(
        mean_rgb(&package(61, 2, 1, &[0, 255])).unwrap(),
        [0.5, 0.0, 0.0]
    );
}

#[test]
fn prepared_bc4_mean_has_zero_green_and_blue() {
    for format in [80, 81] {
        let mut data = package(format, 4, 4, &[127, 0, 0, 0, 0, 0, 0, 0]);
        let info = prepare(&mut data, false).unwrap();
        assert_eq!(info.dxgi_format, 80);
        let expected = if format == 80 { 127.0 / 255.0 } else { 1.0 };
        assert_eq!(mean_rgb(&data).unwrap(), [expected, 0.0, 0.0]);
    }
}
#[test]
fn decode_top_bgra8_bc1_solid_and_bc5_flat_normal() {
    // BC1: both endpoints red 5:6:5 (0xF800), all indices 0 -> every texel (255, 0, 0, 255), written as BGRA.
    let mut bc1 = vec![0u8; 8];
    bc1[0..2].copy_from_slice(&0xF800u16.to_le_bytes());
    bc1[2..4].copy_from_slice(&0xF800u16.to_le_bytes());
    let out = decode_top_bgra8(&bc1, 71, 4, 4).unwrap();
    assert_eq!(out.len(), 4 * 4 * 4);
    assert!(out.chunks_exact(4).all(|p| p == [0, 0, 255, 255]), "{:?}", &out[..4]);
    // BC5 normal: X = Y = 128 (flat); blue is rebuilt from the unit vector, so Z ~ 1 -> 255.
    let mut bc5 = vec![0u8; 16];
    bc5[0] = 128;
    bc5[1] = 128;
    bc5[8] = 128;
    bc5[9] = 128;
    let out = decode_top_bgra8(&bc5, 83, 4, 4).unwrap();
    assert!(out.chunks_exact(4).all(|p| p[2].abs_diff(128) <= 1 && p[1].abs_diff(128) <= 1 && p[0] >= 254 && p[3] == 255), "{:?}", &out[..4]);
    // Unsupported formats are an error, not garbage.
    assert!(decode_top_bgra8(&bc1, 9999, 4, 4).is_err());
}
