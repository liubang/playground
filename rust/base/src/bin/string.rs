pub fn parse_string(lit: &str) -> Result<(), String> {
    if lit.len() < 2 || !lit.starts_with('"') || !lit.ends_with('"') {
        return Err("invalid string literal".to_string());
    }
    println!("{}", &lit[1..lit.len() - 1]);
    println!("{}", lit);
    Ok(())
}

fn main() {
    let s = "\"this is test string\"";
    let _ = parse_string(s);
}
