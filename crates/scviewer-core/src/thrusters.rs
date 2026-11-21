//! Authored vehicle thruster preview data added to the decomposed scene manifest.
use crate::Session;
use anyhow::Result;
use serde_json::{Value, json};
use starbreaker_3d::DecomposedExport;
use std::collections::BTreeMap;

fn entries(value: &Value) -> impl Iterator<Item = &Value> {
    value.as_array().into_iter().flatten()
}

fn component<'a>(entity: &'a Value, kind: &str) -> Option<&'a Value> {
    entries(&entity["Components"]).find(|item| item["__type"].as_str() == Some(kind))
}

fn enabled(value: &Value) -> bool {
    value["enabled"].as_bool() == Some(true)
}

fn xyz(value: &Value) -> Option<[f64; 3]> {
    Some([
        value["x"].as_f64()?,
        value["y"].as_f64()?,
        value["z"].as_f64()?,
    ])
}

fn endpoints(link: Option<&Value>) -> Option<[f64; 2]> {
    let mut values = [0.0, 1.0];
    for modifier in link
        .into_iter()
        .flat_map(|link| entries(&link["modifiers"]))
    {
        match modifier["__type"].as_str()? {
            "SEntityEffectSystem_MultiplierModifier" => {
                let multiplier = modifier["multiplier"].as_f64()?;
                values = values.map(|value| value * multiplier);
            }
            "SEntityEffectSystem_MinMaxModifier" => {
                let min = modifier["minimum"].as_f64()?;
                let max = modifier["maximum"].as_f64()?;
                if min > max {
                    return None;
                }
                values = values.map(|value| value.clamp(min, max));
            }
            _ => return None,
        }
    }
    Some(values)
}

fn strength_link<'a>(
    effect: &'a Value,
    key: &str,
    property: &str,
    tag_name: &mut impl FnMut(&str) -> Option<String>,
) -> Option<&'a Value> {
    entries(&effect["propertyLinks"]).find(|link| {
        link[key]
            .as_str()
            .is_some_and(|p| p.eq_ignore_ascii_case(property))
            && link["parameterName"]
                .as_str()
                .and_then(&mut *tag_name)
                .as_deref()
                == Some("ThrustStrength")
    })
}

fn parse_effect(
    effect: &Value,
    role: &str,
    tag_name: &mut impl FnMut(&str) -> Option<String>,
) -> Option<Value> {
    let reference = effect["particleEffect"]["path"]
        .as_str()
        .filter(|s| !s.is_empty())?;
    let attachment = &effect["attachmentSettings"];
    let zone = attachment["attachmentZone"].as_str()?;
    if !matches!(zone, "Host" | "HostParent") {
        return None;
    }
    let bone = attachment["boneAttachment"].as_str()?;
    let offset = &attachment["offset"];
    Some(json!({
        "role": role,
        "reference": reference,
        "zone": zone,
        "bone": bone,
        "position_sc_m": xyz(&offset["Position"])?,
        "rotation_sc": xyz(&offset["Rotation"])?,
        "strength": endpoints(strength_link(effect, "particleProperty", "strength", tag_name))?,
    }))
}

fn describe(record: &Value, tag_name: &mut impl FnMut(&str) -> Option<String>) -> Option<Value> {
    let item = component(record, "SCItemThrusterParams")?;
    let system = &component(record, "SEntityComponentEffects")?["entityEffects"];
    let effects = &system["particleEffects"];
    if !enabled(effects) {
        return None;
    }
    let mut result = Vec::new();
    for effect in entries(&effects["tagEffects"]).filter(|effect| enabled(effect)) {
        let role = match effect["tag"].as_str().and_then(&mut *tag_name).as_deref() {
            Some("Thrust") => "thrust",
            Some("AfterBurn") => "boost",
            _ => continue,
        };
        if let Some(effect) = parse_effect(effect, role, tag_name) {
            result.push(effect);
        }
    }
    if result.is_empty() {
        return None;
    }
    let category = &system["materialEffects"];
    let mut normal = None;
    let mut boost = None;
    if enabled(category) {
        for effect in entries(&category["tagEffects"]).filter(|effect| enabled(effect)) {
            let target = match effect["tag"].as_str().and_then(&mut *tag_name).as_deref() {
                Some("Thrust") => &mut normal,
                Some("AfterBurn") => &mut boost,
                _ => continue,
            };
            if let Some(link) = strength_link(effect, "materialProperty", "Glow", tag_name) {
                *target = endpoints(Some(link));
            }
        }
    }
    let glow =
        normal.map(|[minimum, full]| [minimum, full, boost.map_or(full, |values| values[1])]);
    let nozzle = item["nozzleAnimation"].as_str().unwrap_or_default();
    Some(json!({"effects": result, "nozzle_animation": nozzle, "glow": glow}))
}

pub(crate) fn add_to_package(session: &Session, package: &mut DecomposedExport) -> Result<()> {
    let mut records = BTreeMap::<String, Option<Value>>::new();
    let mut tags = BTreeMap::<String, Option<String>>::new();
    let mut tag_name = |id: &str| {
        tags.entry(id.to_owned())
            .or_insert_with(|| {
                let tag = session.datacore_record(id).ok()?;
                tag["tagName"].as_str().map(str::to_owned)
            })
            .clone()
    };
    for manifest in package
        .files
        .iter_mut()
        .filter(|file| file.relative_path.ends_with("/scene.json"))
    {
        let mut scene: Value = serde_json::from_slice(&manifest.bytes)?;
        let Some(children) = scene["children"].as_array_mut() else {
            continue;
        };
        let mut changed = false;
        for child in children {
            let Some(item_type) = child["item_type"].as_str() else {
                continue;
            };
            if !item_type.ends_with("Thruster") {
                continue;
            }
            let Some(name) = child["entity_name"].as_str() else {
                continue;
            };
            let data = records.entry(name.to_owned()).or_insert_with(|| {
                let record = session
                    .datacore_record(&format!("EntityClassDefinition.{name}"))
                    .ok()?;
                describe(&record, &mut tag_name)
            });
            if let Some(data) = data {
                child["thruster_vfx"] = data.clone();
                changed = true;
            }
        }
        if changed {
            manifest.bytes = serde_json::to_vec_pretty(&scene)?;
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tag(id: &str) -> Option<String> {
        match id {
            "run" => Some("Thrust".to_owned()),
            "boost" => Some("AfterBurn".to_owned()),
            "throttle" => Some("ThrustStrength".to_owned()),
            _ => None,
        }
    }

    fn effect(id: &str, reference: &str, active: bool) -> Value {
        json!({
            "enabled": active, "tag": id, "particleEffect": {"path": reference},
            "attachmentSettings": {
                "attachmentZone": "Host", "boneAttachment": "test_socket",
                "offset": {"Position": {"x": 1, "y": 2, "z": 3}, "Rotation": {"x": 0, "y": 0, "z": 0}}
            },
            "propertyLinks": [{"particleProperty": "strength", "parameterName": "throttle", "modifiers": [
                {"__type": "SEntityEffectSystem_MultiplierModifier", "multiplier": 0.8},
                {"__type": "SEntityEffectSystem_MinMaxModifier", "minimum": 0.2, "maximum": 0.9}
            ]}]
        })
    }

    #[test]
    fn resolves_authored_tags_strength_curves_and_attachment() {
        let record = json!({"Components": [
            {"__type": "SCItemThrusterParams", "nozzleAnimation": "test_clip"},
            {"__type": "SEntityComponentEffects", "entityEffects": {"particleEffects": {
                "enabled": true, "tagEffects": [
                    effect("run", "test.live", true), effect("boost", "test.boost", true),
                    effect("run", "test.disabled", false), effect("unrelated", "test.damage", true)
                ]
            }}}
        ]});
        let result = describe(&record, &mut tag).unwrap();
        assert_eq!(result["nozzle_animation"], "test_clip");
        let effects = result["effects"].as_array().unwrap();
        assert_eq!(effects.len(), 2);
        assert_eq!(effects[0]["strength"], json!([0.2, 0.8]));
        assert_eq!(effects[0]["position_sc_m"], json!([1.0, 2.0, 3.0]));
        assert_eq!(effects[1]["role"], "boost");
        assert_eq!(effects[1]["reference"], "test.boost");
    }
}
