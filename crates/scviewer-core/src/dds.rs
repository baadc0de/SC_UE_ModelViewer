//! Validate package DDS headers and payloads, adapt BC4/BC5 blocks in place,
//! and compute top-mip linear-light RGB means without allocating a full image.
use anyhow::{Context, Result, ensure};
use starbreaker_dds::{
    DdsFile,
    package::{max_mip_count, mip_byte_size, top_mip_is_aligned},
};

/// Validated payload descriptor. Offsets and sizes are relative to the DDS bytes.
#[derive(Debug, Default)]
pub struct DdsInfo {
    pub dxgi_format: u32,
    pub width: u32,
    pub height: u32,
    pub mip_count: u32,
    pub data_offset: u64,
    pub data_size: u64,
}

fn validate(data: &[u8]) -> Result<DdsInfo> {
    ensure!(data.len() >= 148, "truncated package DDS header");
    let dds = DdsFile::headers_only(data)?;
    let h = dds.header;
    let dx = dds
        .dxt10_header
        .context("package DDS requires DX10 header")?;
    ensure!(
        h.size == 124 && h.pixel_format.size == 32 && h.pixel_format.flags == 4,
        "invalid DDS header sizes or pixel format flags"
    );
    ensure!(h.flags == 0xA1007, "invalid package DDS flags");
    ensure!(
        h.width > 0 && h.height > 0 && h.depth == 0 && h.cubemap_flags == 0,
        "package DDS must be a nonempty 2D texture"
    );
    ensure!(
        dx.resource_dimension == 3 && dx.misc_flag == 0 && dx.array_size == 1,
        "DDS arrays, cubemaps and volumes are unsupported"
    );
    ensure!(
        h.mipmap_count > 0 && h.mipmap_count <= max_mip_count(h.width, h.height),
        "invalid DDS mip count"
    );
    ensure!(
        top_mip_is_aligned(dx.dxgi_format, h.width, h.height),
        "block-compressed DDS top mip must be aligned to 4 texels"
    );
    let mut size = 0_usize;
    for mip in 0..h.mipmap_count {
        let n = mip_byte_size(
            dx.dxgi_format,
            (h.width >> mip).max(1),
            (h.height >> mip).max(1),
        )
        .context("unsupported DDS format or overflowing mip size")?;
        if mip == 0 {
            ensure!(
                u64::from(h.pitch_or_linear_size) == n as u64,
                "DDS linear size does not match top mip"
            );
        }
        size = size.checked_add(n).context("DDS payload size overflow")?;
    }
    ensure!(size == data.len() - 148, "DDS mip payload size mismatch");
    Ok(DdsInfo {
        dxgi_format: dx.dxgi_format,
        width: h.width,
        height: h.height,
        mip_count: h.mipmap_count,
        data_offset: 148,
        data_size: size as u64,
    })
}

/// Adapt validated package blocks for UE. Validation completes before mutation.
/// SNORM endpoints clamp -128 to -127 before negation/rebias; indices are only
/// remapped for the normal green-channel flip. The DX10 format is updated too.
pub fn prepare(data: &mut [u8], normal_map: bool) -> Result<DdsInfo> {
    let mut info = validate(data)?;
    let signed = matches!(info.dxgi_format, 81 | 84);
    let bc5 = matches!(info.dxgi_format, 83 | 84);
    ensure!(!normal_map || bc5, "normal_map requires BC5");
    if signed || normal_map {
        let stride = if bc5 { 16 } else { 8 };
        for block in data[148..].chunks_exact_mut(stride) {
            for (channel, bc4) in block.chunks_exact_mut(8).enumerate() {
                let endpoint = |byte: u8| {
                    if signed {
                        i16::from((byte as i8).max(-127))
                    } else {
                        i16::from(byte)
                    }
                };
                let mut a = endpoint(bc4[0]);
                let mut b = endpoint(bc4[1]);
                if normal_map && channel == 1 {
                    let eight = a > b;
                    (a, b) = if signed { (-b, -a) } else { (255 - b, 255 - a) };
                    let packed = u64::from_le_bytes(bc4.try_into().expect("BC4 block"));
                    let mut indices = 0_u64;
                    for texel in 0..16 {
                        let index = (packed >> (16 + 3 * texel)) & 7;
                        let remapped = match index {
                            0 => 1,
                            1 => 0,
                            k if eight => 9 - k,
                            2 => 5,
                            3 => 4,
                            4 => 3,
                            5 => 2,
                            6 => 7,
                            7 => 6,
                            _ => unreachable!(),
                        };
                        indices |= remapped << (16 + 3 * texel);
                    }
                    bc4.copy_from_slice(&indices.to_le_bytes());
                }
                bc4[0] = (a + if signed { 128 } else { 0 }) as u8;
                bc4[1] = (b + if signed { 128 } else { 0 }) as u8;
            }
        }
    }
    info.dxgi_format = match info.dxgi_format {
        81 => 80,
        84 => 83,
        f => f,
    };
    data[128..132].copy_from_slice(&info.dxgi_format.to_le_bytes());
    Ok(info)
}

/// Linear-light mean of the first mip. Ignores alpha and padded block texels;
/// sRGB transfer is applied per texel before accumulating in f64.
/// Missing color channels sample as zero: R8/BC4 return (R, 0, 0), BC5 (R, G, 0).
pub fn mean_rgb(data: &[u8]) -> Result<[f32; 3]> {
    let info = validate(data)?;
    let f = info.dxgi_format;
    let srgb = matches!(f, 29 | 72 | 75 | 78 | 91 | 99);
    let mut sum = [0.0_f64; 3];
    let mut add = |rgb: [f32; 3]| {
        for c in 0..3 {
            let v = f64::from(rgb[c]);
            sum[c] += if !srgb {
                v
            } else if v <= 0.04045 {
                v / 12.92
            } else {
                ((v + 0.055) / 1.055).powf(2.4)
            };
        }
    };
    let top_size = mip_byte_size(f, info.width, info.height).context("unsupported DDS format")?;
    let top = &data[148..148 + top_size];
    if matches!(f, 61 | 28 | 29 | 87 | 91) {
        let stride = if f == 61 { 1 } else { 4 };
        for pixel in top.chunks_exact(stride) {
            let rgb = if f == 61 {
                [pixel[0], 0, 0]
            } else if matches!(f, 87 | 91) {
                [pixel[2], pixel[1], pixel[0]]
            } else {
                [pixel[0], pixel[1], pixel[2]]
            };
            add(rgb.map(|v| f32::from(v) / 255.0));
        }
    } else {
        let stride = mip_byte_size(f, 4, 4).context("unsupported block format")?;
        let bw = info.width.div_ceil(4) as usize;
        for (index, block) in top.chunks_exact(stride).enumerate() {
            let mut rgb = [[0.0; 3]; 16];
            if matches!(f, 80 | 81 | 83 | 84) {
                let two = matches!(f, 83 | 84);
                let signed = matches!(f, 81 | 84);
                let mut decoded = [0.0; 32];
                if two {
                    bcdec_rs::bc5_float(block, &mut decoded, 8, signed);
                } else {
                    bcdec_rs::bc4_float(block, &mut decoded, 4, signed);
                }
                for i in 0..16 {
                    let channel = |v: f32| if signed { v * 0.5 + 0.5 } else { v };
                    rgb[i] = if two {
                        [channel(decoded[i * 2]), channel(decoded[i * 2 + 1]), 0.0]
                    } else {
                        [channel(decoded[i]), 0.0, 0.0]
                    };
                }
            } else {
                let mut decoded = [0_u8; 64];
                match f {
                    71 | 72 => bcdec_rs::bc1(block, &mut decoded, 16),
                    74 | 75 => bcdec_rs::bc2(block, &mut decoded, 16),
                    77 | 78 => bcdec_rs::bc3(block, &mut decoded, 16),
                    98 | 99 => bcdec_rs::bc7(block, &mut decoded, 16),
                    _ => unreachable!("validated format"),
                }
                for i in 0..16 {
                    rgb[i] = [decoded[i * 4], decoded[i * 4 + 1], decoded[i * 4 + 2]]
                        .map(|v| f32::from(v) / 255.0);
                }
            }
            for py in 0..4 {
                for px in 0..4 {
                    if (index % bw) * 4 + px < info.width as usize
                        && (index / bw) * 4 + py < info.height as usize
                    {
                        add(rgb[py * 4 + px]);
                    }
                }
            }
        }
    }
    let count = f64::from(info.width) * f64::from(info.height);
    Ok(sum.map(|v| (v / count) as f32))
}

#[cfg(test)]
#[path = "dds_tests.rs"]
mod tests;
