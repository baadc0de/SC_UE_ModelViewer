//! Rules of the viewer's vehicle list (`pick_vehicles`) on synthetic records (names and tags are made up, not game data).
use super::{VehicleRecord, pick_vehicles};

fn ship(name: &str, definition: &str) -> VehicleRecord {
    VehicleRecord {
        name: name.into(),
        guid: format!("guid-{name}"),
        label: Some(format!("Test {name}")),
        insurance_class: Some(name.into()),
        definition: definition.into(),
        ..Default::default()
    }
}
fn names(v: &[VehicleRecord]) -> Vec<&str> {
    v.iter().map(|v| v.name.as_str()).collect()
}

#[test]
fn each_rule_removes_its_kind() {
    let mut hulk = ship("Hulk", "d/hulk.xml");
    hulk.insurance_class = None; // R1
    let mut unnamed = ship("Unnamed", "d/unnamed.xml");
    unnamed.label_is_placeholder = true; // R2
    let mut ai = ship("Ai", "d/ai.xml");
    ai.tags = vec!["AI.Faction.Test".into()]; // R3
    let mut crewed = ship("Crewed", "d/crewed.xml");
    crewed.crew = 2; // R3
    let mut display = ship("Display", "d/display.xml");
    display.tags = vec!["Missions.ItemType.Iasi.DisplayItem".into()]; // R4
    let mut other = ship("Other", "d/other.xml");
    other.insurance_class = Some("Base".into()); // R5: insured elsewhere, not included
    let mut included = ship("Included", "d/included.xml");
    included.insurance_class = Some("Base".into());
    included.ea_included = true; // R5 keeps it
    let kept = pick_vehicles(&[hulk, unnamed, ai, crewed, display, other, included, ship("Plain", "d/plain.xml")]);
    assert_eq!(names(&kept), ["Included", "Plain"]);
}

#[test]
fn one_row_per_definition_with_the_self_insured_member() {
    let base = ship("Base", "d/base.xml");
    let mut edition = ship("Base_Edition", "d/base.xml");
    edition.label = Some("Test Base Edition".into());
    edition.insurance_class = Some("Base".into());
    edition.ea_included = true;
    let mut model_a = ship("Big_A", "d/big.xml");
    model_a.tags = vec!["Ship.Model.A".into()];
    let mut model_b = ship("Big_B", "d/big.xml");
    model_b.tags = vec!["Ship.Model.B".into()];
    let mut modified = ship("Base_Mod", "d/base.xml");
    modified.modification = "Variant".into();
    let kept = pick_vehicles(&[edition, base, model_a, model_b, modified]);
    // The edition folds into its base; different Ship.Model tags and a modification stay apart.
    assert_eq!(names(&kept), ["Base", "Base_Mod", "Big_A", "Big_B"]);
}
