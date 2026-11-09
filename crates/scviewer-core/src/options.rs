//! Serializable viewer export options; defaults follow StarBreaker.
use serde::{Deserialize, Serialize};
use starbreaker_3d::{ExportFormat, ExportKind, ExportOptions, MaterialMode};

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
pub enum Materials {
    Textures,
    Colors,
    None,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct GlbOptions {
    pub lod: u32,
    pub mip: u32,
    pub materials: Materials,
    pub attachments: bool,
    pub interior: bool,
    pub lights: bool,
    pub shields: bool,
    pub apply_default_pose: bool,
}
impl Default for GlbOptions {
    fn default() -> Self {
        let o = ExportOptions::default();
        Self {
            lod: o.lod_level,
            mip: o.texture_mip,
            materials: Materials::Textures,
            attachments: o.include_attachments,
            interior: o.include_interior,
            lights: o.include_lights,
            shields: o.include_shields,
            apply_default_pose: o.apply_default_animation_pose,
        }
    }
}
impl GlbOptions {
    pub(crate) fn upstream(&self, kind: ExportKind) -> ExportOptions {
        ExportOptions {
            kind,
            format: ExportFormat::Glb,
            lod_level: self.lod,
            texture_mip: self.mip,
            material_mode: match self.materials {
                Materials::Textures => MaterialMode::Textures,
                Materials::Colors => MaterialMode::Colors,
                Materials::None => MaterialMode::None,
            },
            include_attachments: self.attachments,
            include_interior: self.interior,
            include_lights: self.lights,
            include_shields: self.shields,
            apply_default_animation_pose: self.apply_default_pose,
            include_animations: kind == ExportKind::Decomposed,
            ..Default::default()
        }
    }
}

/// Decomposed exports always include animation sidecars and default to the authored rest pose.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct DecomposedOptions {
    pub geometry: GlbOptions,
}
impl Default for DecomposedOptions {
    fn default() -> Self {
        Self {
            geometry: GlbOptions {
                apply_default_pose: false,
                ..Default::default()
            },
        }
    }
}
