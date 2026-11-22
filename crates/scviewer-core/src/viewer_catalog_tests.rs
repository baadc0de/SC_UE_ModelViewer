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

#[test]
fn model_groups_do_not_depend_on_record_order() {
    let tagged = |name: &str, models: &[&str]| {
        let mut v = ship(name, "d/big.xml");
        v.tags = models.iter().map(|m| format!("Ship.Model.{m}")).collect();
        v
    };
    let (a, b, ab, plain) = (tagged("Big_A", &["A"]), tagged("Big_B", &["B"]), tagged("Big_AB", &["A", "B"]), ship("Big", "d/big.xml"));
    let orders = [
        vec![a.clone(), b.clone(), ab.clone(), plain.clone()],
        vec![ab.clone(), a.clone(), b.clone(), plain.clone()],
        vec![plain.clone(), b.clone(), a.clone(), ab.clone()],
    ];
    for records in orders {
        // A and B meet through AB; the untagged member joins that group: one row.
        assert_eq!(pick_vehicles(&records).len(), 1, "{:?}", names(&records));
    }
    // Apart without the bridge; the untagged member joins one of them, whatever the order.
    let one = pick_vehicles(&[plain.clone(), a.clone(), b.clone()]);
    let two = pick_vehicles(&[b, a, plain]);
    let sorted = |v: &[VehicleRecord]| { let mut n: Vec<String> = v.iter().map(|v| v.name.clone()).collect(); n.sort(); n };
    assert_eq!(sorted(&one), sorted(&two));
    assert_eq!(one.len(), 2);
}

#[test]
fn placeholder_family_blank_text_and_unreadable_evidence_leave() {
    let mut blank = ship("Blank", "d/blank.xml");
    blank.label = Some("  ".into()); // R2: resolves to nothing
    let mut unread = ship("Unread", "d/unread.xml");
    unread.unreadable = "tags do not read".into(); // R3 cannot judge it
    let kept = pick_vehicles(&[blank, unread, ship("Plain", "d/plain.xml")]);
    assert_eq!(names(&kept), ["Plain"]);
    assert!(super::placeholder("@LOC_SOMETHING_NEW") && super::placeholder("") && !super::placeholder("@vehicle_NameTest"));
}

#[test]
fn crew_evidence_tells_absent_from_unreadable() {
    use super::crew_reference;
    use serde_json::json;
    let empty = "00000000-0000-0000-0000-000000000000";
    // No component, no reference, a null or empty one: no crew manifest.
    assert_eq!(crew_reference(None, false), Ok(None));
    assert_eq!(crew_reference(Some(&json!({})), false), Ok(None));
    assert_eq!(crew_reference(Some(&json!({"crewManifest": null})), false), Ok(None));
    assert_eq!(crew_reference(Some(&json!({"crewManifest": empty})), false), Ok(None));
    // A reference to read.
    let id = "3db8fac5-9177-4373-a976-0730eff112fe";
    assert_eq!(crew_reference(Some(&json!({"crewManifest": id})), false).unwrap().map(|g| g.to_string()), Some(id.to_owned()));
    // Cannot be told: the query failed, or the reference is malformed.
    assert!(crew_reference(None, true).is_err());
    assert!(crew_reference(Some(&json!({"crewManifest": "not-a-guid"})), false).is_err());
    assert!(crew_reference(Some(&json!({"crewManifest": 7})), false).is_err());
}

#[test]
fn rows_do_not_depend_on_record_order() {
    use super::{ContainerRecord, container_rows, vehicle_rows};
    let rec = |path: &str, size_id: i64, size: &str, places: &[&str], name: &str| ContainerRecord {
        path: path.into(),
        size_id,
        size: size.into(),
        places: places.iter().map(|p| p.to_string()).collect(),
        name: name.into(),
    };
    // One container from three records: places merged, the smallest named size, an unplaced record as detail-less.
    let records = vec![
        rec("c/one.socpak", 2, "Medium", &["Beta", "Alpha"], "One_B"),
        rec("c/one.socpak", 1, "Small", &["Gamma"], "One_A"),
        rec("c/one.socpak", 3, "", &[], "One_C"),
        rec("c/two.socpak", 1, "Small", &[], "Two_Z"),
        rec("c/two.socpak", 1, "Small", &[], "Two_Y"),
    ];
    let forward = container_rows(records.clone());
    let backward = container_rows(records.into_iter().rev().collect());
    assert_eq!(forward, backward);
    let one = forward.iter().find(|r| r.id == "c/one.socpak").unwrap();
    assert_eq!((one.label.as_str(), one.group.as_str(), one.detail.as_str()), ("Alpha, Beta, Gamma", "Small", "One_A, One_B"));
    let two = forward.iter().find(|r| r.id == "c/two.socpak").unwrap();
    assert_eq!((two.label.as_str(), two.detail.as_str()), ("Two_Y", ""));
    // A size with no name still orders by its id: an unnamed size 4 after a named size 1.
    let sized = container_rows(vec![rec("c/big.socpak", 4, "", &["Zeta"], "Big"), rec("c/small.socpak", 1, "Small", &["Zeta"], "Small")]);
    assert_eq!(sized.iter().map(|r| r.id.as_str()).collect::<Vec<_>>(), ["c/small.socpak", "c/big.socpak"]);

    // Vehicle rows: two groups with the same manufacturer and label sort by id.
    let mut a = ship("Same_A", "d/a.xml");
    let mut b = ship("Same_B", "d/b.xml");
    a.label = Some("Same".into());
    b.label = Some("Same".into());
    assert_eq!(vehicle_rows(&[a.clone(), b.clone()]), vehicle_rows(&[b, a]));
}
