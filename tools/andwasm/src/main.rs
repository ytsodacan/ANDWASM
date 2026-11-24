//! ANDWASM: drop an Android APK on this program (or pass its path) and get a web build.
//!
//! Usage:   andwasm <file.apk>        (drag the .apk onto the terminal window, then Enter)
//!
//! What it does:
//!   1. checks the file is an APK that ships an x86 native library (lib/x86/*.so)
//!   2. runs the build pipeline (extract -> lift x86 -> compile to WebAssembly -> pack)
//!   3. copies the finished web files to "<name>-web/" next to the APK
//!
//! The pipeline steps are the existing tools (tools/apk_to_web.py, recomp/, tools/pack_dist.py);
//! this program is the drag-and-drop front end and the checks around them.

use std::fs;
use std::io::{self, Read, Write};
use std::path::{Path, PathBuf};
use std::process::{exit, Command};

const PIPELINE: &str = "tools/apk_to_web.py";

fn main() {
    let raw = match std::env::args().nth(1) {
        Some(p) => p,
        None => prompt_for_path(),
    };
    let apk = clean_path(&raw);
    if let Err(e) = run(&apk) {
        eprintln!("\nANDWASM failed: {e}");
        exit(1);
    }
}

/// Terminal drag-and-drop pastes the path with quotes or backslash-escaped spaces; undo both.
fn clean_path(raw: &str) -> PathBuf {
    let t = raw.trim();
    let t = t.strip_prefix('\'').and_then(|x| x.strip_suffix('\'')).unwrap_or(t);
    let t = t.strip_prefix('"').and_then(|x| x.strip_suffix('"')).unwrap_or(t);
    let mut out = String::new();
    let mut chars = t.chars().peekable();
    while let Some(c) = chars.next() {
        if c == '\\' {
            if let Some(&n) = chars.peek() {
                out.push(n);
                chars.next();
                continue;
            }
        }
        out.push(c);
    }
    PathBuf::from(out)
}

fn prompt_for_path() -> String {
    eprint!("Drag an .apk onto this window and press Enter: ");
    let _ = io::stderr().flush();
    let mut s = String::new();
    let _ = io::stdin().read_to_string(&mut s);
    s.lines().next().unwrap_or("").to_string()
}

fn run(apk: &Path) -> Result<(), String> {
    if !apk.exists() {
        return Err(format!("no such file: {}", apk.display()));
    }
    if apk.extension().and_then(|e| e.to_str()).map(|e| e.eq_ignore_ascii_case("apk")) != Some(true) {
        return Err("that is not an .apk file".into());
    }
    let has_x86 = zip_has_x86_native(apk)?;
    if !has_x86 {
        return Err("this APK has no x86 native library (lib/x86/*.so), so there is no native code to build".into());
    }

    let repo = repo_root()?;
    let stem = apk.file_stem().and_then(|s| s.to_str()).unwrap_or("app");
    let safe: String = stem.chars().map(|c| if c.is_ascii_alphanumeric() || c == '-' || c == '_' || c == '.' { c } else { '_' }).collect();
    let work = repo.join("build").join("andwasm").join(&safe);
    println!("ANDWASM: building {} (work folder: {})", apk.display(), work.display());

    let status = Command::new("python3")
        .arg(repo.join(PIPELINE))
        .arg(apk)
        .arg("--work")
        .arg(&work)
        .current_dir(&repo)
        .status()
        .map_err(|e| format!("could not start the build pipeline: {e}"))?;
    if !status.success() {
        return Err(format!("the build pipeline stopped (exit {})", status.code().unwrap_or(-1)));
    }

    let dist = work.join("dist");
    if !dist.join("manifest.json").exists() {
        return Err("the build finished without a web output (manifest.json missing)".into());
    }
    let out = apk.parent().unwrap_or(Path::new(".")).join(format!("{safe}-web"));
    if out.exists() {
        fs::remove_dir_all(&out).map_err(|e| format!("could not clear {}: {e}", out.display()))?;
    }
    copy_dir(&dist, &out).map_err(|e| format!("could not copy the web files: {e}"))?;
    println!("\nANDWASM done. Web build: {}", out.display());
    println!("Files: {} (manifest.json plus the wasm and asset chunks)", count_files(&out));
    Ok(())
}

/// The pipeline lives in the repository this tool was built from (the folder with tools/apk_to_web.py).
fn repo_root() -> Result<PathBuf, String> {
    let mut dir = std::env::current_dir().map_err(|e| e.to_string())?;
    loop {
        if dir.join(PIPELINE).exists() {
            return Ok(dir);
        }
        if !dir.pop() {
            break;
        }
    }
    // fall back to the location this binary was built from
    let built = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..");
    if built.join(PIPELINE).exists() {
        return Ok(built.canonicalize().map_err(|e| e.to_string())?);
    }
    Err(format!("could not find {PIPELINE}; run ANDWASM from inside the repository"))
}

/// True when the APK (a zip) contains an x86 native library.
fn zip_has_x86_native(apk: &Path) -> Result<bool, String> {
    let out = Command::new("unzip")
        .arg("-Z1")
        .arg(apk)
        .output()
        .map_err(|e| format!("could not read the APK (needs the unzip tool): {e}"))?;
    if !out.status.success() {
        return Err("the file is not a valid APK (zip) archive".into());
    }
    Ok(String::from_utf8_lossy(&out.stdout).lines().any(|l| l.starts_with("lib/x86/") && l.ends_with(".so")))
}

fn copy_dir(from: &Path, to: &Path) -> io::Result<()> {
    fs::create_dir_all(to)?;
    for e in fs::read_dir(from)? {
        let e = e?;
        let dst = to.join(e.file_name());
        if e.file_type()?.is_dir() {
            copy_dir(&e.path(), &dst)?;
        } else {
            fs::copy(e.path(), dst)?;
        }
    }
    Ok(())
}

fn count_files(dir: &Path) -> usize {
    fs::read_dir(dir).map(|rd| rd.filter_map(|e| e.ok()).filter(|e| e.path().is_file()).count()).unwrap_or(0)
}
