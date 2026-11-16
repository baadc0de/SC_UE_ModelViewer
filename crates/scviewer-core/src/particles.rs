//! Star Citizen particle effects as JSON (`Session::particle_effect`, `Session::material_effect`), for hosts that
//! rebuild them (the viewer's Niagara templates). Read from the archive at run time, never copied.
//!
//! **Effects.** A reference is `<GUID>|<library>.<effect>` (the GUID part optional), e.g.
//! `2c211ca4-…|Gadgets/tractor_beam.fps.beam_only`: the library is `Data/Libs/Particles/<library>.xml` (CryXmlB
//! `ParticleLibrary`), split at the first `.` after the last `/`, since effect paths have dots and library paths do
//! not. Effects nest as `<Particles Name GUID><Params …/><Childs><Particles …/></Childs></Particles>`; a top-level
//! `Name` is the whole dotted path (`fps.beam_only`), a child's is its last part, so a child's path is
//! `<parent path>.<name>`. The reference may name a child too. Its GUID must be the effect's `GUID` attribute; when
//! the name finds another effect (or none) the GUID's effect wins, and a GUID found nowhere is an error.
//!
//! **Emitters.** The effect and every descendant, depth first, `parent` an index into the list (-1 for the first).
//! `raw` has each `Params` attribute as written; the other fields read the parameter syntax
//! ([`crate::particle_params`]) of the *effective* params:
//! - `Inheritance="Parent"`: an attribute the emitter omits is its parent's effective value, and a partial one is
//!   merged over the parent's field by field (`inherited` names the attributes the parent contributed). The editor
//!   omits values equal to what they inherit, so this is what the engine sees.
//! - `"System"` (and every top-level emitter, and the `ExternalRef*` kinds, whose referenced effect is not read):
//!   omitted values are the engine's system defaults. The engine compiles them in; [`SYSTEM_DEFAULTS`] was inferred
//!   from the 877 libraries of LIVE 4.10.2: a value the libraries write explicitly is never the default (e.g.
//!   `Speed="0"` 11 454 times and `Speed="1"` never, so Speed defaults to 1). `defaulted` names the attributes whose
//!   base value came from that table.
//!
//! **Curves.** `ParticleAge=(…)` (over a particle's life, 0..1) becomes `age_curve`, `EmitterStrength=(…)` (over
//! the emitter's strength input, 0..1: the tractor beam's beam strength) `strength_curve`, as `[t, v]` or
//! `[t, r, g, b]` keys. A key's missing `t` or `v` is 0, never 1: the data writes `v=1` tens of thousands of times
//! and `v=0` never. Curves multiply the base value.
//!
//! **Material effects.** `<FXLib>:<effect>`, e.g. `sc_ships_surface_impacts:Metal_Small`, is the `<Effect name>` of
//! `Data/Libs/MaterialEffects/FXLibs/<FXLib>.xml`: its particle references (with the `<Name>` and `<Particle>`
//! attributes, such as `minscale`) and audio triggers. A `<RandEffect>` holds alternatives; its entries carry the
//! same `random_group` index.
use crate::{
    Session,
    archive::{Archive, Element, read_element},
    particle_params::{self as pp, PValue},
};
use anyhow::{Context, Result, anyhow, bail, ensure};
use serde_json::{Map, Value, json};
use std::{
    cell::RefCell,
    collections::{BTreeMap, BTreeSet},
};

pub(crate) const PARTICLE_DIR: &str = "Data/Libs/Particles/";
pub(crate) const FXLIB_DIR: &str = "Data/Libs/MaterialEffects/FXLibs/";

/// The engine's system defaults of the attributes the JSON reads, inferred from LIVE 4.10.2 (module docs).
pub const SYSTEM_DEFAULTS: &[(&str, &str)] = &[
    ("Enabled", "true"),
    ("ParticleSystem", "CryEngine"),
    ("Count", "1"),
    ("Continuous", "true"),
    ("ParticleLifeTime", "1"),
    ("EmitterLifeTime", "0"),
    ("PulsePeriod", "0"),
    ("SpawnDelay", "0"),
    ("EmitPerDistance", "0"),
    ("Size", "1"),
    ("Aspect", "1"),
    ("Alpha", "1"),
    ("Color", "(x=1,y=1,z=1)"),
    ("Speed", "1"),
    ("EmitAngle", "0"),
    ("FocusAngle", "0"),
    ("GravityScale", "0"),
    ("AirResistance", "0"),
    ("InheritVelocity", "0"),
    ("BlendType", "AlphaBased"),
    ("Facing", "Camera"),
    ("OrientToVelocity", "false"),
    ("EmissiveLighting", "0"),
    ("DiffuseLighting", "1"),
    ("Curvature", "1"),
    ("FlareEnabled", "false"),
    ("SplineGuided", "false"),
];

/// A split effect reference.
#[derive(Clone, Debug, PartialEq)]
pub struct EffectRef {
    pub guid: Option<String>,
    pub library: String,
    pub effect: String,
}

/// Split `<GUID>|<library>.<effect>` (GUID optional) at the first `.` after the last `/` of the library part.
pub fn split_reference(reference: &str) -> Result<EffectRef> {
    let reference = reference.trim();
    let (guid, rest) = match reference.split_once('|') {
        Some((guid, rest)) => (Some(guid.trim().to_owned()).filter(|g| !g.is_empty()), rest.trim()),
        None => (None, reference),
    };
    let rest = rest.replace('\\', "/");
    let dir_end = rest.rfind('/').map_or(0, |n| n + 1);
    let dot = rest[dir_end..]
        .find('.')
        .map(|n| n + dir_end)
        .with_context(|| format!("effect reference {reference:?} has no `<library>.<effect>`"))?;
    let (library, effect) = (&rest[..dot], &rest[dot + 1..]);
    ensure!(!library.is_empty() && !library.ends_with('/') && !effect.is_empty(), "effect reference {reference:?} has an empty library or effect");
    Ok(EffectRef { guid, library: library.to_owned(), effect: effect.to_owned() })
}

/// One `<Particles>` node of a library, depth first.
struct Node<'a> {
    element: &'a Element,
    path: String,
    parent: Option<usize>,
}

fn flatten<'a>(element: &'a Element, prefix: Option<&str>, parent: Option<usize>, out: &mut Vec<Node<'a>>) {
    for particles in element.children_named("Particles") {
        let name = particles.attr("Name").unwrap_or_default();
        let path = prefix.map_or_else(|| name.to_owned(), |p| format!("{p}.{name}"));
        let index = out.len();
        out.push(Node { element: particles, path: path.clone(), parent });
        for childs in particles.children_named("Childs") {
            flatten(childs, Some(&path), Some(index), out);
        }
    }
}

fn own_params(node: &Node) -> Vec<(String, String)> {
    node.element.children_named("Params").flat_map(|p| p.attrs.iter().cloned()).collect()
}

/// Effective params of every node (parents precede children) and the attributes each took from its parent.
fn effective_params(nodes: &[Node]) -> Vec<(BTreeMap<String, PValue>, Vec<String>)> {
    let mut out: Vec<(BTreeMap<String, PValue>, Vec<String>)> = Vec::with_capacity(nodes.len());
    for node in nodes {
        let own: BTreeMap<String, PValue> = own_params(node).iter().map(|(k, v)| (k.clone(), pp::parse(v))).collect();
        let from_parent = own.get("Inheritance").and_then(PValue::text) == Some("Parent");
        let (mut params, mut inherited) = (own.clone(), Vec::new());
        if let (true, Some(parent)) = (from_parent, node.parent) {
            for (key, parent_value) in &out[parent].0 {
                if matches!(key.as_str(), "Inheritance" | "Comment") {
                    continue;
                }
                let value = match own.get(key) {
                    Some(child) => pp::merge(parent_value, child),
                    None => parent_value.clone(),
                };
                if own.get(key) != Some(&value) {
                    inherited.push(key.clone());
                }
                params.insert(key.clone(), value);
            }
        }
        out.push((params, inherited));
    }
    out
}

/// The effect a reference names, with all its emitters, as JSON (module docs).
pub(crate) fn effect_json(archive: &dyn Archive, reference: &str) -> Result<Value> {
    let r = split_reference(reference)?;
    let library_path = format!("{PARTICLE_DIR}{}.xml", r.library);
    let library = read_element(archive, &library_path)?;
    ensure!(library.tag == "ParticleLibrary", "{library_path} is a <{}>, not a <ParticleLibrary>", library.tag);
    let mut nodes = Vec::new();
    flatten(&library, None, None, &mut nodes);
    let by_name = nodes.iter().position(|n| n.path.eq_ignore_ascii_case(&r.effect));
    let guid_of = |n: usize| nodes[n].element.attr("GUID").unwrap_or_default();
    let (target, matched_by) = match (&r.guid, by_name) {
        (None, Some(n)) => (n, "name"),
        (None, None) => bail!("{library_path} has no effect {:?}", r.effect),
        (Some(guid), Some(n)) if guid_of(n).eq_ignore_ascii_case(guid) => (n, "name"),
        (Some(guid), _) => {
            let found = (0..nodes.len()).find(|&n| guid_of(n).eq_ignore_ascii_case(guid));
            match (found, by_name) {
                (Some(n), _) => (n, "guid"),
                (None, Some(n)) => bail!(
                    "{library_path}: effect {:?} has GUID {}, not {guid}, and no effect has GUID {guid}",
                    r.effect,
                    guid_of(n)
                ),
                (None, None) => bail!("{library_path} has no effect {:?} and no effect with GUID {guid}", r.effect),
            }
        }
    };
    let effective = effective_params(&nodes);
    // The target and its descendants: the depth-first nodes after it until the next node outside its subtree.
    let mut subtree = vec![target];
    for (n, node) in nodes.iter().enumerate().skip(target + 1) {
        match node.parent {
            Some(p) if subtree.contains(&p) => subtree.push(n),
            _ => break,
        }
    }
    let emitters: Vec<Value> = subtree
        .iter()
        .map(|&n| {
            let parent = nodes[n].parent.and_then(|p| subtree.iter().position(|&s| s == p)).map_or(-1, |i| i as i64);
            emitter_json(archive, &nodes[n], parent, &effective[n].0, &effective[n].1)
        })
        .collect();
    Ok(json!({
        "reference": reference.trim(),
        "library": r.library,
        "library_path": archive.find(&library_path).unwrap_or(library_path),
        "effect": nodes[target].path,
        "guid": guid_of(target),
        "matched_by": matched_by,
        "emitters": emitters,
    }))
}

/// Typed reads of one emitter's effective params; records attributes whose base value came from the defaults.
struct Fields<'a> {
    params: &'a BTreeMap<String, PValue>,
    defaults: BTreeMap<&'static str, PValue>,
    defaulted: RefCell<BTreeSet<String>>,
}
impl<'a> Fields<'a> {
    fn new(params: &'a BTreeMap<String, PValue>) -> Self {
        let defaults = SYSTEM_DEFAULTS.iter().map(|(k, v)| (*k, pp::parse(v))).collect();
        Self { params, defaults, defaulted: RefCell::default() }
    }
    fn get(&self, name: &str) -> Option<&'a PValue> {
        self.params.get(name)
    }
    /// The base value of `name`, falling back to the system default (recorded in `defaulted`).
    fn base(&self, name: &str) -> Option<PValue> {
        if let Some(base) = self.get(name).and_then(PValue::base) {
            return Some(base.clone());
        }
        let default = self.defaults.get(name)?;
        self.defaulted.borrow_mut().insert(name.to_owned());
        default.base().cloned()
    }
    fn num(&self, name: &str) -> f64 {
        self.base(name).and_then(|b| b.num()).unwrap_or(0.0)
    }
    fn flag(&self, name: &str) -> bool {
        self.base(name).and_then(|b| b.bool()).unwrap_or(false)
    }
    fn text(&self, name: &str) -> Option<String> {
        self.base(name).and_then(|b| b.text().map(str::to_owned)).filter(|t| !t.is_empty())
    }
    /// A number only when the attribute is present (its default base otherwise), else null.
    fn present_num(&self, name: &str) -> Value {
        if self.get(name).is_some() { json!(self.num(name)) } else { Value::Null }
    }
    fn modifier(&self, name: &str, modifier: &str) -> Option<&'a PValue> {
        self.get(name)?.get(modifier)
    }
    fn random(&self, name: &str) -> Value {
        self.modifier(name, "Random").and_then(PValue::num).map_or(Value::Null, |r| json!(r))
    }
    fn curve(&self, name: &str, modifier: &str) -> Value {
        self.modifier(name, modifier).map_or(Value::Null, |c| json!(c.curve()))
    }
    fn curve_rgb(&self, name: &str, modifier: &str) -> Value {
        self.modifier(name, modifier).map_or(Value::Null, |c| json!(c.curve_rgb()))
    }
    fn positive(&self, name: &str) -> Value {
        let n = self.num(name);
        if n > 0.0 { json!(n) } else { Value::Null }
    }
    /// `{min, max}` of an angle with a `Random` fraction (`Random=1`: anywhere from 0 to the value).
    fn angle(&self, name: &str) -> Value {
        if self.get(name).is_none() {
            return Value::Null;
        }
        let max = self.num(name);
        let random = self.modifier(name, "Random").and_then(PValue::num).unwrap_or(0.0);
        json!({ "min": max * (1.0 - random), "max": max })
    }
    fn vec3(&self, name: &str) -> Option<[f64; 3]> {
        self.get(name).map(|v| v.vec3([0.0; 3]))
    }
}

/// The archive `.dds` of an authored texture path (`textures/….tif` → `Data/Textures/….dds`, archive case); the
/// authored path itself when that is in the archive; None when neither is.
pub(crate) fn archive_texture(archive: &dyn Archive, authored: &str) -> Option<String> {
    let path = authored.trim().replace('\\', "/");
    if path.is_empty() {
        return None;
    }
    let path = if path.len() >= 5 && path[..5].eq_ignore_ascii_case("data/") { path } else { format!("Data/{path}") };
    let stem_end = path.rfind('.').filter(|&d| d > path.rfind('/').unwrap_or(0)).unwrap_or(path.len());
    [format!("{}.dds", &path[..stem_end]), path].iter().find_map(|p| archive.find(p))
}

fn texture_json(archive: &dyn Archive, f: &Fields, name: &str) -> (Value, Value) {
    match f.get(name).and_then(PValue::text).filter(|t| !t.is_empty()) {
        Some(authored) => (archive_texture(archive, authored).map_or(Value::Null, Value::from), json!(authored)),
        None => (Value::Null, Value::Null),
    }
}

fn emitter_json(
    archive: &dyn Archive,
    node: &Node,
    parent: i64,
    params: &BTreeMap<String, PValue>,
    inherited: &[String],
) -> Value {
    let f = Fields::new(params);
    let mode = if f.num("EmitPerDistance") > 0.0 {
        "per_distance"
    } else if f.flag("Continuous") {
        "continuous"
    } else {
        "burst"
    };
    let (texture, texture_source) = texture_json(archive, &f, "Texture");
    let (normal_texture, _) = texture_json(archive, &f, "NormalTexture");
    let (flow_texture, _) = texture_json(archive, &f, "FlowTexture");
    let anim = f.get("TextureAnimation");
    let tiling = f.get("TextureTiling");
    let sub = |v: Option<&PValue>, key: &str| v.and_then(|v| v.get(key)).and_then(PValue::num);
    let tiles = match (sub(anim, "NumFramesU"), sub(anim, "NumFramesV")) {
        (None, None) => [sub(tiling, "TilesX").unwrap_or(1.0), sub(tiling, "TilesY").unwrap_or(1.0)],
        (u, v) => [u.unwrap_or(1.0), v.unwrap_or(1.0)],
    };
    let anim_json = {
        let from_animation = sub(anim, "NumFramesU").is_some() || sub(anim, "NumFramesV").is_some();
        let frames = if from_animation {
            tiles[0] * tiles[1]
        } else {
            sub(tiling, "AnimFramesCount").unwrap_or(tiles[0] * tiles[1])
        };
        let bool_of = |v: Option<&PValue>, key: &str| v.and_then(|v| v.get(key)).and_then(PValue::bool);
        if frames <= 1.0 {
            Value::Null
        } else if from_animation || tiling.is_none() {
            let rate = sub(anim, "AnimRate").unwrap_or(1.0);
            let lifetime_rate = bool_of(anim, "LifetimeRate").unwrap_or(true);
            let first = match (sub(anim, "FirstFrameIntervalStart"), sub(anim, "FirstFrameIntervalEnd")) {
                (None, None) => Value::Null,
                (s, e) => json!([s.unwrap_or(0.0), e.unwrap_or(0.0)]),
            };
            json!({
                "frames": frames, "fps": if lifetime_rate { Value::Null } else { json!(rate) }, "rate": rate,
                "lifetime_rate": lifetime_rate, "cycle": bool_of(anim, "Loop").unwrap_or(false),
                "blend": bool_of(anim, "FrameBlend").unwrap_or(false), "first_frame": first,
            })
        } else {
            let fps = sub(tiling, "AnimFramerate").unwrap_or(0.0);
            let cycle = tiling.and_then(|t| t.get("AnimCycle")).and_then(PValue::text).is_some_and(|c| c != "Once");
            json!({
                "frames": frames, "fps": if fps > 0.0 { json!(fps) } else { Value::Null }, "rate": fps,
                "lifetime_rate": fps <= 0.0, "cycle": cycle,
                "blend": bool_of(tiling, "AnimBlend").unwrap_or(false), "first_frame": Value::Null,
            })
        }
    };
    let light = f.get("LightSource").map_or(Value::Null, |l| {
        let radius = l.get("Radius");
        let intensity = l.get("Intensity");
        let curve = |v: Option<&PValue>, m: &str| v.and_then(|v| v.get(m)).map_or(Value::Null, |c| json!(c.curve()));
        json!({
            "radius_m": radius.and_then(PValue::num).unwrap_or(0.0),
            "intensity": intensity.map_or(Some(1.0), PValue::num).unwrap_or(1.0),
            "random": intensity.and_then(|i| i.get("Random")).and_then(PValue::num),
            "age_curve": curve(intensity, "ParticleAge"),
            "strength_curve": curve(intensity, "EmitterStrength"),
            "radius_age_curve": curve(radius, "ParticleAge"),
            "soft_bulb": l.get("SoftBulb").and_then(PValue::bool).unwrap_or(false),
            "affects_this_area_only": l.get("AffectsThisAreaOnly").and_then(PValue::bool).unwrap_or(false),
        })
    });
    let spline = match f.get("SplineGuided") {
        Some(s) if f.flag("SplineGuided") => {
            let noise = s.get("SplineNoise");
            let amplitude = noise.and_then(|n| n.get("Amplitude")).map_or([0.0; 3], |a| a.vec3([0.0; 3]));
            json!({
                "type": s.get("Type").and_then(PValue::text),
                "curvature": s.get("SplineCurvature").and_then(PValue::num),
                "noise_enabled": noise.and_then(PValue::bool).unwrap_or(false),
                "noise_amplitude": [amplitude[0], amplitude[1]],
                "noise_amplitude_xyz": amplitude,
                "raw": pp::to_attr(s),
            })
        }
        _ => Value::Null,
    };
    let turbulence_keys = ["Turbulence3DSpeed", "TurbulenceSize", "TurbulenceSpeed", "VortexTurbulence", "CurlNoise"];
    let turbulence = if turbulence_keys.iter().any(|k| f.get(k).is_some()) {
        let raw = |k: &str| f.get(k).map(pp::to_attr);
        json!({
            "speed_3d": f.get("Turbulence3DSpeed").and_then(PValue::num),
            "size": f.get("TurbulenceSize").and_then(PValue::num),
            "speed": f.get("TurbulenceSpeed").and_then(PValue::num),
            "vortex": raw("VortexTurbulence"),
            "curl_noise": raw("CurlNoise"),
        })
    } else {
        Value::Null
    };
    let color_random = f.modifier("Color", "Random");
    let emission_size = f.vec3("EmissionSize");
    let raw: Map<String, Value> = own_params(node).into_iter().map(|(k, v)| (k, Value::String(v))).collect();
    let mut emitter = json!({
        "path": node.path,
        "name": node.element.attr("Name").unwrap_or_default(),
        "parent": parent,
        "guid": node.element.attr("GUID").unwrap_or_default(),
        "enabled": f.flag("Enabled"),
        "inheritance": params.get("Inheritance").and_then(PValue::text).unwrap_or("System"),
        "system": f.text("ParticleSystem"),
        "spawn": {
            "mode": mode,
            "count": f.num("Count"),
            "count_random": f.modifier("Count", "Random").and_then(PValue::num).unwrap_or(0.0),
            "emitter_lifetime": f.positive("EmitterLifeTime"),
            "pulse_period": f.positive("PulsePeriod"),
            "delay": f.num("SpawnDelay"),
            "per_distance_per_m": f.positive("EmitPerDistance"),
            "spawn_on_parent": f.get("Spawn").and_then(PValue::text),
            "count_strength_curve": f.curve("Count", "EmitterStrength"),
        },
        "lifetime": {
            "value": f.num("ParticleLifeTime"),
            "random": f.modifier("ParticleLifeTime", "Random").and_then(PValue::num).unwrap_or(0.0),
            "strength_curve": f.curve("ParticleLifeTime", "EmitterStrength"),
        },
        "size": {
            "value": f.num("Size"),
            "random": f.modifier("Size", "Random").and_then(PValue::num).unwrap_or(0.0),
            "age_curve": f.curve("Size", "ParticleAge"),
            "strength_curve": f.curve("Size", "EmitterStrength"),
        },
        "aspect": f.present_num("Aspect"),
    });
    // Two literals: one is past serde_json macro recursion limit.
    let rest = json!({
        "color": {
            "rgb": f.get("Color").map_or([1.0; 3], |c| c.vec3([1.0; 3])),
            "random": color_random.and_then(PValue::num),
            "random_hue": color_random.and_then(|r| r.get("RandomHue")).and_then(PValue::bool).unwrap_or(false),
            "age_curve": f.curve_rgb("Color", "ParticleAge"),
            "strength_curve": f.curve_rgb("Color", "EmitterStrength"),
        },
        "alpha": {
            "value": f.num("Alpha"),
            "random": f.random("Alpha"),
            "age_curve": f.curve("Alpha", "ParticleAge"),
            "strength_curve": f.curve("Alpha", "EmitterStrength"),
        },
        "speed": {
            "value": f.num("Speed"),
            "random": f.modifier("Speed", "Random").and_then(PValue::num).unwrap_or(0.0),
            "strength_curve": f.curve("Speed", "EmitterStrength"),
        },
        "emit_angle_deg": f.angle("EmitAngle"),
        "focus_angle_deg": f.angle("FocusAngle"),
        "gravity_scale": f.num("GravityScale"),
        "air_resistance": f.num("AirResistance"),
        "drag": f.get("Drag").and_then(PValue::num),
        "inherit_velocity": f.num("InheritVelocity"),
        "turbulence": turbulence,
        "position_offset_m": f.vec3("PositionOffset").unwrap_or_default(),
        "random_offset_m": f.vec3("RandomOffset").or(emission_size).unwrap_or_default(),
        "emission_size_m": emission_size,
        "facing": f.text("Facing"),
        "orient_to_velocity": f.flag("OrientToVelocity"),
        "blend": f.text("BlendType"),
        "emissive_lighting": f.present_num("EmissiveLighting"),
        "diffuse_lighting": f.num("DiffuseLighting"),
        "texture": texture,
        "texture_source": texture_source,
        "normal_texture": normal_texture,
        "flow_texture": flow_texture,
        "material": f.get("Material").and_then(PValue::text).filter(|t| !t.is_empty()),
        "geometry": f.get("Geometry").and_then(PValue::text).filter(|t| !t.is_empty()),
        "tiles": if tiles == [1.0, 1.0] { Value::Null } else { json!(tiles) },
        "anim": anim_json,
        "light": light,
        "flare": f.get("Flare").and_then(PValue::text).filter(|t| !t.is_empty()),
        "flare_enabled": f.flag("FlareEnabled"),
        "spline": spline,
        "raw": raw,
        "inherited": inherited,
    });
    if let (Some(emitter), Value::Object(rest)) = (emitter.as_object_mut(), rest) {
        emitter.extend(rest);
    }
    emitter["defaulted"] = json!(f.defaulted.into_inner());
    emitter
}

/// A MaterialEffects FXLib entry `<FXLib>:<effect>` as `{library, effect, attributes, particles, audio}`.
pub(crate) fn material_effect_json(archive: &dyn Archive, library_and_effect: &str) -> Result<Value> {
    let (library, effect) = library_and_effect
        .trim()
        .split_once(':')
        .filter(|(l, e)| !l.trim().is_empty() && !e.trim().is_empty())
        .ok_or_else(|| anyhow!("material effect {library_and_effect:?} is not `<FXLib>:<effect>`"))?;
    let (library, effect) = (library.trim(), effect.trim());
    let path = format!("{FXLIB_DIR}{library}.xml");
    let root = read_element(archive, &path)?;
    let entry = root
        .children_named("Effect")
        .find(|e| e.attr("name").is_some_and(|n| n.eq_ignore_ascii_case(effect)))
        .with_context(|| format!("{path} has no effect {effect:?}"))?;
    let attrs = |e: &Element, skip: &str| -> Map<String, Value> {
        e.attrs.iter().filter(|(k, _)| k != skip).map(|(k, v)| (k.clone(), Value::String(v.clone()))).collect()
    };
    let (mut particles, mut audio, mut groups) = (Vec::new(), Vec::new(), 0usize);
    let mut visit = |e: &Element, group: Option<usize>| match e.tag.as_str() {
        "Particle" => {
            for name in e.children_named("Name") {
                let mut attributes = attrs(e, "");
                attributes.extend(attrs(name, ""));
                particles.push(json!({
                    "reference": name.text,
                    "attributes": attributes,
                    "direction": e.children_named("Direction").next().map(|d| d.text.clone()),
                    "random_group": group,
                }));
            }
        }
        "Audio" => {
            let mut a = attrs(e, "");
            a.insert("random_group".into(), json!(group));
            audio.push(Value::Object(a));
        }
        _ => {}
    };
    for child in &entry.children {
        if child.tag == "RandEffect" {
            for inner in &child.children {
                visit(inner, Some(groups));
            }
            groups += 1;
        } else {
            visit(child, None);
        }
    }
    Ok(json!({
        "library": library,
        "library_path": archive.find(&path).unwrap_or(path),
        "effect": entry.attr("name"),
        "attributes": attrs(entry, "name"),
        "particles": particles,
        "audio": audio,
        "random_groups": groups,
    }))
}

impl Session {
    /// The particle effect `reference` (`<GUID>|<library>.<effect>`) with all its emitters, as JSON (see the
    /// `particles` module docs for the shape, inheritance and defaults).
    pub fn particle_effect(&self, reference: &str) -> Result<Value> {
        effect_json(&self.loaded()?.p4k, reference)
    }
    /// A MaterialEffects FXLib entry (`<FXLib>:<effect>`, e.g. `sc_ships_surface_impacts:Metal_Small`) as
    /// `{library, effect, attributes, particles: [{reference, attributes, direction, random_group}], audio}`.
    pub fn material_effect(&self, library_and_effect: &str) -> Result<Value> {
        material_effect_json(&self.loaded()?.p4k, library_and_effect)
    }
}

#[cfg(test)]
#[path = "particles_tests.rs"]
mod tests;
