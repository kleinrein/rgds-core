//! Flash's local saves (SharedObjects), one file each under the game's save folder.

use std::path::{Component, Path, PathBuf};

use ruffle_core::backend::storage::StorageBackend;

pub struct FolderStorage {
    root: PathBuf,
}

impl FolderStorage {
    pub fn new(root: PathBuf) -> Self {
        let _ = std::fs::create_dir_all(&root);
        Self { root }
    }

    /// "localhost/games/run.swf/best" -> <root>/localhost/games/run.swf/best.sol, refusing names that climb out.
    fn path(&self, name: &str) -> Option<PathBuf> {
        let rel = Path::new(name);
        if name.is_empty() || !rel.components().all(|c| matches!(c, Component::Normal(_))) {
            return None;
        }
        Some(self.root.join(format!("{name}.sol")))
    }
}

impl StorageBackend for FolderStorage {
    fn get(&self, name: &str) -> Option<Vec<u8>> {
        std::fs::read(self.path(name)?).ok()
    }

    fn put(&mut self, name: &str, value: &[u8]) -> bool {
        let Some(path) = self.path(name) else { return false };
        if let Some(dir) = path.parent() {
            let _ = std::fs::create_dir_all(dir);
        }
        // Written beside and renamed, so power lost mid-write keeps the last good save.
        let tmp = path.with_extension("sol.tmp");
        std::fs::write(&tmp, value).is_ok() && std::fs::rename(&tmp, &path).is_ok()
    }

    fn remove_key(&mut self, name: &str) {
        if let Some(path) = self.path(name) {
            let _ = std::fs::remove_file(path);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn names() {
        let s = FolderStorage { root: PathBuf::from("/s") };
        assert_eq!(s.path("a.com/x.swf/best").unwrap(), PathBuf::from("/s/a.com/x.swf/best.sol"));
        assert!(s.path("../x").is_none());
        assert!(s.path("/abs").is_none());
        assert!(s.path("").is_none());
    }
}
