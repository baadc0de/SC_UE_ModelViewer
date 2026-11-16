//! The parameter syntax, reference split, GUID matching, emitter paths, inheritance and defaults, and the
//! material-effect lookup on in-memory fixtures; with game data, the tractor-beam and box-impact effects.
use super::*;
use crate::archive::MemArchive;
use crate::particle_params::{merge, parse, to_attr};

const LIBRARY: &str = r#"<ParticleLibrary Name="lib" SandboxVersion="0.0.0.0" ParticleVersion="50">
  <Particles Name="fx.beam" GUID="aaaaaaaa-0000-0000-0000-000000000001">
    <Params Inheritance="System" Count="3,DistanceLOD=(v=1;t=1,flags=4)" ParticleSystem="CryEngine" ParticleLifeTime="0.6,Random=0.25" PositionOffset="y=0.3,z=0.1" BlendType="Additive" Texture="textures/vfx/glows/Glow.tif" Color="(x=0.5,y=0.25,z=1),EmitterStrength=(t=0.06,v=(x=0.067,y=0.01);t=0.408,v=(x=0.1,y=0.2,z=0.8))" LightSource="Radius=10,SoftBulb=true,Intensity=(50,EmitterStrength=(v=0.408;t=1,v=1),ParticleAge=(;t=0.5,v=1;t=1))" Speed="0" Size="0.8,ParticleAge=(v=0.27,flags=32;t=1,v=1,flags=4)" />
    <Childs>
      <Particles Name="core" GUID="aaaaaaaa-0000-0000-0000-000000000002">
        <Params Inheritance="System" ParticleSystem="RParticleGPU" Continuous="false" SplineGuided="true,Type=Stateless,SplineCurvature=0.6,SplineNoise=(true,NoiseType 1st=Texture,Amplitude=(x=0.015,y=0.007))" EmitPerDistance="90,DistanceLOD=(v=1;t=1,flags=4)" TextureAnimation="NumFramesU=2,NumFramesV=2,LifetimeRate=false,AnimRate=12,Loop=true" Alpha="0.2,ParticleAge=(v=0.898;t=0.13,v=1;t=1,v=1)" />
        <Childs>
          <Particles Name="thin" GUID="aaaaaaaa-0000-0000-0000-000000000003">
            <Params Inheritance="Parent" SplineGuided="SplineCurvature=0.5" PositionOffset="x=-0.1" Size="0.02" Alpha="Random=0.5" />
          </Particles>
        </Childs>
      </Particles>
    </Childs>
  </Particles>
  <Particles Name="fx.impact" GUID="aaaaaaaa-0000-0000-0000-000000000004">
    <Params Inheritance="System" Continuous="false" Count="15,Random=1" EmitAngle="90,Random=1" Spawn="FromParentBirth" Texture="textures/missing.tif" />
  </Particles>
</ParticleLibrary>"#;

fn archive() -> MemArchive {
    MemArchive::default()
        .with(r"Data\Libs\Particles\Gadgets\Lib.xml", LIBRARY)
        .with(r"Data\Textures\VFX\Glows\glow.dds", b"DDS ".to_vec())
}

fn effect(reference: &str) -> Value {
    effect_json(&archive(), reference).unwrap()
}

#[test]
fn parameter_syntax_reads_base_values_modifiers_and_curves() {
    let count = parse("3,DistanceLOD=(v=1;t=1,flags=4)");
    assert_eq!(count.num(), Some(3.0));
    assert_eq!(count.get("DistanceLOD").unwrap().curve(), vec![[0.0, 1.0], [1.0, 0.0]]);

    let lifetime = parse("0.6,Random=0.102273");
    assert_eq!((lifetime.num(), lifetime.get("Random").and_then(PValue::num)), (Some(0.6), Some(0.102273)));

    let light = parse("Radius=10,SoftBulb=true,Intensity=(50,EmitterStrength=(v=0.408;t=1,v=1),ParticleAge=(;t=0.5,v=1;t=1))");
    assert_eq!(light.num(), None);
    assert_eq!(light.get("Radius").and_then(PValue::num), Some(10.0));
    assert_eq!(light.get("SoftBulb").and_then(PValue::bool), Some(true));
    let intensity = light.get("Intensity").unwrap();
    assert_eq!(intensity.num(), Some(50.0));
    // An empty key is (0, 0); a missing v is 0.
    assert_eq!(intensity.get("ParticleAge").unwrap().curve(), vec![[0.0, 0.0], [0.5, 1.0], [1.0, 0.0]]);
    assert_eq!(intensity.get("EmitterStrength").unwrap().curve(), vec![[0.0, 0.408], [1.0, 1.0]]);

    let color = parse("(x=0.21,y=0.22,z=0.23),EmitterStrength=(t=0.06,v=(x=0.067,y=0.01);t=0.408)");
    assert_eq!(color.vec3([1.0; 3]), [0.21, 0.22, 0.23]);
    assert_eq!(color.get("EmitterStrength").unwrap().curve_rgb(), vec![[0.06, 0.067, 0.01, 0.0], [0.408, 0.0, 0.0, 0.0]]);
    // A colour's omitted component keeps the base default (white), unlike a curve key's.
    assert_eq!(parse("(y=0,z=0)").vec3([1.0; 3]), [1.0, 0.0, 0.0]);
    assert_eq!(parse("y=0.3,z=0.1").vec3([0.0; 3]), [0.0, 0.3, 0.1]);
    assert_eq!(parse("Random=(0.45,RandomHue=true)").get("Random").and_then(PValue::num), Some(0.45));

    let spline = parse("true,NoiseType 1st=Texture,1stDispTexture=textures/vfx/noise/a_01.tif,Amplitude=(x=0.015,y=0.007)");
    assert_eq!(spline.bool(), Some(true));
    assert_eq!(spline.get("NoiseType 1st").and_then(PValue::text), Some("Texture"));
    assert_eq!(spline.get("1stDispTexture").and_then(PValue::text), Some("textures/vfx/noise/a_01.tif"));

    for text in ["0.8,ParticleAge=(v=0.27,flags=32;t=1,v=1,flags=4)", "Radius=10,Intensity=(0.45,ParticleAge=(;t=0.498,v=1;t=1))"] {
        assert_eq!(to_attr(&parse(text)), text);
    }
    // Malformed text never panics.
    for text in ["", ")", "((", "a=(", "=5", "x=1)),y=2", ";;", "t=,v="] {
        let _ = parse(text).curve();
    }
    assert_eq!(parse("x=1)),y=2").vec3([0.0; 3]), [1.0, 2.0, 0.0]);
}

#[test]
fn merge_layers_a_child_over_its_parent_field_by_field() {
    let parent = parse("true,Type=Stateless,SplineCurvature=0.6,SplineNoise=(true,Amplitude=(x=0.015,y=0.007))");
    let child = parse("SplineCurvature=0.5,SplineNoise=(Amplitude=(x=0.04))");
    let merged = merge(&parent, &child);
    assert_eq!(to_attr(&merged), "true,Type=Stateless,SplineCurvature=0.5,SplineNoise=(true,Amplitude=(x=0.04,y=0.007))");
    // A child's base replaces the parent's; a curve replaces the whole curve.
    assert_eq!(to_attr(&merge(&parse("0.8,ParticleAge=(v=0.27;t=1,v=1)"), &parse("0.02"))), "0.02,ParticleAge=(v=0.27;t=1,v=1)");
    assert_eq!(to_attr(&merge(&parse("1,ParticleAge=(v=0.27;t=1,v=1)"), &parse("ParticleAge=(;t=1)"))), "1,ParticleAge=(;t=1)");
    assert_eq!(to_attr(&merge(&parse("Random=0.3"), &parse("2"))), "2,Random=0.3");
}

#[test]
fn references_split_at_the_first_dot_after_the_last_slash() {
    let r = split_reference("2c211ca4-c915-b0c7-5d0e-58fe5c503a16|Gadgets/tractor_beam.fps.beam_only").unwrap();
    assert_eq!(r.guid.as_deref(), Some("2c211ca4-c915-b0c7-5d0e-58fe5c503a16"));
    assert_eq!((r.library.as_str(), r.effect.as_str()), ("Gadgets/tractor_beam", "fps.beam_only"));
    let r = split_reference("tractorbeam/fps_tractorbeam.FPS_Tractor_Beam.Muzzleflash_1st").unwrap();
    assert_eq!((r.guid, r.library.as_str(), r.effect.as_str()), (None, "tractorbeam/fps_tractorbeam", "FPS_Tractor_Beam.Muzzleflash_1st"));
    let r = split_reference(r"|lib.a.b").unwrap();
    assert_eq!((r.guid, r.library.as_str(), r.effect.as_str()), (None, "lib", "a.b"));
    for bad in ["Gadgets/tractor_beam", "x|Gadgets/.fx", "lib.", ""] {
        assert!(split_reference(bad).is_err(), "{bad}");
    }
}

#[test]
fn an_effect_lists_its_emitters_depth_first_with_paths_and_parents() {
    let fx = effect("aaaaaaaa-0000-0000-0000-000000000001|Gadgets/lib.fx.beam");
    assert_eq!(fx["effect"], "fx.beam");
    assert_eq!(fx["library"], "Gadgets/lib");
    assert_eq!(fx["library_path"], "Data/Libs/Particles/Gadgets/Lib.xml");
    assert_eq!(fx["matched_by"], "name");
    let emitters = fx["emitters"].as_array().unwrap();
    let paths: Vec<_> = emitters.iter().map(|e| (e["path"].as_str().unwrap(), e["name"].as_str().unwrap(), e["parent"].as_i64().unwrap())).collect();
    assert_eq!(paths, vec![("fx.beam", "fx.beam", -1), ("fx.beam.core", "core", 0), ("fx.beam.core.thin", "thin", 1)]);
    assert_eq!(emitters[2]["guid"], "aaaaaaaa-0000-0000-0000-000000000003");

    // A child can be named directly (case-insensitively); its subtree starts at parent -1.
    let core = effect("gadgets/LIB.FX.BEAM.CORE");
    let emitters = core["emitters"].as_array().unwrap();
    assert_eq!(emitters.len(), 2);
    assert_eq!((emitters[0]["parent"].as_i64(), emitters[1]["parent"].as_i64()), (Some(-1), Some(0)));
}

#[test]
fn the_guid_must_match_and_wins_over_the_name() {
    // The name finds fx.beam, the GUID fx.impact: the GUID wins.
    let fx = effect("aaaaaaaa-0000-0000-0000-000000000004|Gadgets/lib.fx.beam");
    assert_eq!((fx["effect"].as_str(), fx["matched_by"].as_str()), (Some("fx.impact"), Some("guid")));
    // A renamed effect is still found by its GUID.
    let fx = effect("AAAAAAAA-0000-0000-0000-000000000003|Gadgets/lib.renamed");
    assert_eq!(fx["effect"], "fx.beam.core.thin");
    let error = effect_json(&archive(), "ffffffff-0000-0000-0000-000000000000|Gadgets/lib.fx.beam").unwrap_err().to_string();
    assert!(error.contains("has GUID aaaaaaaa-0000-0000-0000-000000000001"), "{error}");
    assert!(effect_json(&archive(), "Gadgets/lib.fx.none").is_err());
    assert!(effect_json(&archive(), "Gadgets/none.fx.beam").is_err());
}

#[test]
fn emitters_map_params_with_inheritance_and_system_defaults() {
    let fx = effect("Gadgets/lib.fx.beam");
    let [beam, core, thin] = [&fx["emitters"][0], &fx["emitters"][1], &fx["emitters"][2]];

    assert_eq!(beam["system"], "CryEngine");
    assert_eq!(beam["spawn"]["mode"], "continuous"); // Continuous defaults to true
    assert_eq!(beam["spawn"]["count"], 3.0);
    assert_eq!(beam["spawn"]["emitter_lifetime"], Value::Null);
    assert_eq!(beam["lifetime"], json!({"value": 0.6, "random": 0.25, "strength_curve": null}));
    assert_eq!(beam["size"]["value"], 0.8);
    assert_eq!(beam["size"]["age_curve"], json!([[0.0, 0.27], [1.0, 1.0]]));
    assert_eq!(beam["color"]["rgb"], json!([0.5, 0.25, 1.0]));
    assert_eq!(beam["color"]["strength_curve"], json!([[0.06, 0.067, 0.01, 0.0], [0.408, 0.1, 0.2, 0.8]]));
    assert_eq!(beam["alpha"]["value"], 1.0); // default
    assert_eq!(beam["speed"]["value"], 0.0);
    assert_eq!(beam["position_offset_m"], json!([0.0, 0.3, 0.1]));
    assert_eq!(beam["blend"], "Additive");
    assert_eq!(beam["facing"], "Camera");
    assert_eq!(beam["texture"], "Data/Textures/VFX/Glows/glow.dds");
    assert_eq!(beam["texture_source"], "textures/vfx/glows/Glow.tif");
    assert_eq!(beam["light"]["radius_m"], 10.0);
    assert_eq!(beam["light"]["intensity"], 50.0);
    assert_eq!(beam["light"]["age_curve"], json!([[0.0, 0.0], [0.5, 1.0], [1.0, 0.0]]));
    assert_eq!(beam["light"]["soft_bulb"], true);
    assert_eq!(beam["spline"], Value::Null);
    assert_eq!(beam["raw"]["Count"], "3,DistanceLOD=(v=1;t=1,flags=4)");
    let defaulted: Vec<_> = beam["defaulted"].as_array().unwrap().iter().filter_map(Value::as_str).collect();
    assert!(defaulted.contains(&"Alpha") && defaulted.contains(&"Facing") && !defaulted.contains(&"Speed"), "{defaulted:?}");

    assert_eq!(core["spawn"]["mode"], "per_distance");
    assert_eq!(core["spawn"]["per_distance_per_m"], 90.0);
    assert_eq!(core["tiles"], json!([2.0, 2.0]));
    assert_eq!(core["anim"]["frames"], 4.0);
    assert_eq!(core["anim"]["fps"], 12.0);
    assert_eq!(core["anim"]["cycle"], true);
    assert_eq!(core["spline"]["curvature"], 0.6);
    assert_eq!(core["spline"]["noise_amplitude"], json!([0.015, 0.007]));
    assert_eq!(core["alpha"]["age_curve"], json!([[0.0, 0.898], [0.13, 1.0], [1.0, 1.0]]));
    assert_eq!(core["texture"], Value::Null);

    // Inheritance="Parent": omitted attributes are the parent's, partial ones merged over it.
    assert_eq!(thin["inheritance"], "Parent");
    assert_eq!(thin["system"], "RParticleGPU");
    assert_eq!(thin["spawn"]["mode"], "per_distance");
    assert_eq!(thin["spline"]["curvature"], 0.5);
    assert_eq!(thin["spline"]["type"], "Stateless");
    assert_eq!(thin["size"]["value"], 0.02);
    assert_eq!(thin["alpha"]["value"], 0.2);
    assert_eq!(thin["alpha"]["random"], 0.5);
    assert_eq!(thin["position_offset_m"], json!([-0.1, 0.0, 0.0]));
    assert_eq!(thin["raw"], json!({"Inheritance": "Parent", "SplineGuided": "SplineCurvature=0.5", "PositionOffset": "x=-0.1", "Size": "0.02", "Alpha": "Random=0.5"}));
    let inherited: Vec<_> = thin["inherited"].as_array().unwrap().iter().filter_map(Value::as_str).collect();
    for name in ["SplineGuided", "EmitPerDistance", "ParticleSystem", "Alpha", "TextureAnimation"] {
        assert!(inherited.contains(&name), "{name} in {inherited:?}");
    }
    assert!(!inherited.contains(&"Size"), "{inherited:?}");

    let impact = &effect("Gadgets/lib.fx.impact")["emitters"][0];
    assert_eq!(impact["spawn"]["mode"], "burst");
    assert_eq!((impact["spawn"]["count"].as_f64(), impact["spawn"]["count_random"].as_f64()), (Some(15.0), Some(1.0)));
    assert_eq!(impact["spawn"]["spawn_on_parent"], "FromParentBirth");
    assert_eq!(impact["emit_angle_deg"], json!({"min": 0.0, "max": 90.0}));
    assert_eq!(impact["focus_angle_deg"], Value::Null);
    assert_eq!(impact["speed"]["value"], 1.0); // default
    // An authored texture missing from the archive: no archive path, the authored one kept.
    assert_eq!((impact["texture"].clone(), impact["texture_source"].clone()), (Value::Null, json!("textures/missing.tif")));
}

const FXLIB: &str = r#"<FXLib>
  <Effect name="Metal_Small">
    <Audio minPlayTime="0.3" trigger="Play_Metal" bIgnoreDistMult="0" />
    <Particle>
      <Name>c4b45b50-13cc-cfab-ad7e-22b0a3c2aac5|spaceships/surface_effects/Ground_Impacts.Metal.small_impact_gpu</Name>
    </Particle>
  </Effect>
  <Effect name="Sand_Medium" delay="0.005">
    <Particle>
      <Name minscale="1.5" maxscale="1.5">5029686d|Spaceships/Surface_Effects/ground_impacts.Sand.small_impact_gpu</Name>
      <Direction>Ricochet</Direction>
    </Particle>
    <RandEffect>
      <Particle><Name>a|x.one</Name></Particle>
      <Particle><Name>b|x.two</Name></Particle>
    </RandEffect>
  </Effect>
</FXLib>"#;

#[test]
fn material_effects_list_particle_references_and_audio() {
    let archive = MemArchive::default().with(r"Data\Libs\MaterialEffects\FXLibs\sc_ships_surface_impacts.xml", FXLIB);
    let metal = material_effect_json(&archive, "sc_ships_surface_impacts:metal_small").unwrap();
    assert_eq!(metal["effect"], "Metal_Small");
    assert_eq!(
        metal["particles"],
        json!([{"reference": "c4b45b50-13cc-cfab-ad7e-22b0a3c2aac5|spaceships/surface_effects/Ground_Impacts.Metal.small_impact_gpu",
                "attributes": {}, "direction": null, "random_group": null}])
    );
    assert_eq!(metal["audio"], json!([{"minPlayTime": "0.3", "trigger": "Play_Metal", "bIgnoreDistMult": "0", "random_group": null}]));

    let sand = material_effect_json(&archive, "SC_Ships_Surface_Impacts:Sand_Medium").unwrap();
    assert_eq!(sand["attributes"], json!({"delay": "0.005"}));
    let particles = sand["particles"].as_array().unwrap();
    assert_eq!(particles[0]["attributes"], json!({"minscale": "1.5", "maxscale": "1.5"}));
    assert_eq!(particles[0]["direction"], "Ricochet");
    assert_eq!((particles[1]["random_group"].as_u64(), particles[2]["random_group"].as_u64()), (Some(0), Some(0)));
    assert_eq!(sand["random_groups"], 1);

    for bad in ["sc_ships_surface_impacts:Nope", "sc_ships_surface_impacts", "none:Metal_Small", ":x"] {
        assert!(material_effect_json(&archive, bad).is_err(), "{bad}");
    }
}

fn live_session() -> Session {
    let found = starbreaker_common::discover::find_p4k().expect("Data.p4k");
    Session::open(Some(&found.path), None).unwrap()
}

fn find<'a>(fx: &'a Value, name: &str) -> &'a Value {
    fx["emitters"].as_array().unwrap().iter().find(|e| e["name"] == name).unwrap_or_else(|| panic!("no emitter {name}"))
}

#[test]
#[ignore = "needs the installed Data.p4k"]
fn live_tractor_beam_and_box_impact_effects() {
    let session = live_session();
    let beam = session.particle_effect("2c211ca4-c915-b0c7-5d0e-58fe5c503a16|Gadgets/tractor_beam.fps.beam_only").unwrap();
    assert_eq!(beam["matched_by"], "name");
    let core = find(&beam, "beam_core");
    assert_eq!(core["path"], "fps.beam_only.beam_core");
    assert_eq!(core["system"], "RParticleGPU");
    assert_eq!(core["spawn"]["mode"], "per_distance");
    assert!(core["spline"]["curvature"].as_f64().is_some());
    assert!(core["texture"].as_str().is_some_and(|t| t.starts_with("Data/") && t.to_lowercase().ends_with(".dds")), "{}", core["texture"]);
    let thin = find(&beam, "thin_core");
    assert_eq!((thin["inheritance"].as_str(), thin["parent"].as_i64()), (Some("Parent"), beam["emitters"].as_array().unwrap().iter().position(|e| e["name"] == "beam_core").map(|i| i as i64)));
    assert_eq!(thin["system"], "RParticleGPU");

    let impact = session.particle_effect("Gadgets/tractor_beam.fps.impact").unwrap();
    assert!(!impact["emitters"].as_array().unwrap().is_empty());
    let muzzle = session.particle_effect("tractorbeam/fps_tractorbeam.FPS_Tractor_Beam.Muzzleflash_1st").unwrap();
    assert!(muzzle["emitters"][0]["light"]["radius_m"].as_f64().is_some_and(|r| r > 0.0));

    let metal = session.material_effect("sc_ships_surface_impacts:Metal_Small").unwrap();
    assert_eq!(metal["audio"][0]["trigger"], "Play_SSDM_Shared_Collisions_Material_Metal");
    for (reference, flash) in [
        (metal["particles"][0]["reference"].as_str().unwrap().to_owned(), "Metal.small_impact_gpu"),
        ("8efcc38f-9eb6-3717-7fb3-ff56da87fa0e|Spaceships/Surface_Effects/ground_impacts.Metal.large_impact_gpu".to_owned(), "Metal.large_impact_gpu"),
    ] {
        let fx = session.particle_effect(&reference).unwrap();
        assert_eq!(fx["effect"], flash);
        assert_eq!(fx["emitters"][0]["spawn"]["mode"], "burst");
        let spark = find(&fx, "spark");
        assert_eq!(spark["spawn"]["spawn_on_parent"], "FromParentBirth");
        assert!(spark["texture"].as_str().is_some(), "{}", spark["texture_source"]);
        assert!(find(&fx, "smoke")["anim"]["frames"].as_f64().is_some_and(|f| f > 1.0));
    }
}
