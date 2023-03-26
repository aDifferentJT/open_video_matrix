fn main() {
    println!("cargo:rustc-link-search=/Applications/VLC.app/Contents/MacOS/lib");
    println!("cargo:rustc-link-lib=vlc");
    println!("cargo:rerun-if-changed=vlc/vlc.h");

    let bindings = bindgen::Builder::default()
        .header("wrapper.h")
        .parse_callbacks(Box::new(bindgen::CargoCallbacks))
        .clang_arg("-I/Applications/VLC.app/Contents/MacOS/include")
        .generate()
        .expect("Unable to generate bindings");

    let out_path = std::path::PathBuf::from(std::env::var("OUT_DIR").unwrap());
    bindings
        .write_to_file(out_path.join("bindings.rs"))
        .expect("Couldn't write bindings!");
}
