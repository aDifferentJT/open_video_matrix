fn main() {
    println!("cargo:rustc-link-search=/usr/local/lib");
    println!("cargo:rustc-link-lib=srt");
    println!("cargo:rerun-if-changed=srt_wrapper.h");

    let srt_bindings = bindgen::Builder::default()
        .header("srt_wrapper.h")
        .parse_callbacks(Box::new(bindgen::CargoCallbacks))
        .clang_arg("-I/usr/local/include")
        .generate()
        .expect("Unable to generate bindings");

    let out_path = std::path::PathBuf::from(std::env::var("OUT_DIR").unwrap());
    srt_bindings
        .write_to_file(out_path.join("srt_bindings.rs"))
        .expect("Couldn't write bindings!");
}
