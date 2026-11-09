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
    /// Ordered animation tags passed to upstream's default-pose selection.
    #[serde(default = "default_animation_tags")]
    pub default_animation_tags: Vec<String>,
    pub include_animations: bool,
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
            default_animation_tags: o.default_animation_tags,
            include_animations: true,
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
            default_animation_tags: self.default_animation_tags.clone(),
            include_animations: self.include_animations,
            ..Default::default()
        }
    }
}

fn default_animation_tags() -> Vec<String> {
    ExportOptions::default().default_animation_tags
}

/// Decomposed exports default to animation sidecars and the authored rest pose.
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn default_animation_tags_survive_serialization_and_upstream_conversion() {
        let options = GlbOptions {
            default_animation_tags: vec!["synthetic_a".into(), "synthetic_b".into()],
            ..Default::default()
        };
        let mut json = serde_json::to_value(&options).unwrap();
        let decoded: GlbOptions = serde_json::from_value(json.clone()).unwrap();
        assert_eq!(decoded, options);
        assert_eq!(
            decoded.upstream(ExportKind::Bundled).default_animation_tags,
            options.default_animation_tags
        );
        json.as_object_mut()
            .unwrap()
            .remove("default_animation_tags");
        let legacy: GlbOptions = serde_json::from_value(json).unwrap();
        assert_eq!(
            legacy.default_animation_tags,
            ExportOptions::default().default_animation_tags
        );
    }
}
