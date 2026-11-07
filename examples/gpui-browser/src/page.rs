//! A tab's page: a zebkit view whose callbacks become events for the
//! browser. The engine calls these on GPUI's main thread, inside
//! `zebkit::pump()`; they only queue, and the browser handles them next.

use futures::channel::mpsc::UnboundedSender;
use zebkit::{
    CertificateRequest, DownloadRequest, FileChooserRequest, HttpAuthRequest, LoadEvent, MediaAccessRequest, PermissionRequest, View,
    page::Dialog,
};

pub type TabId = u64;

pub enum PageEvent {
    Load(LoadEvent, String),
    Title(String),
    Closed,
    /// window.open(), target=_blank: a new view, still linked to its opener.
    Opened(View, String),
    PopupBlocked,
    /// The page's alert(), confirm(), prompt() or "Leave site?".
    Dialog(Dialog),
    Permission(PermissionRequest),
    Media(MediaAccessRequest),
    Download(DownloadRequest),
    DownloadProgress(String),
    Auth(HttpAuthRequest),
    Certificate(CertificateRequest),
    FileChooser(FileChooserRequest),
    /// A browser shortcut pressed while the page had the keyboard.
    Key(String),
}

impl PageEvent {
    /// Whether the browser answers it with a dialog.
    pub fn asks(&self) -> bool {
        matches!(
            self,
            PageEvent::Dialog(_)
                | PageEvent::Permission(_)
                | PageEvent::Media(_)
                | PageEvent::Download(_)
                | PageEvent::Auth(_)
                | PageEvent::Certificate(_)
        )
    }
}

/// The browser's shortcuts, which the page does not get: the engine tells
/// the application instead (`View::set_reserved_keys`).
pub const RESERVED_KEYS: &[&str] = if cfg!(target_os = "macos") {
    &[
        "Meta+KeyL",
        "Meta+KeyT",
        "Meta+KeyW",
        "Meta+KeyR",
        "Meta+KeyF",
        "Ctrl+Tab",
        "Ctrl+Shift+Tab",
        "Meta+BracketLeft",
        "Meta+BracketRight",
    ]
} else {
    &[
        "Ctrl+KeyL",
        "Ctrl+KeyT",
        "Ctrl+KeyW",
        "Ctrl+KeyR",
        "Ctrl+KeyF",
        "Ctrl+Tab",
        "Ctrl+Shift+Tab",
        "Alt+ArrowLeft",
        "Alt+ArrowRight",
        "F5",
    ]
};

/// Hands each of the view's callbacks and decisions to the browser.
pub fn connect(view: &View, tab: TabId, events: &UnboundedSender<(TabId, PageEvent)>) {
    macro_rules! send {
        ($event:expr) => {{
            let events = events.clone();
            move |value| drop(events.unbounded_send((tab, $event(value))))
        }};
    }
    let events2 = events.clone();
    view.on_load(move |event, url| drop(events2.unbounded_send((tab, PageEvent::Load(event, url.to_string())))));
    view.on_title(send!(|title: &str| PageEvent::Title(title.to_string())));
    let events2 = events.clone();
    view.on_close(move || drop(events2.unbounded_send((tab, PageEvent::Closed))));
    let events2 = events.clone();
    view.on_window_open(move |view, url| drop(events2.unbounded_send((tab, PageEvent::Opened(view, url.to_string())))));
    view.on_popup_blocked(send!(|_: &str| PageEvent::PopupBlocked));
    view.on_dialog(send!(|dialog: &Dialog| PageEvent::Dialog(dialog.clone())));
    view.on_permission(send!(PageEvent::Permission));
    view.on_media_access(send!(PageEvent::Media));
    view.on_download(send!(PageEvent::Download));
    view.on_download_progress(send!(|json: &str| PageEvent::DownloadProgress(json.to_string())));
    view.on_http_auth(send!(PageEvent::Auth));
    view.on_certificate(send!(PageEvent::Certificate));
    view.on_file_chooser(send!(PageEvent::FileChooser));
    let keys = serde_json::to_string(RESERVED_KEYS).unwrap();
    let _ = view.set_reserved_keys(&keys);
    view.on_reserved_key(send!(|json: &str| PageEvent::Key(json.to_string())));
}
