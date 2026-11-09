//! Additive decomposed child identity contract. Item categories are authored DataCore
//! SAttachableComponentParams.AttachDef.Type strings, never inferred from asset names.
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ChildItemIdentity {
    pub entity_name: String,
    /// None for legacy packages and attachments without an authored item type.
    #[serde(default)]
    pub item_type: Option<String>,
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn preserves_authored_types_and_accepts_legacy_children() {
        let typed: ChildItemIdentity =
            serde_json::from_str(r#"{"entity_name":"synthetic","item_type":"Seat"}"#).unwrap();
        assert_eq!(typed.item_type.as_deref(), Some("Seat"));
        let legacy: ChildItemIdentity =
            serde_json::from_str(r#"{"entity_name":"synthetic"}"#).unwrap();
        assert_eq!(legacy.item_type, None);
        assert_eq!(serde_json::to_value(typed).unwrap()["item_type"], "Seat");
    }
}
