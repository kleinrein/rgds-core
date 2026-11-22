//! What the player tells the screen around it: whether the game hides the pointer, and whether a text field
//! wants typing (the player then offers the on-screen keyboard).

use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};

use ruffle_core::backend::ui::{
    DialogResultFuture, FileFilter, FontDefinition, FullscreenError, LanguageIdentifier, MouseCursor,
    MultiDialogResultFuture, NullUiBackend, UiBackend,
};
use ruffle_core::font::FontQuery;
use url::Url;

#[derive(Default)]
pub struct Shared {
    pub mouse_visible: AtomicBool,
    pub wants_keyboard: AtomicBool,
    pub hand_cursor: AtomicBool,
}

pub struct PlayUi {
    inner: NullUiBackend,
    shared: Arc<Shared>,
}

impl PlayUi {
    pub fn new(shared: Arc<Shared>) -> Self {
        shared.mouse_visible.store(true, Ordering::Relaxed);
        Self { inner: NullUiBackend::new(), shared }
    }
}

impl UiBackend for PlayUi {
    fn mouse_visible(&self) -> bool {
        self.shared.mouse_visible.load(Ordering::Relaxed)
    }
    fn set_mouse_visible(&mut self, visible: bool) {
        self.shared.mouse_visible.store(visible, Ordering::Relaxed);
    }
    fn set_mouse_cursor(&mut self, cursor: MouseCursor) {
        self.shared.hand_cursor.store(cursor == MouseCursor::Hand, Ordering::Relaxed);
    }
    fn clipboard_content(&mut self) -> String {
        String::new()
    }
    fn set_clipboard_content(&mut self, _content: String) {}
    fn set_fullscreen(&mut self, _is_full: bool) -> Result<(), FullscreenError> {
        Ok(())
    }
    fn display_root_movie_download_failed_message(&self, invalid_swf: bool, error: String) {
        tracing::error!("flash: the game could not start (invalid swf: {invalid_swf}): {error}");
    }
    fn message(&self, message: &str) {
        tracing::info!("flash: {message}");
    }
    fn open_virtual_keyboard(&self) {
        self.shared.wants_keyboard.store(true, Ordering::Relaxed);
    }
    fn close_virtual_keyboard(&self) {
        self.shared.wants_keyboard.store(false, Ordering::Relaxed);
    }
    fn language(&self) -> LanguageIdentifier {
        self.inner.language()
    }
    fn display_unsupported_video(&self, url: Url) {
        tracing::warn!("flash: unsupported video {url}");
    }
    fn load_device_font(&self, query: &FontQuery, register: &mut dyn FnMut(FontDefinition)) {
        self.inner.load_device_font(query, register)
    }
    fn sort_device_fonts(&self, query: &FontQuery, register: &mut dyn FnMut(FontDefinition)) -> Vec<FontQuery> {
        self.inner.sort_device_fonts(query, register)
    }
    fn display_file_open_dialog(&mut self, filters: Vec<FileFilter>) -> Option<DialogResultFuture> {
        self.inner.display_file_open_dialog(filters)
    }
    fn display_file_open_dialog_multiple(&mut self, filters: Vec<FileFilter>) -> Option<MultiDialogResultFuture> {
        self.inner.display_file_open_dialog_multiple(filters)
    }
    fn display_file_save_dialog(&mut self, file_name: String, title: String) -> Option<DialogResultFuture> {
        self.inner.display_file_save_dialog(file_name, title)
    }
    fn close_file_dialog(&mut self) {}
}
