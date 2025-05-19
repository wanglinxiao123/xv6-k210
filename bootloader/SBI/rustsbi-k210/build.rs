// 自定义构建脚本

use std::env;          //用于访问环境变量和命令行参数
use std::fs;           //用于文件系统操作
use std::io::Write;    //用于写入操作的 trait
use std::path::PathBuf;//用于构造和操作文件路径的类型

fn main() {
    // 获取构建目录的输出路径
    let out_dir = PathBuf::from(env::var("OUT_DIR").unwrap());

    // 在 output 文件夹下创建 link-k210.ld 文件
    // 将 link-k210.ld 文件写入
    fs::File::create(out_dir.join("link-k210.ld"))
        .unwrap()
        .write_all(include_bytes!("link-k210.ld"))
        .unwrap();

    println!("cargo:rustc-link-search={}", out_dir.display());
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=link-k210.ld");
}

