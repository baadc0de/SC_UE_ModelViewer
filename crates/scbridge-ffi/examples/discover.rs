//! Probe core archive discovery for the explicit C smoke script; 77 means skip.
fn main() {
    match starbreaker_common::discover::find_p4k() {
        Ok(found) => match found.path.to_str() {
            Some(path) => println!("{path}"),
            None => {
                eprintln!("C ABI requires a UTF-8 p4k path");
                std::process::exit(1);
            }
        },
        Err(error) => {
            eprintln!("SKIP C smoke: no Data.p4k discovered ({error})");
            std::process::exit(77);
        }
    }
}
