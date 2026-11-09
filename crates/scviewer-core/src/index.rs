//! Owned DataCore/index lifetimes and the persisted entity and geometry browser index.
use anyhow::{Context, Result};
use serde::{Deserialize, Serialize};
use starbreaker_datacore::{Database, QueryResultExt, loadout::EntityIndex, query::value::Value};
use starbreaker_p4k::MappedP4k;
use std::collections::{BTreeSet, HashMap};

self_cell::self_cell!(
    pub(crate) struct DatabaseCell {
        owner: Vec<u8>,
        #[covariant]
        dependent: Database,
    }
);
self_cell::self_cell!(
    pub(crate) struct IndexCell {
        owner: DatabaseCell,
        #[not_covariant]
        dependent: EntityIndex,
    }
);
pub(crate) struct Loaded {
    pub p4k: MappedP4k,
    pub index: IndexCell,
}
impl Loaded {
    pub fn open(path: &std::path::Path) -> Result<Self> {
        let p4k = MappedP4k::open(path)?;
        let dcb = if p4k.entry_case_insensitive(r"Data\Game2.dcb").is_some() {
            r"Data\Game2.dcb"
        } else {
            r"Data\Game.dcb"
        };
        let database = DatabaseCell::try_new(p4k.read_file(dcb)?, |b| Database::from_bytes(b))?;
        let index = IndexCell::new(database, |d| EntityIndex::new(d.borrow_dependent()));
        Ok(Self { p4k, index })
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct EntityHit {
    pub name: String,
    pub guid: String,
    pub geometry_path: Option<String>,
    pub category: String,
    pub display_name: Option<String>,
}
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DirEntry {
    pub path: String,
    pub is_dir: bool,
    pub size: u64,
}
#[derive(Serialize, Deserialize)]
pub(crate) struct BrowserIndex {
    pub version: u32,
    pub build_id: String,
    pub entities: Vec<EntityHit>,
    pub paths: Vec<DirEntry>,
}

pub(crate) fn parse_localization(data: &[u8]) -> HashMap<String, String> {
    String::from_utf8_lossy(data)
        .lines()
        .filter_map(|line| {
            let line = line.trim_start_matches('\u{feff}').trim();
            if line.starts_with([';', '#']) {
                return None;
            }
            line.split_once('=')
                .map(|(k, v)| (k.trim().to_lowercase(), v.trim().to_owned()))
        })
        .collect()
}
pub(crate) fn normalize(path: &str) -> String {
    path.replace('\\', "/").trim_matches('/').to_owned()
}
fn geometry(path: &str) -> bool {
    path.rsplit('.').next().is_some_and(|e| {
        ["cga", "cgf", "skin", "chr", "cdf"].contains(&e.to_ascii_lowercase().as_str())
    })
}
impl BrowserIndex {
    pub fn build(loaded: &Loaded, build_id: &str) -> Result<Self> {
        let loc = parse_localization(
            &loaded
                .p4k
                .read_file(r"Data\Localization\english\global.ini")?,
        );
        let entities = loaded.index.with_dependent(|_, idx| -> Result<Vec<EntityHit>> {
            let db = idx.db;
            let query = db.compile_rooted::<Value>("EntityClassDefinition.Components[SAttachableComponentParams].AttachDef.Localization.Name").optional()?;
            let mut entities = Vec::new();
            for record in db.records_by_type_name("EntityClassDefinition").filter(|r|db.is_main_record(r)) {
                let full_name = db.resolve_string2(record.name_offset);
                let name = full_name.rsplit('.').next().unwrap_or(full_name).to_owned();
                let key = query.as_ref().map(|q| db.query_single::<Value>(q, record).optional()).transpose()?.flatten().flatten();
                let display_name = match key {
                    Some(Value::String(s) | Value::Locale(s)) => loc.get(&s.trim_start_matches('@').to_lowercase()).cloned(),
                    None => None,
                    Some(other) => anyhow::bail!("Localization.Name for {name} has unexpected type: {other:?}"),
                };
                let source = normalize(db.resolve_string(record.file_name_offset)).to_lowercase();
                let category = if source.contains("entities/spaceships") { "Ships" }
                    else if source.contains("entities/groundvehicles") { "Ground Vehicles" }
                    else if source.contains("weapon") { "Weapons" } else { "Other" };
                entities.push(EntityHit { name, guid: record.id.to_string(), geometry_path: idx.query_geometry_paths(record).0,
                    category: category.into(), display_name });
            }
            entities.sort_by(|a,b| (a.name.len(), &a.name).cmp(&(b.name.len(), &b.name)));
            Ok(entities)
        }).context("building entity index")?;
        let mut dirs = BTreeSet::new();
        let mut paths = Vec::new();
        for entry in loaded.p4k.entries().iter().filter(|e| geometry(&e.name)) {
            let path = normalize(&entry.name);
            let mut parent = path.as_str();
            while let Some((dir, _)) = parent.rsplit_once('/') {
                dirs.insert(dir.to_owned());
                parent = dir;
            }
            paths.push(DirEntry {
                path,
                is_dir: false,
                size: entry.uncompressed_size,
            });
        }
        paths.extend(dirs.into_iter().map(|path| DirEntry {
            path,
            is_dir: true,
            size: 0,
        }));
        paths.sort_by(|a, b| a.path.cmp(&b.path));
        Ok(Self {
            version: crate::CACHE_FORMAT_VERSION,
            build_id: build_id.into(),
            entities,
            paths,
        })
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn localization_handles_bom_comments_case_and_equals() {
        let l = parse_localization("\u{feff};comment\n#comment\n ItEm = A=B \n".as_bytes());
        assert_eq!(l.len(), 1);
        assert_eq!(l["item"], "A=B");
    }
}
