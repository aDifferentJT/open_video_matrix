fn main() {
    println!("cargo:rustc-link-search=/usr/local/lib");
    println!("cargo:rustc-link-lib=avcodec");
    println!("cargo:rustc-link-lib=avformat");
    println!("cargo:rustc-link-lib=swresample");
    println!("cargo:rustc-link-lib=swscale");
    println!("cargo:rerun-if-changed=ffmpeg_wrapper.h");

    let ffmpeg_bindings = bindgen::Builder::default()
        .header("ffmpeg_wrapper.h")
        .parse_callbacks(Box::new(bindgen::CargoCallbacks))
        .clang_arg("-I/usr/local/include")
        .generate()
        .expect("Unable to generate bindings");

    let out_path = std::path::PathBuf::from(std::env::var("OUT_DIR").unwrap());
    ffmpeg_bindings
        .write_to_file(out_path.join("ffmpeg_bindings.rs"))
        .expect("Couldn't write bindings!");
}
