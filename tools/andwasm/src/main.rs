//! ANDWASM: drop an Android app (.apk, .xapk or .apkm) on this program (or pass its path) and get a web build.
//!
//! Usage:   andwasm <file.apk|.xapk|.apkm>   (drag the file onto the terminal window, then Enter)
//!
//! What it does:
//!   - x86 native code (lib/x86/*.so, plain .apk): tools/apk_to_web.py (extract -> lift x86 -> WebAssembly -> pack)
//!   - otherwise ARM (lib/armeabi-v7a, also inside .xapk/.apkm bundles): recomp/arm/port_apk.py
//!     (lift ARM -> WebAssembly, reusing an earlier build when the library is the same)
//!   - copies the finished web files to "<name>-web/" next to the input file
//!
//! This program is the drag-and-drop front end and the checks around the pipeline scripts.

use std::fs;
use std::io::{self, Read, Write};
use std::path::{Path, PathBuf};
use std::process::{exit, Command};

const PIPELINE: &str = "tools/apk_to_web.py";
const ARM_PIPELINE: &str = "recomp/arm/port_apk.py";

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
    eprint!("Drag an .apk / .xapk / .apkm onto this window and press Enter: ");
    let _ = io::stderr().flush();
    let mut s = String::new();
    let _ = io::stdin().read_to_string(&mut s);
    s.lines().next().unwrap_or("").to_string()
}

fn run(apk: &Path) -> Result<(), String> {
    if !apk.exists() {
        return Err(format!("no such file: {}", apk.display()));
    }
    let ext = apk.extension().and_then(|e| e.to_str()).unwrap_or("").to_ascii_lowercase();
    if !["apk", "xapk", "apkm"].contains(&ext.as_str()) {
        return Err("that is not an .apk, .xapk or .apkm file".into());
    }
    let repo = repo_root()?;
    let stem = apk.file_stem().and_then(|s| s.to_str()).unwrap_or("app");
    let safe: String = stem.chars().map(|c| if c.is_ascii_alphanumeric() || c == '-' || c == '_' || c == '.' { c } else { '_' }).collect();
    let out = apk.parent().unwrap_or(Path::new(".")).join(format!("{safe}-web"));

    if ext == "apk" && zip_has_x86_native(apk)? {
        return run_x86(apk, &repo, &safe, &out);
    }
    // ARM (armeabi-v7a), including split bundles
    println!("ANDWASM: building {} with the ARM pipeline", apk.display());
    let status = Command::new("python3")
        .arg(repo.join(ARM_PIPELINE))
        .arg(apk)
        .arg("--name")
        .arg(&safe)
        .current_dir(&repo)
        .status()
        .map_err(|e| format!("could not start the ARM pipeline: {e}"))?;
    if !status.success() {
        return Err(format!("the ARM pipeline stopped (exit {})", status.code().unwrap_or(-1)));
    }
    let web = repo.join("build").join("games").join(&safe).join("web");
    if !web.join("index.html").exists() {
        return Err("the build finished without a web output (index.html missing)".into());
    }
    if out.exists() {
        fs::remove_dir_all(&out).map_err(|e| format!("could not clear {}: {e}", out.display()))?;
    }
    copy_dir(&web, &out).map_err(|e| format!("could not copy the web files: {e}"))?;
    println!("\nANDWASM done. Web build: {}", out.display());
    println!("Play it:  python3 {} {} 8000   then open http://localhost:8000/", repo.join("recomp/arm/web/serve.py").display(), out.display());
    Ok(())
}

fn run_x86(apk: &Path, repo: &Path, safe: &str, out: &Path) -> Result<(), String> {
    let work = repo.join("build").join("andwasm").join(safe);
    println!("ANDWASM: building {} (work folder: {})", apk.display(), work.display());

    let status = Command::new("python3")
        .arg(repo.join(PIPELINE))
        .arg(apk)
        .arg("--work")
        .arg(&work)
        .current_dir(repo)
        .status()
        .map_err(|e| format!("could not start the build pipeline: {e}"))?;
    if !status.success() {
        return Err(format!("the build pipeline stopped (exit {})", status.code().unwrap_or(-1)));
    }

    let dist = work.join("dist");
    if !dist.join("manifest.json").exists() {
        return Err("the build finished without a web output (manifest.json missing)".into());
    }
    if out.exists() {
        fs::remove_dir_all(out).map_err(|e| format!("could not clear {}: {e}", out.display()))?;
    }
    copy_dir(&dist, out).map_err(|e| format!("could not copy the web files: {e}"))?;
    println!("\nANDWASM done. Web build: {}", out.display());
    println!("Files: {} (manifest.json plus the wasm and asset chunks)", count_files(out));
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
