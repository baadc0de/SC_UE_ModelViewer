//! The viewer's pick lists (WP-O1-3, `Session::viewer_catalog`), chosen from game data, never from asset names:
//!
//! - **Environments**: the game's instanced hangars — `EntityClassDefinition` records with
//!   `InstancedInteriorComponentParams` and a set `SObjectContainerComponentParams.objectContainer` — labelled by their
//!   size (`InstancedInteriorComponentParams.name`) and the places that offer them
//!   (`InstancedInteriorLocationMap.locationInteriors[].defaultHangars`, places not `devOnly`).
//! - **Vehicles**: `VehicleComponentParams` records through the rules of [`pick_vehicles`] (R1 insurable, R2 named,
//!   R3 not AI-tagged nor NPC-crewed, R4 not a display item, R5 insured to itself or included, R6 one row per vehicle
//!   definition and modification), the research in docs/work-packages/WP-O1-unrealcitizen-split.md (O1-3).
//!
//! Rows are `{id, label, group, detail}` (`CatalogRow`); `id` is what the viewer loads (an archive path, an entity).
use crate::Session;
use crate::index::parse_localization;
use anyhow::Result;
use serde::Serialize;
use serde_json::Value as Json;
use starbreaker_common::CigGuid;
use starbreaker_datacore::{Database, query::compile::CompiledPath, query::record_json::{record_json, value_to_json}, types::Record};
use std::collections::{BTreeMap, HashMap};
use std::str::FromStr;

#[derive(Debug, Clone, Serialize, PartialEq)]
pub struct CatalogRow {
    pub id: String,
    pub label: String,
    pub group: String,
    pub detail: String,
}
#[derive(Debug, Clone, Serialize)]
pub struct ViewerCatalog {
    pub environments: Vec<CatalogRow>,
    pub vehicles: Vec<CatalogRow>,
}

/// One vehicle record, as the rules see it.
#[derive(Debug, Clone, Default)]
pub(crate) struct VehicleRecord {
    pub name: String,
    pub guid: String,
    pub label: Option<String>,
    /// `AttachDef.Localization.Name` is a placeholder key (`@LOC_PLACEHOLDER`, `@LOC_EMPTY`, ...).
    pub label_is_placeholder: bool,
    pub tags: Vec<String>,
    pub crew: usize,
    pub insurance_class: Option<String>,
    pub ea_included: bool,
    pub entitled: bool,
    pub definition: String,
    pub modification: String,
    pub manufacturer: String,
    pub size: Option<i64>,
    pub sub_type: String,
    pub gravlev: bool,
    pub role: String,
}
impl VehicleRecord {
    fn has_tag_under(&self, roots: &[&str]) -> bool {
        self.tags.iter().any(|t| roots.iter().any(|r| t == r || t.starts_with(&format!("{r}."))))
    }
    fn self_insured(&self) -> bool {
        self.insurance_class.as_deref().is_some_and(|c| c.eq_ignore_ascii_case(&self.name))
    }
    fn models(&self) -> Vec<&str> {
        self.tags.iter().filter(|t| t.starts_with("Ship.Model.")).map(String::as_str).collect()
    }
}

/// The vehicle rules, in order: R1 has insurance properties (salvage hulks and other dummies have none); R2 a real
/// localized name; R3 no AI tag (`AI.Faction`, `AI.Spawning`, `AI.Ship.Unmanned`) and no NPC crew manifest; R4 not a
/// mission display item; R5 insured to itself or included in the game's entity data (`ReadyToInclude`); R6 one row per
/// (vehicle definition, modification), split where members carry different `Ship.Model.*` tags. The row of a group is
/// its self-insured member, else an entitled one, else any: the one most records insure to, then the one whose name
/// begins the others', then the most tagged.
pub(crate) fn pick_vehicles(all: &[VehicleRecord]) -> Vec<VehicleRecord> {
    const AI: [&str; 3] = ["AI.Faction", "AI.Spawning", "AI.Ship.Unmanned"];
    let kept: Vec<&VehicleRecord> = all
        .iter()
        .filter(|v| v.insurance_class.is_some())
        .filter(|v| !v.label_is_placeholder && v.label.is_some())
        .filter(|v| !v.has_tag_under(&AI) && v.crew == 0)
        .filter(|v| !v.has_tag_under(&["Missions.ItemType.Iasi.DisplayItem"]))
        .filter(|v| v.self_insured() || v.ea_included)
        .collect();
    let mut insured_to: HashMap<String, usize> = HashMap::new();
    for v in all {
        if let Some(c) = &v.insurance_class {
            *insured_to.entry(c.to_lowercase()).or_default() += 1;
        }
    }
    // R6: by (definition, modification); within a key, members whose Ship.Model tags meet (or have none) stay together.
    let mut by_key: BTreeMap<(String, String), Vec<Vec<&VehicleRecord>>> = BTreeMap::new();
    for v in kept {
        let subs = by_key.entry((v.definition.to_lowercase(), v.modification.to_lowercase())).or_default();
        let m = v.models();
        match subs.iter_mut().find(|s| {
            let sm: Vec<&str> = s.iter().flat_map(|x| x.models()).collect();
            m.is_empty() || sm.is_empty() || m.iter().any(|t| sm.contains(t))
        }) {
            Some(s) => s.push(v),
            None => subs.push(vec![v]),
        }
    }
    let mut out = Vec::new();
    for group in by_key.into_values().flatten() {
        let label = |v: &VehicleRecord| v.label.clone().unwrap_or_default();
        let selves: Vec<&VehicleRecord> = group.iter().copied().filter(|v| v.self_insured()).collect();
        let mut pool = if !selves.is_empty() { selves } else { group.iter().copied().filter(|v| v.entitled).collect() };
        if pool.is_empty() {
            pool = group.clone();
        }
        let prefix_of = |v: &VehicleRecord| group.iter().filter(|o| label(o).starts_with(&label(v))).count();
        pool.sort_by(|a, b| {
            let ins = |v: &VehicleRecord| insured_to.get(&v.name.to_lowercase()).copied().unwrap_or(0);
            ins(b).cmp(&ins(a))
                .then(prefix_of(b).cmp(&prefix_of(a)))
                .then(b.tags.len().cmp(&a.tags.len()))
                .then(a.guid.cmp(&b.guid))
        });
        out.push(pool[0].clone());
    }
    out
}

/// A loc key's text; keys in `global.ini` may carry a `,P` suffix (`parse_localization` keeps it).
fn localize(loc: &HashMap<String, String>, key: &str) -> Option<String> {
    let k = key.trim().trim_start_matches('@').to_lowercase();
    if k.is_empty() {
        return None;
    }
    loc.get(&k).or_else(|| loc.get(&format!("{k},p"))).cloned()
}
fn placeholder(key: &str) -> bool {
    let k = key.trim().to_lowercase();
    k.is_empty() || ["@loc_placeholder", "@loc_empty", "@loc_uninitialized", "@loc_invalid"].contains(&k.as_str())
}
fn component<'j>(value: &'j Json, list: &str, kind: &str) -> Option<&'j Json> {
    value.get(list)?.as_array()?.iter().find(|c| c.get("__type").and_then(Json::as_str) == Some(kind))
}
fn str_at<'j>(value: &'j Json, path: &[&str]) -> Option<&'j str> {
    path.iter().try_fold(value, |v, k| v.get(k))?.as_str()
}
fn record_by_guid(db: &Database, guid: Option<&str>) -> Option<Json> {
    let id = CigGuid::from_str(guid?.trim()).ok()?;
    let record = db.record_by_id(&id)?;
    record_json(db, record).ok()
}
/// The parts of `EntityClassDefinition` records the rules read, without materializing whole records (a large vehicle
/// exceeds the record materializer's node limit): each component or static-data entry by its type, references kept as
/// GUIDs, in `record_json`'s shape (`Components`, `StaticEntityClassData`, `tags`, `__record_name`).
struct EntityParts<'d> {
    db: &'d Database<'d>,
    parts: Vec<(&'static str, CompiledPath)>,
    tags: CompiledPath,
}
impl<'d> EntityParts<'d> {
    fn new(db: &'d Database<'d>, components: &[&str], statics: &[&str]) -> Result<Self> {
        use starbreaker_datacore::query::value::Value;
        let mut parts = Vec::new();
        for (list, kinds) in [("Components", components), ("StaticEntityClassData", statics)] {
            for kind in kinds {
                parts.push((list, db.compile_rooted::<Value>(&format!("EntityClassDefinition.{list}[{kind}]"))?));
            }
        }
        Ok(Self { db, parts, tags: db.compile_rooted::<Value>("EntityClassDefinition.tags")? })
    }
    /// The main `EntityClassDefinition` records that have component `kind` (one of `components`).
    fn with<'s>(&'s self, kind_index: usize) -> impl Iterator<Item = &'d Record> + 's {
        let db = self.db;
        db.records_by_type_name("EntityClassDefinition")
            .filter(move |r| db.is_main_record(r))
            .filter(move |r| self.db.query_no_references(&self.parts[kind_index].1, r).is_ok_and(|v| !v.is_empty()))
    }
    fn json(&self, record: &Record) -> Json {
        let mut out = serde_json::Map::new();
        for (list, path) in &self.parts {
            if let Ok(values) = self.db.query_no_references(path, record) {
                let entry = out.entry(*list).or_insert_with(|| Json::Array(Vec::new()));
                if let Json::Array(items) = entry {
                    items.extend(values.iter().map(value_to_json));
                }
            }
        }
        let tags = self.db.query_no_references(&self.tags, record).unwrap_or_default();
        out.insert("tags".into(), Json::Array(tags.iter().map(value_to_json).flat_map(|v| match v {
            Json::Array(items) => items,
            other => vec![other],
        }).collect()));
        out.insert("__record_name".into(), self.db.resolve_string2(record.name_offset).into());
        Json::Object(out)
    }
}

impl Session {
    /// The viewer's pick lists (module header). Environments only where the archive has the container.
    pub fn viewer_catalog(&self) -> Result<ViewerCatalog> {
        let loaded = self.loaded()?;
        let loc = parse_localization(&loaded.p4k.read_file(r"Data\Localization\english\global.ini")?);
        let archive: HashMap<String, &String> = self.browser.environments.iter().map(|p| (p.to_lowercase(), p)).collect();
        loaded.index.with_dependent(|_, idx| -> Result<ViewerCatalog> {
            let db = idx.db;
            Ok(ViewerCatalog { environments: environments(db, &loc, &archive)?, vehicles: vehicles(db, &loc)? })
        })
    }
}

fn environments(db: &Database, loc: &HashMap<String, String>, archive: &HashMap<String, &String>) -> Result<Vec<CatalogRow>> {
    // Places by hangar record: the location map's non-dev entries.
    let mut places: HashMap<String, Vec<String>> = HashMap::new();
    for map in db.records_by_type_name("InstancedInteriorLocationMap").filter(|r| db.is_main_record(r)) {
        let Ok(map) = record_json(db, map) else { continue };
        let entries: Vec<Json> = map.get("locationInteriors").and_then(Json::as_array).into_iter().flatten()
            .filter_map(|e| if e.is_string() { record_by_guid(db, e.as_str()) } else { Some(e.clone()) })
            .collect();
        for entry in &entries {
            if entry.get("devOnly").and_then(Json::as_bool).unwrap_or(false) {
                continue;
            }
            let place = record_by_guid(db, entry.get("location").and_then(Json::as_str))
                .and_then(|l| l.get("name").and_then(Json::as_str).and_then(|k| localize(loc, k)));
            let Some(place) = place else { continue };
            for hangar in entry.get("defaultHangars").and_then(Json::as_array).into_iter().flatten().filter_map(Json::as_str) {
                let list = places.entry(hangar.to_lowercase()).or_default();
                if !list.contains(&place) {
                    list.push(place.clone());
                }
            }
        }
    }
    let mut rows: Vec<(i64, CatalogRow)> = Vec::new();
    let mut seen: HashMap<String, usize> = HashMap::new();
    let parts = EntityParts::new(db, &["InstancedInteriorComponentParams", "SObjectContainerComponentParams"], &[])?;
    for entity in parts.with(0) {
        let guid = entity.id.to_string();
        let record = parts.json(entity);
        let Some(container) = component(&record, "Components", "SObjectContainerComponentParams")
            .and_then(|c| c.get("objectContainer")).and_then(Json::as_str).filter(|s| !s.is_empty())
        else {
            continue; // a template (parent) record
        };
        // The archive's spelling of the container (the export takes archive paths).
        let wanted = container.replace('\\', "/").trim_start_matches('/').to_lowercase();
        let Some(path) = archive.get(&wanted).or_else(|| archive.get(&format!("data/{wanted}"))) else { continue };
        let interior = component(&record, "Components", "InstancedInteriorComponentParams");
        let size_id = interior.and_then(|c| c.get("sizeId")).and_then(Json::as_i64).unwrap_or(0);
        // The size's name; its text may be a format with the owner on a first line ("%ls's" + newline + "Small Hangar").
        let size = interior.and_then(|c| c.get("name")).and_then(Json::as_str).and_then(|k| localize(loc, k))
            .map(|s| s.replace("\\n", "\n").rsplit('\n').next().unwrap_or_default().trim().to_owned()).unwrap_or_default();
        let name = record.get("__record_name").and_then(Json::as_str).unwrap_or_default();
        let short = name.rsplit('.').next().unwrap_or(name).to_owned();
        let at = places.get(&guid.to_lowercase()).map(|p| p.join(", ")).unwrap_or_default();
        // The places that offer it, else the record's name (no place in data); grouped by size.
        let row = CatalogRow {
            id: (*path).clone(),
            label: if at.is_empty() { short.clone() } else { at.clone() },
            group: size,
            detail: if at.is_empty() { String::new() } else { short },
        };
        // Records sharing one container are one row (its places merged).
        match seen.get(&row.id) {
            Some(&i) if !at.is_empty() && rows[i].1.detail.is_empty() => rows[i].1 = row,
            Some(_) => {}
            None => {
                seen.insert(row.id.clone(), rows.len());
                rows.push((size_id, row));
            }
        }
    }
    rows.sort_by(|a, b| a.0.cmp(&b.0).then(a.1.label.cmp(&b.1.label)));
    Ok(rows.into_iter().map(|(_, r)| r).collect())
}

fn vehicles(db: &Database, loc: &HashMap<String, String>) -> Result<Vec<CatalogRow>> {
    let mut rows: Vec<CatalogRow> = pick_vehicles(&vehicle_records(db, loc)?)
        .into_iter()
        .map(|v| {
            let kind = if v.gravlev { "Hover bike" } else if v.sub_type == "Vehicle_GroundVehicle" { "Ground vehicle" } else { "" };
            let detail: Vec<String> = [v.size.map(|s| format!("Size {s}")).unwrap_or_default(), v.role.clone(), kind.to_owned()]
                .into_iter()
                .filter(|s| !s.is_empty())
                .collect();
            CatalogRow { id: v.name.clone(), label: v.label.clone().unwrap_or_default(), group: v.manufacturer.clone(), detail: detail.join(" · ") }
        })
        .collect();
    rows.sort_by(|a, b| a.group.cmp(&b.group).then(a.label.cmp(&b.label)));
    Ok(rows)
}

/// Every vehicle record as the rules see it (`VehicleComponentParams` entities).
pub(crate) fn vehicle_records(db: &Database, loc: &HashMap<String, String>) -> Result<Vec<VehicleRecord>> {
    let mut manufacturers: HashMap<String, String> = HashMap::new();
    let mut crews: HashMap<String, usize> = HashMap::new();
    let mut all = Vec::new();
    let parts = EntityParts::new(
        db,
        &["VehicleComponentParams", "SAttachableComponentParams", "SEntityComponentCrewManifestParams"],
        &["SEntityInsuranceProperties", "EAEntityDataParams", "DefaultEntitlementEntityParams"],
    )?;
    for entity in parts.with(0) {
        let guid = entity.id.to_string();
        let record = parts.json(entity);
        let Some(vc) = component(&record, "Components", "VehicleComponentParams") else { continue };
        let attach = component(&record, "Components", "SAttachableComponentParams").and_then(|c| c.get("AttachDef"));
        let name_key = attach.and_then(|a| str_at(a, &["Localization", "Name"])).unwrap_or_default();
        let manufacturer_guid = vc.get("manufacturer").and_then(Json::as_str).unwrap_or_default().to_owned();
        let manufacturer = manufacturers
            .entry(manufacturer_guid.clone())
            .or_insert_with(|| {
                record_by_guid(db, Some(&manufacturer_guid))
                    .map(|m| {
                        str_at(&m, &["Localization", "Name"]).and_then(|k| localize(loc, k))
                            .or_else(|| m.get("Code").and_then(Json::as_str).map(str::to_owned))
                            .unwrap_or_default()
                    })
                    .unwrap_or_default()
            })
            .clone();
        let crew_guid = component(&record, "Components", "SEntityComponentCrewManifestParams")
            .and_then(|c| c.get("crewManifest")).and_then(Json::as_str).unwrap_or_default().to_owned();
        let crew = *crews.entry(crew_guid.clone()).or_insert_with(|| {
            record_by_guid(db, Some(&crew_guid)).and_then(|m| m.get("crew").and_then(Json::as_array).map(Vec::len)).unwrap_or(0)
        });
        let tags = record
            .get("tags").and_then(Json::as_array).into_iter().flatten().filter_map(Json::as_str)
            // The tag database's full names (`/AI/Spawning/GameMode/PU`) in the dotted form the rules name.
            .filter_map(|t| CigGuid::from_str(t).ok().and_then(|id| db.tag_path(&id)).map(|p| p.trim_start_matches('/').replace('/', ".")))
            .collect();
        let insurance = component(&record, "StaticEntityClassData", "SEntityInsuranceProperties");
        let ea = component(&record, "StaticEntityClassData", "EAEntityDataParams");
        let entitlement = component(&record, "StaticEntityClassData", "DefaultEntitlementEntityParams");
        let name = record.get("__record_name").and_then(Json::as_str).unwrap_or_default();
        all.push(VehicleRecord {
            name: name.rsplit('.').next().unwrap_or(name).to_owned(),
            guid: guid.clone(),
            label: localize(loc, name_key),
            label_is_placeholder: placeholder(name_key),
            tags,
            crew,
            insurance_class: insurance.map(|i| str_at(i, &["shipInsuranceParams", "shipEntityClassName"]).unwrap_or_default().to_owned()),
            ea_included: ea.and_then(|e| e.get("inclusionMode")).and_then(Json::as_str) == Some("ReadyToInclude"),
            entitled: entitlement
                .and_then(|e| e.get("entitlementPolicy")).and_then(Json::as_str)
                .is_some_and(|g| CigGuid::from_str(g).ok().is_some_and(|id| db.record_by_id(&id).is_some())),
            definition: vc.get("vehicleDefinition").and_then(Json::as_str).unwrap_or_default().to_owned(),
            modification: vc.get("modification").and_then(Json::as_str).unwrap_or_default().to_owned(),
            manufacturer,
            size: attach.and_then(|a| a.get("Size")).and_then(Json::as_i64),
            sub_type: attach.and_then(|a| a.get("SubType")).and_then(Json::as_str).unwrap_or_default().to_owned(),
            gravlev: vc.get("isGravlevVehicle").and_then(Json::as_bool).unwrap_or(false),
            role: vc.get("vehicleRole").and_then(Json::as_str).and_then(|k| localize(loc, k)).unwrap_or_default(),
        });
    }
    Ok(all)
}

#[cfg(test)]
#[path = "viewer_catalog_tests.rs"]
mod tests;
