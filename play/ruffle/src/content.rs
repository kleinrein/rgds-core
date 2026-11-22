//! Where a game's files come from: a lone .swf on the card, or a zip pack.
//!
//! A Flashpoint game zip ("GameZIP") keeps every file the game ever loaded under the web address it came from,
//! `content/<host>/<path>`; a Flashpoint curation keeps them under `content/` too, beside a `meta.yaml` that names
//! the launch address. Any other zip is a folder of files served from `http://localhost/`. The player asks for
//! files by their web address, and this answers from the pack, so a game finds its levels and passes its site
//! lock offline.

use std::collections::HashMap;
use std::fs::File;
use std::io::{BufReader, Read};
use std::path::{Path, PathBuf};
use std::sync::Mutex;

use url::Url;

pub const LOCAL_HOST: &str = "localhost";

pub enum Source {
    /// A .swf on the card: its folder answers relative loads, and http addresses by host folder or file name.
    Folder(PathBuf),
    /// A zip pack, indexed by "host/path" in lower case.
    Zip {
        archive: Mutex<zip::ZipArchive<BufReader<File>>>,
        index: HashMap<String, usize>,
        by_name: HashMap<String, Vec<usize>>,
    },
}

pub struct Content {
    pub source: Source,
    /// The address the game starts from (what Flash reports as its own URL).
    pub launch: String,
    /// The bytes of the movie at `launch`, read once to size the screen.
    pub movie: Vec<u8>,
    /// FlashVars a launch page passed to its movie, as Flash's `parameters`.
    pub parameters: Vec<(String, String)>,
    /// The sidecar's lines (`<stem>.flashpoint`): genre and tags from Flashpoint, a `stick=` the owner wrote.
    pub details: HashMap<String, String>,
}

fn key_of(host: &str, path: &str) -> String {
    let path = percent_encoding::percent_decode_str(path).decode_utf8_lossy();
    format!("{}/{}", host.to_ascii_lowercase(), path.trim_start_matches('/').to_lowercase())
}

fn file_name(key: &str) -> &str {
    key.rsplit('/').next().unwrap_or(key)
}

/// "host/path" for a zip entry, or None for folders and the pack's own notes.
fn entry_key(name: &str) -> Option<String> {
    let name = name.replace('\\', "/");
    if name.ends_with('/') {
        return None;
    }
    for prefix in ["content/", "htdocs/", "Legacy/htdocs/"] {
        if let Some(rest) = name.strip_prefix(prefix) {
            let (host, path) = rest.split_once('/')?;
            return Some(key_of(host, path));
        }
    }
    // A curation zipped with its folder: "<name>/content/<host>/...".
    if let Some((_, rest)) = name.split_once("/content/") {
        let (host, path) = rest.split_once('/')?;
        return Some(key_of(host, path));
    }
    Some(key_of(LOCAL_HOST, &name))
}

fn lookup_key(url: &Url) -> Option<String> {
    Some(key_of(url.host_str()?, url.path()))
}

/// The launch address and FlashVars written beside a pack by the library's Flashpoint import (`<stem>.flashpoint`,
/// lines of `key=value`).
fn sidecar(game: &Path) -> HashMap<String, String> {
    let mut out = HashMap::new();
    let side = game.with_extension("flashpoint");
    if let Ok(text) = std::fs::read_to_string(side) {
        for line in text.lines() {
            if let Some((k, v)) = line.split_once('=') {
                out.insert(k.trim().to_ascii_lowercase(), v.trim().to_string());
            }
        }
    }
    out
}

fn yaml_launch(text: &str) -> Option<String> {
    for line in text.lines() {
        let line = line.trim();
        for key in ["Launch Command:", "launchCommand:", "Application Path Launch Command:"] {
            if let Some(v) = line.strip_prefix(key) {
                let v = v.trim().trim_matches('"').trim_matches('\'').trim();
                if !v.is_empty() {
                    return Some(v.to_string());
                }
            }
        }
    }
    None
}

/// The movie a launch page embeds, and the FlashVars it passes (the first `.swf` named in an attribute or a
/// script string).
pub fn movie_in_page(page: &str) -> Option<(String, Vec<(String, String)>)> {
    let lower = page.to_ascii_lowercase();
    let mut found = None;
    let mut at = 0;
    while let Some(i) = lower[at..].find(".swf") {
        let end = at + i + 4;
        let start = lower[..end].rfind(['"', '\'', '=', ' ', '(']).map(|s| s + 1).unwrap_or(0);
        let name = page[start..end].trim();
        if !name.is_empty() && !name.contains(['<', '>']) {
            found = Some(name.to_string());
            break;
        }
        at = end;
    }
    let movie = found?;
    let mut vars = Vec::new();
    if let Some(i) = lower.find("flashvars") {
        let rest = &page[i + 9..];
        let rest = rest.trim_start_matches(['"', '\'', ' ', ':', '=']);
        let rest = rest.strip_prefix("value=").unwrap_or(rest).trim_start_matches(['"', '\'']);
        let end = rest.find(['"', '\'']).unwrap_or(rest.len());
        for pair in rest[..end].split('&') {
            if let Some((k, v)) = pair.split_once('=') {
                let v = percent_encoding::percent_decode_str(v).decode_utf8_lossy().to_string();
                vars.push((k.to_string(), v));
            }
        }
    }
    Some((movie, vars))
}

impl Content {
    pub fn open(path: &Path) -> Result<Content, String> {
        let ext = path.extension().and_then(|e| e.to_str()).unwrap_or("").to_ascii_lowercase();
        if ext == "zip" {
            return Self::open_zip(path);
        }
        let movie = std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
        let side = sidecar(path);
        // A Flashpoint legacy game: its other files under the install's htdocs, by host.
        let dir = match side.get("htdocs") {
            Some(h) if Path::new(h).is_dir() => PathBuf::from(h),
            _ => path.parent().unwrap_or(Path::new(".")).to_path_buf(),
        };
        let launch = match side.get("launch") {
            // A loose movie that a site served (a file copied from Flashpoint's htdocs): keep its address.
            Some(addr) if Url::parse(addr).is_ok() => addr.clone(),
            _ => Url::from_file_path(path.canonicalize().unwrap_or(path.to_path_buf()))
                .map_err(|_| "bad path".to_string())?
                .to_string(),
        };
        Ok(Content { source: Source::Folder(dir), launch, movie, parameters: Vec::new(), details: side })
    }

    fn open_zip(path: &Path) -> Result<Content, String> {
        let file = File::open(path).map_err(|e| format!("{}: {e}", path.display()))?;
        // Buffered: a zip's directory and entries are read in many small pieces.
        let mut archive = zip::ZipArchive::new(BufReader::new(file)).map_err(|e| format!("{}: {e}", path.display()))?;
        let mut index = HashMap::new();
        let mut by_name: HashMap<String, Vec<usize>> = HashMap::new();
        let mut meta = None;
        let mut largest: Option<(u64, String)> = None;
        for i in 0..archive.len() {
            let Ok(entry) = archive.by_index_raw(i) else { continue };
            let Ok(name) = entry.name().map(|n| n.to_string()) else { continue };
            let size = entry.size();
            drop(entry);
            let base = name.rsplit('/').next().unwrap_or(&name).to_ascii_lowercase();
            if (base == "meta.yaml" || base == "meta.yml") && name.matches('/').count() <= 1 {
                meta = Some(i);
                continue;
            }
            let Some(key) = entry_key(&name) else { continue };
            if key.ends_with(".swf") && largest.as_ref().is_none_or(|(s, _)| size > *s) {
                largest = Some((size, key.clone()));
            }
            by_name.entry(file_name(&key).to_string()).or_default().push(i);
            index.insert(key, i);
        }
        let side = sidecar(path);
        let mut launch = side.get("launch").cloned();
        if launch.is_none()
            && let Some(i) = meta
        {
            let mut text = String::new();
            if let Ok(mut entry) = archive.by_index(i) {
                let _ = entry.read_to_string(&mut text);
            }
            launch = yaml_launch(&text);
        }
        let launch = match launch {
            Some(l) => l,
            None => {
                let (_, key) = largest.ok_or_else(|| "no .swf in the pack".to_string())?;
                let (host, rest) = key.split_once('/').unwrap_or((LOCAL_HOST, &key));
                format!("http://{host}/{rest}")
            }
        };
        let content = Content {
            source: Source::Zip { archive: Mutex::new(archive), index, by_name },
            launch: launch.clone(),
            movie: Vec::new(),
            parameters: Vec::new(),
            details: HashMap::new(),
        };
        let mut content = content.with_movie(launch, &side)?;
        content.details = side;
        Ok(content)
    }

    /// Reads the movie at the launch address; a launch page (Flashpoint names some games by the page that held
    /// them) is followed to the movie it embeds.
    fn with_movie(mut self, launch: String, side: &HashMap<String, String>) -> Result<Content, String> {
        let url = Url::parse(&launch).map_err(|e| format!("launch address {launch}: {e}"))?;
        let bytes = self.read(&url).ok_or_else(|| format!("{launch} is not in the pack"))?;
        let lower = url.path().to_ascii_lowercase();
        if lower.ends_with(".htm") || lower.ends_with(".html") || lower.ends_with(".php") || lower.ends_with('/') {
            let page = String::from_utf8_lossy(&bytes);
            let (movie, vars) = movie_in_page(&page).ok_or_else(|| format!("{launch} embeds no movie"))?;
            let movie_url = url.join(&movie).map_err(|e| e.to_string())?;
            self.movie = self.read(&movie_url).ok_or_else(|| format!("{movie_url} is not in the pack"))?;
            self.launch = movie_url.to_string();
            self.parameters = vars;
        } else {
            self.movie = bytes;
            self.launch = launch;
        }
        if let Some(vars) = side.get("flashvars") {
            for pair in vars.split('&') {
                if let Some((k, v)) = pair.split_once('=') {
                    let v = percent_encoding::percent_decode_str(v).decode_utf8_lossy().to_string();
                    self.parameters.push((k.to_string(), v));
                }
            }
        }
        Ok(self)
    }

    /// A file by its web address, or None when the pack doesn't hold it.
    pub fn read(&self, url: &Url) -> Option<Vec<u8>> {
        match &self.source {
            Source::Folder(dir) => {
                if url.scheme() == "file" {
                    let mut u = url.clone();
                    u.set_query(None);
                    return std::fs::read(u.to_file_path().ok()?).ok();
                }
                let key = lookup_key(url)?;
                let (host, rest) = key.split_once('/')?;
                let mut tries = vec![dir.join(host).join(rest)];
                tries.push(dir.join(rest));
                tries.push(dir.join(file_name(&key)));
                tries.into_iter().find_map(|p| read_folded(&p))
            }
            Source::Zip { archive, index, by_name } => {
                let key = lookup_key(url)?;
                let i = index.get(&key).copied().or_else(|| {
                    // The same file under another host (a game moved between its own site and a portal's), when
                    // only one file in the pack has that name.
                    let same = by_name.get(file_name(&key))?;
                    (same.len() == 1).then(|| same[0])
                })?;
                let mut archive = archive.lock().ok()?;
                let mut entry = archive.by_index(i).ok()?;
                let mut out = Vec::with_capacity(entry.size() as usize);
                entry.read_to_end(&mut out).ok()?;
                Some(out)
            }
        }
    }
}

/// Reads a path whose letter case may differ from the card's (sites were case-insensitive).
fn read_folded(path: &Path) -> Option<Vec<u8>> {
    if let Ok(b) = std::fs::read(path) {
        return Some(b);
    }
    let mut at = PathBuf::from("/");
    for part in path.components().skip(1) {
        let want = part.as_os_str().to_string_lossy().to_lowercase();
        let found =
            std::fs::read_dir(&at).ok()?.flatten().find(|e| e.file_name().to_string_lossy().to_lowercase() == want)?;
        at = found.path();
    }
    std::fs::read(at).ok()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn keys() {
        assert_eq!(entry_key("content/www.Example.com/Games/A%20b.swf").unwrap(), "www.example.com/games/a b.swf");
        assert_eq!(entry_key("Game/content/host/x.swf").unwrap(), "host/x.swf");
        assert_eq!(entry_key("game.swf").unwrap(), "localhost/game.swf");
        assert!(entry_key("content/").is_none());
    }

    #[test]
    fn page() {
        let (m, v) =
            movie_in_page(r#"<embed src="games/run.swf" flashvars="lvl=2&name=a%20b" width=550></embed>"#).unwrap();
        assert_eq!(m, "games/run.swf");
        assert_eq!(v, vec![("lvl".into(), "2".into()), ("name".into(), "a b".into())]);
    }

    #[test]
    fn yaml() {
        assert_eq!(
            yaml_launch("Title: X\nLaunch Command: http://a.com/b.swf\n").as_deref(),
            Some("http://a.com/b.swf")
        );
    }
}
