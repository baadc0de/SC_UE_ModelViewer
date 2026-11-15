//! One DataCore record as JSON (`Session::datacore_record`), so hosts read game values at run time instead of
//! copying them: lookup and shape are StarBreaker's `query::record_json` (the `dcb query` value shape, `__type` on
//! objects, references as record GUIDs that resolve by another lookup).
use crate::Session;
use anyhow::{Result, anyhow, bail};
use starbreaker_datacore::query::record_json::{find_records, record_json};

impl Session {
    /// The record `name_or_guid` names — `Type.Name`, a bare record name (exact, case-insensitive) or a GUID — as
    /// JSON with `__record_name` / `__record_id`. An error when none or several records match.
    pub fn datacore_record(&self, name_or_guid: &str) -> Result<serde_json::Value> {
        let loaded = self.loaded()?;
        loaded.index.with_dependent(|_, idx| -> Result<_> {
            let db = idx.db;
            let matches = find_records(db, name_or_guid);
            match matches.as_slice() {
                [] => bail!("no DataCore record named {name_or_guid:?}"),
                [record] => Ok(record_json(db, record)?),
                several => {
                    let names: Vec<_> = several.iter().map(|r| db.resolve_string2(r.name_offset)).collect();
                    Err(anyhow!(
                        "{name_or_guid:?} names {} DataCore records; use Type.Name or a GUID: {}",
                        names.len(),
                        names.join(", ")
                    ))
                }
            }
        })
    }
}
