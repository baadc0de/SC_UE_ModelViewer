//! Raw Data.p4k reads by archive path (`Session::p4k_read`, `Session::p4k_read_xml`) and the small XML tree the
//! effect readers walk. A path names an archive entry (e.g. `Data/Libs/Particles/Gadgets/tractor_beam.xml`), either
//! slash style, any case. Binary CryXML (`CryXmlB`) becomes text XML through StarBreaker's converter, which keeps
//! element text; other files pass through when they are UTF-8.
use crate::Session;
use anyhow::{Context, Result, anyhow, bail};
use starbreaker_p4k::MappedP4k;

/// What the effect readers need from an archive: entry lookup and reads. `MappedP4k` in sessions, a map in tests.
pub(crate) trait Archive {
    /// The entry's own name (forward slashes, archive case) for a path in either slash style, any case.
    fn find(&self, path: &str) -> Option<String>;
    /// The entry's bytes; an error naming the path when it is not in the archive.
    fn read(&self, path: &str) -> Result<Vec<u8>>;
}

/// An archive path in the archive's own form: backslashes, no leading separator.
pub(crate) fn archive_name(path: &str) -> String {
    path.trim().replace('/', "\\").trim_start_matches('\\').to_owned()
}

impl Archive for MappedP4k {
    fn find(&self, path: &str) -> Option<String> {
        self.entry_case_insensitive(&archive_name(path)).map(|e| e.name.replace('\\', "/"))
    }
    fn read(&self, path: &str) -> Result<Vec<u8>> {
        let entry = self
            .entry_case_insensitive(&archive_name(path))
            .with_context(|| format!("{path} is not in Data.p4k"))?;
        MappedP4k::read(self, entry).with_context(|| format!("reading {path}"))
    }
}

/// Text XML of a file's bytes: CryXmlB converted, anything else passed through when it is UTF-8.
pub(crate) fn xml_text(bytes: Vec<u8>, path: &str) -> Result<String> {
    if starbreaker_cryxml::is_cryxmlb(&bytes) {
        let doc = starbreaker_cryxml::from_bytes(&bytes).map_err(|e| anyhow!("{path}: bad CryXmlB: {e}"))?;
        return Ok(doc.to_string());
    }
    String::from_utf8(bytes).map_err(|_| anyhow!("{path} is neither CryXmlB nor UTF-8 text"))
}

/// One XML element: tag, attributes in document order, own text (trimmed) and child elements.
#[derive(Clone, Debug, Default, PartialEq)]
pub(crate) struct Element {
    pub tag: String,
    pub attrs: Vec<(String, String)>,
    pub text: String,
    pub children: Vec<Element>,
}
impl Element {
    /// Parse text XML (a leading byte-order mark is ignored).
    pub fn parse(text: &str) -> Result<Self> {
        let doc = roxmltree::Document::parse(text.trim_start_matches('\u{feff}')).context("parsing XML")?;
        Ok(Self::from_node(doc.root_element()))
    }
    fn from_node(node: roxmltree::Node) -> Self {
        Self {
            tag: node.tag_name().name().to_owned(),
            attrs: node.attributes().map(|a| (a.name().to_owned(), a.value().to_owned())).collect(),
            text: node
                .children()
                .filter(|c| c.is_text())
                .filter_map(|c| c.text())
                .collect::<String>()
                .trim()
                .to_owned(),
            children: node.children().filter(|c| c.is_element()).map(Self::from_node).collect(),
        }
    }
    /// An attribute's value by exact name.
    pub fn attr(&self, name: &str) -> Option<&str> {
        self.attrs.iter().find(|(k, _)| k == name).map(|(_, v)| v.as_str())
    }
    /// Child elements with this tag.
    pub fn children_named<'a>(&'a self, tag: &'a str) -> impl Iterator<Item = &'a Element> + 'a {
        self.children.iter().filter(move |c| c.tag == tag)
    }
}

/// Read an archive file as an XML tree.
pub(crate) fn read_element(archive: &dyn Archive, path: &str) -> Result<Element> {
    let text = xml_text(archive.read(path)?, path)?;
    Element::parse(&text).with_context(|| format!("{path} is not XML"))
}

impl Session {
    /// The bytes of the archive file at `path` (e.g. `Data/Libs/Particles/Gadgets/tractor_beam.xml`; either slash
    /// style, case-insensitive). An error when the archive has no such file.
    pub fn p4k_read(&self, path: &str) -> Result<Vec<u8>> {
        if path.trim().is_empty() {
            bail!("empty archive path");
        }
        Archive::read(&self.loaded()?.p4k, path)
    }
    /// The archive file at `path` as text XML: binary CryXML is converted (element text kept); other files pass
    /// through unchanged when they are UTF-8, and are an error otherwise.
    pub fn p4k_read_xml(&self, path: &str) -> Result<String> {
        xml_text(self.p4k_read(path)?, path)
    }
}

/// An in-memory archive for tests: entry names as given, looked up like Data.p4k (either slash, any case).
#[cfg(test)]
#[derive(Default)]
pub(crate) struct MemArchive(pub Vec<(String, Vec<u8>)>);
#[cfg(test)]
impl MemArchive {
    pub fn with(mut self, name: &str, bytes: impl Into<Vec<u8>>) -> Self {
        self.0.push((name.to_owned(), bytes.into()));
        self
    }
    fn entry(&self, path: &str) -> Option<&(String, Vec<u8>)> {
        let wanted = archive_name(path);
        self.0.iter().find(|(name, _)| archive_name(name).eq_ignore_ascii_case(&wanted))
    }
}
#[cfg(test)]
impl Archive for MemArchive {
    fn find(&self, path: &str) -> Option<String> {
        self.entry(path).map(|(name, _)| name.replace('\\', "/"))
    }
    fn read(&self, path: &str) -> Result<Vec<u8>> {
        self.entry(path).map(|(_, b)| b.clone()).with_context(|| format!("{path} is not in the archive"))
    }
}

#[cfg(test)]
#[path = "archive_tests.rs"]
mod tests;
