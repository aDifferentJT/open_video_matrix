fn main() -> miette::Result<()> {
    let boost_path = "/usr/local/Cellar/boost/1.81.0_1/include";

    let paths = [
        std::path::PathBuf::from(boost_path),
        std::path::PathBuf::from("."),
    ]; // include path
    let mut b = autocxx_build::Builder::new("src/lib.rs", &paths)
        .extra_clang_args(&["--std=c++20"])
        .build()?;
    // This assumes all your C++ bindings are in main.rs
    b.flag_if_supported("--std=c++20")
        .include(boost_path)
        .compile("autocxx-demo"); // arbitrary library name, pick anything

    println!("cargo:rerun-if-changed=src/lib.rs");
    println!("cargo:rerun-if-changed=../triple_buffer.hpp");
    Ok(())
}
