//! The value syntax of CryEngine particle `Params` attributes, as the Star Citizen libraries write it: a base value,
//! then comma-separated named modifiers, nested in parentheses, e.g. `Size="0.8,ParticleAge=(v=0.27;t=1,v=1)"` or
//! `LightSource="Radius=10,Intensity=(50,EmitterStrength=(v=0.408;t=1,v=1))"`. A parenthesised group holds one or more
//! `;`-separated sequences; several sequences are the keys of a curve.
//!
//! The editor omits every value equal to its default (the data never writes `v=0` in a curve key, nor `Speed="1"`),
//! so readers supply defaults: a curve key's missing `t` and `v` are 0 (a missing vector component of `v` too),
//! a vector's missing component is the caller's default. [`merge`] layers a child's partial value over its parent's,
//! field by field, which is what `Inheritance="Parent"` means.
use std::fmt;

/// One parsed value: a bare scalar, or a parenthesised group of `;`-separated sequences of items.
#[derive(Clone, Debug, PartialEq)]
pub enum PValue {
    Scalar(String),
    Group(Vec<Vec<PItem>>),
}

/// An item of a sequence: `key=value` or a positional value.
#[derive(Clone, Debug, PartialEq)]
pub struct PItem {
    pub key: Option<String>,
    pub value: PValue,
}

/// Parse an attribute value. The top level is a group (one sequence unless the text has a top-level `;`).
/// Never fails: unbalanced parentheses close at the end of the text, a stray `)` is skipped.
pub fn parse(text: &str) -> PValue {
    let chars: Vec<char> = text.chars().collect();
    let mut pos = 0;
    let mut seqs = parse_group(&chars, &mut pos);
    while pos < chars.len() {
        // A stray ')' at the top level: skip it and keep reading into the last sequence.
        pos += 1;
        let more = parse_group(&chars, &mut pos);
        let mut more = more.into_iter();
        if let (Some(last), Some(first)) = (seqs.last_mut(), more.next()) {
            last.extend(first);
        }
        seqs.extend(more);
    }
    PValue::Group(seqs)
}

fn parse_group(chars: &[char], pos: &mut usize) -> Vec<Vec<PItem>> {
    let mut seqs = vec![Vec::new()];
    while *pos < chars.len() {
        match chars[*pos] {
            ')' => break,
            ';' => {
                seqs.push(Vec::new());
                *pos += 1;
            }
            ',' => *pos += 1,
            c if c.is_whitespace() => *pos += 1,
            _ => {
                let item = parse_item(chars, pos);
                seqs.last_mut().expect("one sequence").push(item);
            }
        }
    }
    seqs
}

fn parse_paren(chars: &[char], pos: &mut usize) -> PValue {
    *pos += 1; // '('
    let group = parse_group(chars, pos);
    if *pos < chars.len() {
        *pos += 1; // ')'
    }
    PValue::Group(group)
}

fn parse_item(chars: &[char], pos: &mut usize) -> PItem {
    if chars[*pos] == '(' {
        return PItem { key: None, value: parse_paren(chars, pos) };
    }
    let text = read_text(chars, pos, true);
    if *pos < chars.len() && chars[*pos] == '=' {
        *pos += 1;
        let value = if *pos < chars.len() && chars[*pos] == '(' {
            parse_paren(chars, pos)
        } else {
            PValue::Scalar(read_text(chars, pos, false))
        };
        return PItem { key: Some(text), value };
    }
    PItem { key: None, value: PValue::Scalar(text) }
}

/// Text up to the next `,;()` (and `=` when reading a key or positional value), trimmed.
fn read_text(chars: &[char], pos: &mut usize, stop_at_eq: bool) -> String {
    let start = *pos;
    while *pos < chars.len() && !matches!(chars[*pos], ',' | ';' | '(' | ')') && !(stop_at_eq && chars[*pos] == '=') {
        *pos += 1;
    }
    if *pos == start && *pos < chars.len() && chars[*pos] == '=' {
        // `=value` with no key: keep the text rather than looping on the '='.
        *pos += 1;
        return read_text(chars, pos, false);
    }
    chars[start..*pos].iter().collect::<String>().trim().to_owned()
}

impl PValue {
    /// The first sequence's items (a scalar has none).
    pub fn items(&self) -> &[PItem] {
        match self {
            PValue::Group(seqs) => seqs.first().map(Vec::as_slice).unwrap_or(&[]),
            PValue::Scalar(_) => &[],
        }
    }
    /// The base value: a scalar itself, or a group's first positional item.
    pub fn base(&self) -> Option<&PValue> {
        match self {
            PValue::Scalar(_) => Some(self),
            PValue::Group(_) => self.items().iter().find(|i| i.key.is_none()).map(|i| &i.value),
        }
    }
    /// A named item of the first sequence.
    pub fn get(&self, key: &str) -> Option<&PValue> {
        self.items().iter().find(|i| i.key.as_deref() == Some(key)).map(|i| &i.value)
    }
    /// The scalar text of the base value.
    pub fn text(&self) -> Option<&str> {
        match self.base()? {
            PValue::Scalar(s) => Some(s.as_str()),
            group @ PValue::Group(_) => group.text(),
        }
    }
    /// The base value as a number.
    pub fn num(&self) -> Option<f64> {
        self.text()?.parse().ok()
    }
    /// The base value as a boolean (`true`/`false`, or a number: nonzero is true).
    pub fn bool(&self) -> Option<bool> {
        match self.text()? {
            t if t.eq_ignore_ascii_case("true") => Some(true),
            t if t.eq_ignore_ascii_case("false") => Some(false),
            t => t.parse::<f64>().ok().map(|n| n != 0.0),
        }
    }
    /// A vector from `x=`, `y=`, `z=` items (missing ones from `default`). A group whose base is a vector group,
    /// such as `Color="(x=1,y=0.5,z=0),Random=0.3"`, reads that base; a bare number fills all three.
    pub fn vec3(&self, default: [f64; 3]) -> [f64; 3] {
        if let Some(n) = self.num() {
            return [n; 3];
        }
        let source = match self.base() {
            Some(inner @ PValue::Group(_)) if !self.has_axes() => inner,
            _ => self,
        };
        let axis = |name: &str, d: f64| source.get(name).and_then(PValue::num).unwrap_or(d);
        [axis("x", default[0]), axis("y", default[1]), axis("z", default[2])]
    }
    fn has_axes(&self) -> bool {
        ["x", "y", "z"].iter().any(|a| self.get(a).is_some())
    }
    /// Whether this group is a curve: several sequences, or keys named `t`/`v`/`flags`.
    pub fn is_curve(&self) -> bool {
        match self {
            PValue::Group(seqs) => {
                seqs.len() > 1
                    || seqs.iter().flatten().any(|i| matches!(i.key.as_deref(), Some("t" | "v" | "flags")))
            }
            PValue::Scalar(_) => false,
        }
    }
    /// A scalar curve: `[t, v]` per key; missing `t` and `v` are 0.
    pub fn curve(&self) -> Vec<[f64; 2]> {
        self.curve_keys().map(|(t, v)| [t, v.and_then(PValue::num).unwrap_or(0.0)]).collect()
    }
    /// A colour curve: `[t, r, g, b]` per key; a missing `v` or component is 0.
    pub fn curve_rgb(&self) -> Vec<[f64; 4]> {
        self.curve_keys()
            .map(|(t, v)| {
                let [r, g, b] = v.map_or([0.0; 3], |v| v.vec3([0.0; 3]));
                [t, r, g, b]
            })
            .collect()
    }
    fn curve_keys(&self) -> impl Iterator<Item = (f64, Option<&PValue>)> {
        let seqs: &[Vec<PItem>] = match self {
            PValue::Group(seqs) => seqs,
            PValue::Scalar(_) => &[],
        };
        seqs.iter().map(|key| {
            let get = |name: &str| key.iter().find(|i| i.key.as_deref() == Some(name)).map(|i| &i.value);
            (get("t").and_then(PValue::num).unwrap_or(0.0), get("v"))
        })
    }
}

/// Layer `child` over `parent` field by field: a named item replaces the parent's item of that name (merged again
/// when both are single-sequence, non-curve groups), the n-th positional item replaces the parent's n-th, and a curve
/// replaces the parent's curve whole.
pub fn merge(parent: &PValue, child: &PValue) -> PValue {
    match (parent, child) {
        (PValue::Group(p), PValue::Group(c))
            if p.len() == 1 && c.len() == 1 && !parent.is_curve() && !child.is_curve() =>
        {
            let mut items = p[0].clone();
            let mut positional = 0;
            for item in &c[0] {
                let slot = match &item.key {
                    Some(key) => items.iter().position(|i| i.key.as_ref() == Some(key)),
                    None => {
                        let slot = items.iter().enumerate().filter(|(_, i)| i.key.is_none()).nth(positional).map(|(n, _)| n);
                        positional += 1;
                        slot
                    }
                };
                match slot {
                    Some(n) => items[n].value = merge(&items[n].value, &item.value),
                    None if item.key.is_none() => items.insert((positional - 1).min(items.len()), item.clone()),
                    None => items.push(item.clone()),
                }
            }
            PValue::Group(vec![items])
        }
        // A scalar over a group replaces the group's base; a group over a scalar base keeps the base.
        (PValue::Group(_), PValue::Scalar(_)) if !parent.is_curve() => {
            merge(parent, &PValue::Group(vec![vec![PItem { key: None, value: child.clone() }]]))
        }
        (PValue::Scalar(_), PValue::Group(_)) if !child.is_curve() => {
            merge(&PValue::Group(vec![vec![PItem { key: None, value: parent.clone() }]]), child)
        }
        _ => child.clone(),
    }
}

impl fmt::Display for PValue {
    /// The attribute syntax again (top-level groups without their parentheses are written by [`to_attr`]).
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            PValue::Scalar(s) => f.write_str(s),
            PValue::Group(_) => write!(f, "({})", to_attr(self)),
        }
    }
}

/// A top-level value as attribute text (no enclosing parentheses).
pub fn to_attr(value: &PValue) -> String {
    match value {
        PValue::Scalar(s) => s.clone(),
        PValue::Group(seqs) => seqs
            .iter()
            .map(|seq| {
                seq.iter()
                    .map(|i| match &i.key {
                        Some(k) => format!("{k}={}", i.value),
                        None => i.value.to_string(),
                    })
                    .collect::<Vec<_>>()
                    .join(",")
            })
            .collect::<Vec<_>>()
            .join(";"),
    }
}
