//! Archive path forms, CryXmlB/plain XML text and the XML tree; with game data, reads from Data.p4k.
use super::*;

/// `<Particle a="1"><Name>ref|lib.fx</Name></Particle>` as CryXmlB (node, child, attribute and string tables).
fn cryxmlb() -> Vec<u8> {
    let strings = b"\0Particle\0a\x001\0Name\0ref|lib.fx\0";
    let offset = |s: &[u8]| strings.windows(s.len()).position(|w| w == s).unwrap() as u32;
    let (particle, a, one, name, text) =
        (offset(b"Particle\0"), offset(b"a\0"), offset(b"1\0"), offset(b"Name\0"), offset(b"ref|lib.fx\0"));
    let mut out = b"CryXmlB\0".to_vec();
    let u32s = |out: &mut Vec<u8>, values: &[u32]| values.iter().for_each(|v| out.extend(v.to_le_bytes()));
    // xml_size, node pos/count, attribute pos/count, child pos/count, string pos/size (positions are not read).
    u32s(&mut out, &[0, 0, 2, 0, 1, 0, 1, 0, strings.len() as u32]);
    let node = |out: &mut Vec<u8>, tag: u32, content: u32, attrs: u16, children: u16, parent: i32, first_attr: i32, first_child: i32| {
        out.extend(tag.to_le_bytes());
        out.extend(content.to_le_bytes());
        out.extend(attrs.to_le_bytes());
        out.extend(children.to_le_bytes());
        for v in [parent, first_attr, first_child, 0] {
            out.extend(v.to_le_bytes());
        }
    };
    node(&mut out, particle, 0, 1, 1, -1, 0, 0);
    node(&mut out, name, text, 0, 0, 0, 1, 1);
    out.extend(1i32.to_le_bytes()); // child table: node 1
    u32s(&mut out, &[a, one]); // attribute table
    out.extend(strings);
    out
}

#[test]
fn archive_paths_take_either_slash_and_any_case() {
    assert_eq!(archive_name("Data/Libs/Particles/x.xml"), r"Data\Libs\Particles\x.xml");
    assert_eq!(archive_name(r"\Data\a.xml"), r"Data\a.xml");
    let archive = MemArchive::default().with(r"Data\Libs\Particles\Gadgets\tractor_beam.xml", "<a/>");
    assert_eq!(archive.find("data/libs/particles/gadgets/TRACTOR_BEAM.xml").as_deref(), Some("Data/Libs/Particles/Gadgets/tractor_beam.xml"));
    assert!(archive.read(r"DATA\LIBS\PARTICLES\GADGETS\tractor_beam.xml").is_ok());
    let missing = archive.read("Data/none.xml").unwrap_err().to_string();
    assert!(missing.contains("Data/none.xml"), "{missing}");
}

#[test]
fn cryxmlb_becomes_text_xml_with_element_text_and_plain_xml_passes_through() {
    let text = xml_text(cryxmlb(), "fixture").unwrap();
    let element = Element::parse(&text).unwrap();
    assert_eq!(element.tag, "Particle");
    assert_eq!(element.attr("a"), Some("1"));
    assert_eq!(element.children[0].tag, "Name");
    assert_eq!(element.children[0].text, "ref|lib.fx");

    let plain = "\u{feff}<FXLib>\n  <Effect name=\"x\"><Name k=\"v\"> r </Name></Effect>\n</FXLib>";
    assert_eq!(xml_text(plain.as_bytes().to_vec(), "plain").unwrap(), plain);
    let parsed = Element::parse(plain).unwrap();
    let name = &parsed.children_named("Effect").next().unwrap().children[0];
    assert_eq!((name.text.as_str(), name.attr("k")), ("r", Some("v")));

    let error = xml_text(vec![0xff, 0xfe, 0x00], "Data/bin.dat").unwrap_err().to_string();
    assert!(error.contains("Data/bin.dat"), "{error}");
}

#[test]
#[ignore = "needs the installed Data.p4k"]
fn live_p4k_read_returns_bytes_and_converted_xml() {
    let found = starbreaker_common::discover::find_p4k().expect("Data.p4k");
    let session = Session::open(Some(&found.path), None).unwrap();
    let bytes = session.p4k_read("data/libs/particles/gadgets/TRACTOR_BEAM.xml").unwrap();
    assert!(starbreaker_cryxml::is_cryxmlb(&bytes));
    assert_eq!(session.p4k_read(r"Data\Libs\Particles\Gadgets\tractor_beam.xml").unwrap(), bytes);
    let xml = session.p4k_read_xml("Data/Libs/MaterialEffects/FXLibs/sc_ships_surface_impacts.xml").unwrap();
    assert!(xml.starts_with("<FXLib"), "{}", &xml[..80.min(xml.len())]);
    // Element text survives the conversion: the particle references are <Name> text.
    assert!(xml.contains("|spaceships/surface_effects/Ground_Impacts.Metal.small_impact_gpu</Name>"));
    assert!(session.p4k_read("Data/Libs/Particles/no_such_library.xml").is_err());
    assert!(session.p4k_read("").is_err());
}
