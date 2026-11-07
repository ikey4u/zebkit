//! The browser: GPUI-KIT tabs, toolbar and dialogs around zebkit pages.

use crate::embed::PageHost;
use crate::page::{self, PageEvent, TabId};
use futures::{FutureExt as _, StreamExt as _, channel::mpsc::UnboundedSender};
use gpui_kit::assets::IconName as Lucide;
use gpui_kit::component::{
    ActiveTheme as _, Icon, IconName, Sizable as _, Theme, ThemeMode, TitleBar, WindowExt as _,
    button::{Button, ButtonVariants as _},
    dialog::{DialogAction, DialogClose, DialogEntrance, DialogFooter},
    h_flex,
    input::{Input, InputEvent, InputState},
    menu::{DropdownMenu as _, PopupMenuItem},
    spinner::Spinner,
    tab::{Tab, TabBar},
    tag::Tag,
    v_flex,
};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::*;
use std::{path::PathBuf, rc::Rc, sync::Arc, time::Duration};
use zebkit::{LoadEvent, PermissionState, View, page::DialogType};

actions!(browser, [FocusAddress, NewTab, CloseTab, Reload, NextTab, PreviousTab, Back, Forward, Find, ToggleTheme]);

pub const START_PAGE: &str = "browser://start/";

/// The shortcuts, in GPUI's notation: Cmd on macOS, Ctrl elsewhere.
pub fn bind_keys(cx: &mut App) {
    macro_rules! primary {
        ($key:literal) => {
            if cfg!(target_os = "macos") { concat!("cmd-", $key) } else { concat!("ctrl-", $key) }
        };
    }
    cx.bind_keys([
        KeyBinding::new(primary!("l"), FocusAddress, None),
        KeyBinding::new(primary!("t"), NewTab, None),
        KeyBinding::new(primary!("w"), CloseTab, None),
        KeyBinding::new(primary!("r"), Reload, None),
        KeyBinding::new("f5", Reload, None),
        KeyBinding::new(primary!("f"), Find, None),
        KeyBinding::new("ctrl-tab", NextTab, None),
        KeyBinding::new("ctrl-shift-tab", PreviousTab, None),
        KeyBinding::new("alt-left", Back, None),
        KeyBinding::new("alt-right", Forward, None),
    ]);
}

struct BrowserTab {
    id: TabId,
    view: View,
    title: SharedString,
    url: String,
    loading: bool,
    favicon: Option<Arc<Image>>,
}

pub struct Browser {
    tabs: Vec<BrowserTab>,
    active: usize,
    host: Rc<PageHost>,
    events: UnboundedSender<(TabId, PageEvent)>,
    next_id: TabId,
    address: Entity<InputState>,
    find: Entity<InputState>,
    finding: bool,
    focus: FocusHandle,
    smoke: Option<crate::Smoke>,
    /// Without composition (Windows) GPUI cannot draw over a page's window:
    /// under a dialog the page is hidden and this picture of it shown.
    picture: Option<Arc<Image>>,
    /// A word in the toolbar for a few seconds (a download done, a pop-up
    /// blocked), with how many were told, so that only the last is cleared.
    status: Option<(bool, SharedString)>,
    told: usize,
    /// Sites whose certificate errors the user chose to continue past.
    trusted_hosts: Rc<std::cell::RefCell<std::collections::HashSet<String>>>,
}

impl Browser {
    pub fn new(urls: Vec<String>, smoke: Option<crate::Smoke>, window: &mut Window, cx: &mut Context<Self>) -> Self {
        let address = cx.new(|cx| InputState::new(window, cx).placeholder("Search or enter an address"));
        let find = cx.new(|cx| InputState::new(window, cx).placeholder("Find in page"));
        cx.subscribe_in(&address, window, |this, input, event, window, cx| {
            if let InputEvent::PressEnter { .. } = event {
                let text = input.read(cx).value().trim().to_string();
                if !text.is_empty() {
                    this.navigate(&text, window, cx);
                }
            }
        })
        .detach();
        cx.subscribe_in(&find, window, |this, input, event, _, cx| {
            if let InputEvent::PressEnter { shift, .. } = event {
                let text = serde_json::to_string(input.read(cx).value().as_ref()).unwrap();
                if let Some(tab) = this.tabs.get(this.active) {
                    tab.view.evaluate(&format!("window.find({text}, false, {shift}, true)"), |_| {});
                }
            }
        })
        .detach();

        // The pages' events, handled one at a time after each pump.
        let (events, mut received) = futures::channel::mpsc::unbounded::<(TabId, PageEvent)>();
        cx.spawn_in(window, async move |this, cx| {
            while let Some((tab, event)) = received.next().await {
                // A question for the user: a GPUI-KIT dialog. Without composition (Windows) the
                // page's window would cover it, so the page is hidden and shown as a picture.
                if event.asks() {
                    let Ok(picture) = this.update_in(cx, |this, window, cx| this.picture_of(tab, window, cx)) else { break };
                    // No picture of a page blocked in alert(): its renderer draws no more.
                    let timeout = cx.background_executor().timer(Duration::from_millis(400));
                    let picture = futures::select_biased! { png = picture.fuse() => png.ok().flatten(), _ = timeout.fuse() => None };
                    let _ = this.update(cx, |this, _| this.picture = picture.map(|png| Arc::new(Image::from_bytes(ImageFormat::Png, png))));
                }
                if this.update_in(cx, |this, window, cx| this.handle(tab, event, window, cx)).is_err() {
                    break;
                }
            }
        })
        .detach();

        let mut browser = Browser {
            tabs: Vec::new(),
            active: 0,
            host: Rc::new(PageHost::new(window)),
            events,
            next_id: 1,
            address,
            find,
            finding: false,
            focus: cx.focus_handle(),
            smoke,
            picture: None,
            trusted_hosts: Default::default(),
            status: None,
            told: 0,
        };
        for url in urls {
            browser.open_tab(Some(&url), window, cx);
        }
        browser
    }

    // ---- Tabs --------------------------------------------------------------

    fn open_tab(&mut self, url: Option<&str>, window: &mut Window, cx: &mut Context<Self>) {
        let view = View::new(Some(url.unwrap_or(START_PAGE)), 0, 0);
        self.add_tab(view, url.unwrap_or(START_PAGE).to_string(), window, cx);
        if url.is_none() {
            self.focus_address(&FocusAddress, window, cx);
        }
    }

    fn add_tab(&mut self, view: View, url: String, window: &mut Window, cx: &mut Context<Self>) {
        self.host.attach(&view);
        page::connect(&view, self.next_id, &self.events);
        let scheme = if cx.theme().mode.is_dark() { "dark" } else { "light" };
        let _ = view.set_settings(&format!(r#"{{"colorScheme":"{scheme}"}}"#));
        self.tabs.push(BrowserTab { id: self.next_id, view, title: "New Tab".into(), url, loading: true, favicon: None });
        self.next_id += 1;
        self.select(self.tabs.len() - 1, window, cx);
    }

    fn select(&mut self, index: usize, window: &mut Window, cx: &mut Context<Self>) {
        self.active = index.min(self.tabs.len().saturating_sub(1));
        for (i, tab) in self.tabs.iter().enumerate() {
            tab.view.set_visible(i == self.active);
        }
        // The keyboard to the page shown (a hidden page's window drops it).
        if let Some(view) = self.active_view() {
            view.focus();
        }
        self.show_address(window, cx);
        cx.notify();
    }

    fn close_tab_by_id(&mut self, id: TabId) {
        // Like a browser's tab: the page's "Leave site?" first; then on_close.
        if let Some(tab) = self.tabs.iter().find(|tab| tab.id == id) {
            tab.view.request_close();
        }
    }

    fn index_of(&self, id: TabId) -> Option<usize> {
        self.tabs.iter().position(|tab| tab.id == id)
    }

    /// Brings the tab to the front and takes a picture of its page.
    fn picture_of(
        &mut self,
        tab: TabId,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> futures::channel::oneshot::Receiver<Option<Vec<u8>>> {
        let (done, picture) = futures::channel::oneshot::channel();
        match self.index_of(tab) {
            Some(index) => {
                self.select(index, window, cx);
                if self.host.overlays_above() {
                    let _ = done.send(None);
                } else {
                    self.tabs[index].view.capture(move |png| drop(done.send(png.ok())));
                }
            }
            None => drop(done.send(None)),
        }
        picture
    }

    fn active_view(&self) -> Option<&View> {
        self.tabs.get(self.active).map(|tab| &tab.view)
    }

    // ---- The address bar --------------------------------------------------

    fn navigate(&mut self, text: &str, window: &mut Window, cx: &mut Context<Self>) {
        let url = if text.contains("://") || text.starts_with("about:") || text.starts_with("data:") {
            text.to_string()
        } else if text.starts_with("localhost") {
            format!("http://{text}")
        } else if !text.contains(' ') && text.contains('.') {
            format!("https://{text}")
        } else {
            let query: String = text
                .bytes()
                .map(|b| match b {
                    b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'_' | b'.' | b'~' => (b as char).to_string(),
                    b' ' => "+".into(),
                    _ => format!("%{b:02X}"),
                })
                .collect();
            format!("https://duckduckgo.com/?q={query}")
        };
        if let Some(view) = self.active_view() {
            view.load_url(&url);
            view.focus();
        }
        window.focus(&self.focus, cx);
        cx.notify();
    }

    fn show_address(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let url = self.tabs.get(self.active).map(|tab| tab.url.clone()).unwrap_or_default();
        let shown = if url.starts_with("browser://start") { String::new() } else { url };
        self.address.update(cx, |input, cx| input.set_value(shown, window, cx));
    }

    // ---- What the pages tell and ask ---------------------------------------

    fn handle(&mut self, id: TabId, event: PageEvent, window: &mut Window, cx: &mut Context<Self>) {
        let Some(index) = self.index_of(id) else { return };
        match event {
            PageEvent::Load(state, url) => {
                let tab = &mut self.tabs[index];
                tab.loading = state == LoadEvent::Started || (state == LoadEvent::Committed && tab.view.is_loading());
                if state == LoadEvent::Committed {
                    // Until the page names itself (a PDF, a JSON file do not).
                    tab.title = url.split("://").nth(1).unwrap_or(&url).trim_end_matches('/').to_string().into();
                    tab.url = url;
                    tab.favicon = None;
                    if index == self.active && !self.address.read(cx).focus_handle(cx).is_focused(window) {
                        self.show_address(window, cx);
                    }
                }
                if state == LoadEvent::Finished {
                    self.load_favicon(id, cx);
                    if let Some(smoke) = self.smoke.as_mut() {
                        smoke.loaded(&self.tabs[index].view, &self.tabs[index].url, cx);
                    }
                }
            }
            PageEvent::Title(title) => self.tabs[index].title = if title.is_empty() { "Untitled".into() } else { title.into() },
            PageEvent::Closed => {
                self.tabs.remove(index);
                if self.tabs.is_empty() {
                    cx.quit();
                    return;
                }
                let active =
                    if index < self.active || self.active >= self.tabs.len() { self.active.saturating_sub(1) } else { self.active };
                self.select(active, window, cx);
            }
            PageEvent::Opened(view, url) => self.add_tab(view, url, window, cx),
            PageEvent::PopupBlocked => self.tell(false, "Pop-up blocked".into(), cx),
            PageEvent::Dialog(dialog) => {
                self.select(index, window, cx);
                self.page_dialog(self.tabs[index].view.clone(), dialog, window, cx);
            }
            PageEvent::Permission(request) => {
                let info: serde_json::Value = serde_json::from_str(request.metadata()).unwrap_or_default();
                let what = match info["permission"].as_str().unwrap_or("") {
                    "notifications" => "show notifications",
                    "geolocation" | "geolocation-approximate" => "know your location",
                    "camera" | "video-capture" => "use your camera",
                    "microphone" | "audio-capture" => "use your microphone",
                    "clipboard-read" => "see text and images copied to the clipboard",
                    other => other,
                }
                .to_string();
                let origin = info["requestingOrigin"].as_str().unwrap_or("This page").to_string();
                let (allow, block) = (request.clone(), request);
                self.ask(
                    Ask::new(
                        Some(Lucide::ShieldQuestionMark),
                        format!("{} wants to {what}", host_of(&origin)),
                        origin,
                        "Allow",
                        Some("Block"),
                    ),
                    move |_| {
                        allow.respond(PermissionState::Granted, true);
                    },
                    move || {
                        block.respond(PermissionState::Denied, true);
                    },
                    window,
                    cx,
                );
            }
            PageEvent::Media(request) => {
                let info: serde_json::Value = serde_json::from_str(request.metadata()).unwrap_or_default();
                let what = match (info["video"].as_bool() == Some(true), info["audio"].as_bool() == Some(true)) {
                    (true, true) => "use your camera and microphone",
                    (true, false) => "use your camera",
                    _ => "use your microphone",
                };
                let origin = info["origin"].as_str().unwrap_or("").to_string();
                let (allow, block) = (request.clone(), request);
                self.ask(
                    Ask::new(Some(Lucide::Camera), format!("{} wants to {what}", host_of(&origin)), origin, "Allow", Some("Block")),
                    move |_| {
                        let _ = allow.respond(true, None, None);
                    },
                    move || {
                        let _ = block.respond(false, None, None);
                    },
                    window,
                    cx,
                );
            }
            PageEvent::Download(request) => {
                let info: serde_json::Value = serde_json::from_str(request.metadata()).unwrap_or_default();
                let name = info["suggestedName"].as_str().filter(|n| !n.is_empty()).unwrap_or("download").to_string();
                let size = info["totalBytes"].as_u64().map(|n| format!(" ({})", human_size(n))).unwrap_or_default();
                let path = free_path(&downloads_dir(), &name);
                let (save, cancel) = (request.clone(), request);
                self.ask(
                    Ask::new(
                        Some(Lucide::Download),
                        format!("Save {name}{size}?"),
                        "It goes to your Downloads folder.".into(),
                        "Save",
                        Some("Cancel"),
                    ),
                    move |_| {
                        let _ = std::fs::create_dir_all(path.parent().unwrap());
                        let _ = save.save(&path.to_string_lossy());
                    },
                    move || {
                        cancel.cancel();
                    },
                    window,
                    cx,
                );
            }
            PageEvent::DownloadProgress(json) => {
                let info: serde_json::Value = serde_json::from_str(&json).unwrap_or_default();
                if info["state"] == "complete" {
                    let name = info["path"]
                        .as_str()
                        .and_then(|p| PathBuf::from(p).file_name().map(|n| n.to_string_lossy().into_owned()))
                        .unwrap_or_default();
                    self.tell(true, format!("{name} downloaded"), cx);
                }
            }
            PageEvent::Auth(request) => self.sign_in(request, window, cx),
            PageEvent::Certificate(request) => {
                let info: serde_json::Value = serde_json::from_str(request.metadata()).unwrap_or_default();
                let url = info["url"].as_str().unwrap_or("").to_string();
                let host = host_of(&url);
                if self.trusted_hosts.borrow().contains(&host) {
                    request.respond(true);
                    return;
                }
                let error = match info["error"].as_i64() {
                    Some(-200) => "its certificate is for another site".to_string(),
                    Some(-201) => "its certificate has expired or is not valid yet".to_string(),
                    Some(-202) => "its certificate is not from an authority the engine trusts".to_string(),
                    Some(code) => format!("its certificate is not valid (error {code})"),
                    None => info["error"].as_str().unwrap_or("its certificate is not valid").to_string(),
                };
                // Sites that require a valid certificate (HSTS) cannot be continued to.
                let can_continue = info["canIgnore"].as_bool() != Some(false);
                let mut question = Ask::new(
                    Some(Lucide::ShieldAlert),
                    "Untrusted certificate".into(),
                    format!("{host}: {error}."),
                    "Continue",
                    Some("Go back"),
                );
                if !can_continue {
                    (question.yes, question.no) = ("Go back", None);
                }
                let (proceed, back, trusted) = (request.clone(), request, self.trusted_hosts.clone());
                self.ask(
                    question,
                    move |_| {
                        // For the site's other requests too, until the browser closes.
                        if can_continue {
                            trusted.borrow_mut().insert(host.clone());
                        }
                        proceed.respond(can_continue);
                    },
                    move || {
                        back.respond(false);
                    },
                    window,
                    cx,
                );
            }
            PageEvent::FileChooser(request) => {
                let paths = cx.prompt_for_paths(PathPromptOptions { files: true, directories: false, multiple: false, prompt: None });
                cx.spawn(async move |_, _| {
                    let chosen = paths.await.ok().and_then(|r| r.ok()).flatten().unwrap_or_default();
                    let chosen: Vec<String> = chosen.iter().map(|p| p.to_string_lossy().into_owned()).collect();
                    let chosen: Vec<&str> = chosen.iter().map(String::as_str).collect();
                    let _ = request.respond(&chosen);
                })
                .detach();
            }
            PageEvent::Key(json) => {
                let info: serde_json::Value = serde_json::from_str(&json).unwrap_or_default();
                let combo = info["combo"].as_str().unwrap_or("");
                let key = combo.rsplit('+').next().unwrap_or("");
                let action: Box<dyn Action> = match (combo.contains("Shift"), key) {
                    (_, "KeyL") => Box::new(FocusAddress),
                    (_, "KeyT") => Box::new(NewTab),
                    (_, "KeyW") => Box::new(CloseTab),
                    (_, "KeyR") | (_, "F5") => Box::new(Reload),
                    (_, "KeyF") => Box::new(Find),
                    (false, "Tab") | (_, "BracketRight") => Box::new(NextTab),
                    (true, "Tab") | (_, "BracketLeft") => Box::new(PreviousTab),
                    (_, "ArrowLeft") => Box::new(Back),
                    (_, "ArrowRight") => Box::new(Forward),
                    _ => return,
                };
                window.focus(&self.focus, cx);
                window.dispatch_action(action, cx);
            }
        }
        cx.notify();
    }

    /// A JavaScript dialog as a GPUI-KIT dialog; the answer goes back to the page.
    fn page_dialog(&mut self, view: View, dialog: zebkit::page::Dialog, window: &mut Window, cx: &mut Context<Self>) {
        let id = dialog.id;
        let mut question = Ask::new(None, format!("{} says", host_of(&dialog.url)), dialog.message.clone(), "OK", Some("Cancel"));
        match dialog.kind {
            DialogType::Prompt => {
                question.fields.push(cx.new(|cx| InputState::new(window, cx).default_value(dialog.default_prompt.clone())))
            }
            DialogType::BeforeUnload => {
                question = Ask::new(None, "Leave site?".into(), "Changes you made may not be saved.".into(), "Leave", Some("Stay"));
            }
            DialogType::Alert | DialogType::Print => question.no = None,
            DialogType::Confirm => {}
        }
        let (ok, cancel) = (view.clone(), view);
        self.ask(
            question,
            move |text| {
                ok.respond_dialog(id, true, text.first().map(String::as_str));
            },
            move || {
                cancel.respond_dialog(id, false, None);
            },
            window,
            cx,
        );
    }

    /// A question from the engine as a GPUI-KIT dialog: its fields' text to `on_yes`.
    fn ask(
        &mut self,
        question: Ask,
        on_yes: impl Fn(Vec<String>) + 'static,
        on_no: impl Fn() + 'static,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        // The keyboard to GPUI for the dialog, and back to the page after it.
        self.host.focus_gpui();
        let this = cx.weak_entity();
        let page_again = move |cx: &mut App| drop(this.update(cx, |browser, _| browser.active_view().map(View::focus)));
        let on_yes = Rc::new({
            let page_again = page_again.clone();
            move |fields, cx: &mut App| {
                on_yes(fields);
                page_again(cx)
            }
        });
        let on_no = Rc::new(move |cx: &mut App| {
            on_no();
            page_again(cx)
        });
        let first = std::cell::Cell::new(true);
        window.open_dialog(cx, move |dialog, window, cx| {
            if let Some(field) = question.fields.first().filter(|_| first.replace(false)) {
                field.update(cx, |input, cx| input.focus(window, cx));
            }
            let (yes, no, fields) = (on_yes.clone(), on_no.clone(), question.fields.clone());
            let muted = cx.theme().muted_foreground;
            // No backdrop: GPUI cannot dim the page's native window everywhere.
            dialog
                .overlay(false)
                // No sliding: over a page without a compositing manager, X11 would
                // keep the frames of its way out on the page.
                .entrance(DialogEntrance::None)
                .close_button(false)
                .w(px(440.))
                .title(
                    h_flex()
                        .gap_2()
                        .items_center()
                        .when_some(question.icon.clone(), |title, icon| title.child(Icon::new(icon).text_color(cx.theme().primary)))
                        .child(question.title.clone()),
                )
                .child(
                    v_flex()
                        .gap_3()
                        .when(!question.detail.is_empty(), |body| {
                            body.child(div().text_sm().text_color(muted).child(question.detail.clone()))
                        })
                        .children(question.fields.iter().map(Input::new)),
                )
                .footer(
                    DialogFooter::new()
                        .when_some(question.no, |footer, no| footer.child(DialogClose::new().child(Button::new("no").outline().label(no))))
                        .child(DialogAction::new().child(Button::new("yes").primary().label(question.yes))),
                )
                .on_ok(move |_, _, cx| {
                    yes(fields.iter().map(|field| field.read(cx).value().to_string()).collect(), cx);
                    true
                })
                .on_cancel(move |_, _, cx| {
                    no(cx);
                    true
                })
        });
    }

    /// Something to tell in the toolbar, for five seconds.
    fn tell(&mut self, good: bool, text: String, cx: &mut Context<Self>) {
        self.status = Some((good, text.into()));
        self.told += 1;
        let told = self.told;
        cx.spawn(async move |this, cx| {
            cx.background_executor().timer(Duration::from_secs(5)).await;
            let _ = this.update(cx, |browser, cx| {
                if browser.told == told {
                    browser.status = None;
                    cx.notify();
                }
            });
        })
        .detach();
        cx.notify();
    }

    /// HTTP authentication: a GPUI-KIT form.
    fn sign_in(&mut self, request: zebkit::HttpAuthRequest, window: &mut Window, cx: &mut Context<Self>) {
        let info: serde_json::Value = serde_json::from_str(request.metadata()).unwrap_or_default();
        let site = host_of(info["challenger"].as_str().or(info["url"].as_str()).unwrap_or(""));
        let realm = info["realm"]
            .as_str()
            .filter(|realm| !realm.is_empty())
            .map(|realm| format!(" for \u{201c}{realm}\u{201d}"))
            .unwrap_or_default();
        let mut question = Ask::new(
            Some(Lucide::Lock),
            "Sign in".into(),
            format!("{site} asks for a username and password{realm}."),
            "Sign in",
            Some("Cancel"),
        );
        question.fields.push(cx.new(|cx| InputState::new(window, cx).placeholder("Username")));
        question.fields.push(cx.new(|cx| InputState::new(window, cx).placeholder("Password").masked(true)));
        let (provide, cancel) = (request.clone(), request);
        self.ask(
            question,
            move |fields| {
                let _ = provide.provide(&fields[0], &fields[1]);
            },
            move || {
                cancel.cancel();
            },
            window,
            cx,
        );
    }

    /// The site's icon, fetched by the engine's network stack and made a PNG by
    /// its image services.
    fn load_favicon(&mut self, id: TabId, cx: &mut Context<Self>) {
        let Some(index) = self.index_of(id) else { return };
        // PNG-like icons first, then .ico files (whose PNG entry is used).
        let script = r#"(() => {
            const links = [...document.querySelectorAll('link[rel~="icon"], link[rel="apple-touch-icon"]')].map(l => l.href);
            const ico = links.filter(h => /\.ico(\?|$)/.test(h));
            const fallback = location.protocol.startsWith('http') ? [location.origin + '/favicon.ico'] : [];
            return [...links.filter(h => h.startsWith('http') && !/\.(ico|svg)(\?|$)/.test(h)), ...ico, ...fallback];
        })()"#;
        let this = cx.weak_entity();
        let cx = cx.to_async();
        self.tabs[index].view.evaluate(script, move |links| {
            let links: Vec<String> = links.ok().and_then(|json| serde_json::from_str(&json).ok()).unwrap_or_default();
            fetch_icon(links, move |png| {
                let icon = Arc::new(Image::from_bytes(ImageFormat::Png, png));
                let _ = this.update(&mut cx.clone(), |browser, cx| {
                    if let Some(index) = browser.index_of(id) {
                        browser.tabs[index].favicon = Some(icon);
                        cx.notify();
                    }
                });
            });
        });
    }

    // ---- Actions -------------------------------------------------------------

    fn focus_address(&mut self, _: &FocusAddress, window: &mut Window, cx: &mut Context<Self>) {
        self.host.focus_gpui();
        self.address.update(cx, |input, cx| {
            input.focus(window, cx);
            input.select_all(window, cx);
        });
    }

    fn new_tab(&mut self, _: &NewTab, window: &mut Window, cx: &mut Context<Self>) {
        self.open_tab(None, window, cx);
    }

    fn close_tab(&mut self, _: &CloseTab, _: &mut Window, _: &mut Context<Self>) {
        if let Some(tab) = self.tabs.get(self.active) {
            self.close_tab_by_id(tab.id);
        }
    }

    fn reload(&mut self, _: &Reload, _: &mut Window, _: &mut Context<Self>) {
        if let Some(tab) = self.tabs.get(self.active) {
            if tab.loading { tab.view.stop() } else { tab.view.reload() }
        }
    }

    fn next_tab(&mut self, _: &NextTab, window: &mut Window, cx: &mut Context<Self>) {
        self.select((self.active + 1) % self.tabs.len().max(1), window, cx);
    }

    fn previous_tab(&mut self, _: &PreviousTab, window: &mut Window, cx: &mut Context<Self>) {
        let count = self.tabs.len().max(1);
        self.select((self.active + count - 1) % count, window, cx);
    }

    fn back(&mut self, _: &Back, _: &mut Window, _: &mut Context<Self>) {
        self.active_view().map(View::go_back);
    }

    fn forward(&mut self, _: &Forward, _: &mut Window, _: &mut Context<Self>) {
        self.active_view().map(View::go_forward);
    }

    fn find(&mut self, _: &Find, window: &mut Window, cx: &mut Context<Self>) {
        self.finding = !self.finding;
        if self.finding {
            self.host.focus_gpui();
            self.find.update(cx, |input, cx| input.focus(window, cx));
        } else if let Some(view) = self.active_view() {
            view.evaluate("getSelection().removeAllRanges()", |_| {});
            view.focus();
        }
        cx.notify();
    }

    /// GPUI-KIT's theme, and the pages' prefers-color-scheme with it.
    fn toggle_theme(&mut self, _: &ToggleTheme, window: &mut Window, cx: &mut Context<Self>) {
        let dark = !cx.theme().mode.is_dark();
        Theme::change(if dark { ThemeMode::Dark } else { ThemeMode::Light }, Some(window), cx);
        let settings = format!(r#"{{"colorScheme":"{}"}}"#, if dark { "dark" } else { "light" });
        for tab in &self.tabs {
            let _ = tab.view.set_settings(&settings);
        }
        cx.notify();
    }

    // ---- Drawing -----------------------------------------------------------

    fn render_tabs(&self, cx: &mut Context<Self>) -> impl IntoElement {
        TabBar::new("tabs")
            .folder()
            .small()
            .max_width(px(220.))
            .selected_index(self.active)
            .on_click(cx.listener(|this, index: &usize, window, cx| this.select(*index, window, cx)))
            .children(self.tabs.iter().map(|tab| {
                let icon = if tab.loading {
                    Spinner::new().xsmall().into_any_element()
                } else if let Some(favicon) = &tab.favicon {
                    img(favicon.clone()).size_4().into_any_element()
                } else {
                    Icon::new(IconName::Globe).xsmall().into_any_element()
                };
                Tab::new().label(tab.title.clone()).prefix(div().pl_2().child(icon)).suffix(
                    Button::new(("close", tab.id)).ghost().xsmall().icon(IconName::Close).on_click({
                        let id = tab.id;
                        cx.listener(move |this, _, _, cx| {
                            this.close_tab_by_id(id);
                            cx.stop_propagation();
                        })
                    }),
                )
            }))
            .last_empty_space(
                div().h_full().flex().items_center().pl_1().child(
                    Button::new("new-tab")
                        .ghost()
                        .xsmall()
                        .icon(IconName::Plus)
                        .on_click(cx.listener(|this, _, window, cx| this.open_tab(None, window, cx))),
                ),
            )
    }

    /// No tooltips: GPUI is not told when the pointer leaves for the page's
    /// window, so a tooltip shown over the page would stay.
    fn render_toolbar(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let loading = self.tabs.get(self.active).is_some_and(|tab| tab.loading);
        let secure = self.tabs.get(self.active).is_some_and(|tab| {
            (tab.url.starts_with("https://") && !self.trusted_hosts.borrow().contains(&host_of(&tab.url)))
                || tab.url.starts_with("browser://")
        });
        let dark = cx.theme().mode.is_dark();
        let this = cx.weak_entity();
        h_flex()
            .gap_1()
            .px_2()
            .py_1p5()
            .bg(cx.theme().tab_active)
            .border_b_1()
            .border_color(cx.theme().border)
            .child(
                Button::new("back")
                    .ghost()
                    .small()
                    .icon(IconName::ArrowLeft)
                    .on_click(cx.listener(|this, _, window, cx| this.back(&Back, window, cx))),
            )
            .child(
                Button::new("forward")
                    .ghost()
                    .small()
                    .icon(IconName::ArrowRight)
                    .on_click(cx.listener(|this, _, window, cx| this.forward(&Forward, window, cx))),
            )
            .child(
                Button::new("reload")
                    .ghost()
                    .small()
                    .icon(if loading { IconName::Close } else { IconName::RotateCw })
                    .on_click(cx.listener(|this, _, window, cx| this.reload(&Reload, window, cx))),
            )
            .child(
                div().flex_1().mx_1().child(Input::new(&self.address).small().prefix(
                    Icon::new(if secure { Lucide::Lock } else { Lucide::Search }).xsmall().text_color(cx.theme().muted_foreground),
                )),
            )
            .when_some(self.status.clone(), |bar, (good, text)| {
                let tag = if good { Tag::success() } else { Tag::warning() };
                bar.child(tag.small().child(
                    h_flex().gap_1().child(Icon::new(if good { IconName::CircleCheck } else { IconName::Ban }).xsmall()).child(text),
                ))
            })
            .child(
                Button::new("theme")
                    .ghost()
                    .small()
                    .icon(if dark { IconName::Sun } else { IconName::Moon })
                    .on_click(cx.listener(|this, _, window, cx| this.toggle_theme(&ToggleTheme, window, cx))),
            )
            .child(Button::new("menu").ghost().small().icon(IconName::Ellipsis).dropdown_menu(move |menu, _, _| {
                let item = |label: &'static str, icon: IconName, run: fn(&mut Browser, &mut Window, &mut Context<Browser>)| {
                    let this = this.clone();
                    PopupMenuItem::new(label).icon(icon).on_click(move |_, window, cx| {
                        let _ = this.update(cx, |browser, cx| run(browser, window, cx));
                    })
                };
                menu.item(item("New tab", IconName::Plus, |b, w, cx| b.open_tab(None, w, cx)))
                    .item(item("Find in page", IconName::Search, |b, w, cx| b.find(&Find, w, cx)))
                    .item(item("Save page as PDF", IconName::FileText, |b, w, cx| b.save_pdf(w, cx)))
                    .separator()
                    .item(item("About", IconName::Info, |b, w, cx| b.about(w, cx)))
            }))
    }

    fn render_find(&self, cx: &mut Context<Self>) -> impl IntoElement {
        h_flex()
            .gap_1()
            .px_2()
            .py_1()
            .bg(cx.theme().tab_active)
            .border_b_1()
            .border_color(cx.theme().border)
            .child(div().w(px(280.)).child(Input::new(&self.find).small().prefix(Icon::new(IconName::Search).xsmall())))
            .child(div().text_xs().text_color(cx.theme().muted_foreground).child("Enter: next · Shift+Enter: previous"))
            .child(div().flex_1())
            .child(
                Button::new("find-close")
                    .ghost()
                    .xsmall()
                    .icon(IconName::Close)
                    .on_click(cx.listener(|this, _, window, cx| this.find(&Find, window, cx))),
            )
    }

    /// The page printed to PDF by the engine, then opened in a new tab by its viewer.
    fn save_pdf(&mut self, _: &mut Window, cx: &mut Context<Self>) {
        let Some(view) = self.active_view() else { return };
        let this = cx.weak_entity();
        let mut cx = cx.to_async();
        view.print_to_pdf("{}", move |pdf| {
            let Ok(pdf) = pdf else { return };
            let _ = std::fs::create_dir_all(downloads_dir());
            let path = free_path(&downloads_dir(), "page.pdf");
            if std::fs::write(&path, pdf).is_ok() {
                let path = path.to_string_lossy().replace('\\', "/");
                let url = if path.starts_with('/') { format!("file://{path}") } else { format!("file:///{path}") };
                let _ = this.update_in(&mut cx, |browser, window, cx| browser.open_tab(Some(&url), window, cx));
            }
        });
    }

    fn about(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let text = format!("zebkit {} with Chromium {}, in a GPUI-KIT window.", zebkit::version(), zebkit::engine_version());
        self.ask(Ask::new(Some(Lucide::Globe), "A browser in GPUI-KIT".into(), text, "OK", None), |_| {}, || {}, window, cx);
    }
}

impl Render for Browser {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        // GPUI's overlays are cut out of the page where composition allows;
        // elsewhere, under a dialog, the page is its picture.
        let modal = !self.host.overlays_above() && window.has_active_dialog(cx);
        if let Some(view) = self.active_view() {
            view.set_visible(!modal);
        }
        if modal {
            cx.on_next_frame(window, |_, _, cx| cx.notify());
        } else {
            self.picture = None;
        }
        let picture = self.picture.clone().filter(|_| modal);
        let host = self.host.clone();
        let pages: Vec<View> = self.tabs.iter().map(|tab| tab.view.clone()).collect();
        v_flex()
            .size_full()
            .track_focus(&self.focus)
            .capture_any_mouse_down(cx.listener(|this, _, _, _| this.host.focus_gpui()))
            .bg(cx.theme().background)
            .on_action(cx.listener(Self::focus_address))
            .on_action(cx.listener(Self::new_tab))
            .on_action(cx.listener(Self::close_tab))
            .on_action(cx.listener(Self::reload))
            .on_action(cx.listener(Self::next_tab))
            .on_action(cx.listener(Self::previous_tab))
            .on_action(cx.listener(Self::back))
            .on_action(cx.listener(Self::forward))
            .on_action(cx.listener(Self::find))
            .on_action(cx.listener(Self::toggle_theme))
            .child(TitleBar::new().pl(px(if cfg!(target_os = "macos") { 72. } else { 4. })).child(self.render_tabs(cx)))
            .child(self.render_toolbar(cx))
            .when(self.finding, |this| this.child(self.render_find(cx)))
            // The content area: GPUI lays it out, the page follows it.
            .child(
                div()
                    .flex_1()
                    .w_full()
                    .relative()
                    .child(
                        canvas(move |bounds, window, _| host.layout(bounds, window.scale_factor(), pages.into_iter()), |_, _, _, _| {})
                            .size_full(),
                    )
                    .when_some(picture, |area, picture| area.child(img(picture).absolute().top_0().left_0().size_full())),
            )
    }
}

/// What a dialog asks: a title, a line under it, fields, and its buttons.
struct Ask {
    icon: Option<Lucide>,
    title: String,
    detail: String,
    fields: Vec<Entity<InputState>>,
    yes: &'static str,
    no: Option<&'static str>,
}

impl Ask {
    fn new(icon: Option<Lucide>, title: String, detail: String, yes: &'static str, no: Option<&'static str>) -> Self {
        Ask { icon, title, detail, fields: Vec::new(), yes, no }
    }
}

fn host_of(url: &str) -> String {
    if url.starts_with("browser:") {
        return "This page".into();
    }
    zebkit::services::url_part(url, None, zebkit::services::UrlPart::Host)
        .filter(|host| !host.is_empty())
        .unwrap_or_else(|| url.to_string())
}

/// The first of `links` that is an image, as a PNG.
fn fetch_icon(mut links: Vec<String>, done: impl FnOnce(Vec<u8>) + 'static) {
    if links.is_empty() {
        return;
    }
    let link = links.remove(0);
    zebkit::services::fetch("GET", &link, &[], &[], move |response| {
        let mut bytes = &response.body[..];
        // An .ico with a PNG inside: its directory's largest entry that is one.
        if bytes.starts_with(&[0, 0, 1, 0]) && bytes.len() > 6 {
            let count = u16::from_le_bytes([bytes[4], bytes[5]]) as usize;
            let png = (0..count)
                .filter_map(|i| bytes.get(6 + i * 16..22 + i * 16))
                .map(|entry| {
                    let size = u32::from_le_bytes(entry[8..12].try_into().unwrap()) as usize;
                    let offset = u32::from_le_bytes(entry[12..16].try_into().unwrap()) as usize;
                    &response.body[offset.min(response.body.len())..(offset + size).min(response.body.len())]
                })
                .filter(|data| data.starts_with(b"\x89PNG"))
                .max_by_key(|data| data.len());
            bytes = png.unwrap_or(&[]);
        }
        let png = zebkit::services::decode_image(bytes)
            .and_then(|image| zebkit::services::encode_image(zebkit::services::ImageFormat::Png, &image, 100));
        match png {
            Ok(png) => done(png),
            Err(_) => fetch_icon(links, done),
        }
    });
}

fn human_size(bytes: u64) -> String {
    match bytes {
        0..1024 => format!("{bytes} bytes"),
        1024..1_048_576 => format!("{:.0} KB", bytes as f64 / 1024.0),
        _ => format!("{:.1} MB", bytes as f64 / 1_048_576.0),
    }
}

/// `name` in `directory`, or "name (2).ext" and so on when it is taken.
fn free_path(directory: &std::path::Path, name: &str) -> PathBuf {
    let (stem, extension) = name.rsplit_once('.').map_or((name, String::new()), |(stem, ext)| (stem, format!(".{ext}")));
    (1..)
        .map(|n| directory.join(if n == 1 { name.to_string() } else { format!("{stem} ({n}){extension}") }))
        .find(|path| !path.exists())
        .unwrap()
}

fn downloads_dir() -> PathBuf {
    let home = std::env::var_os("HOME").or_else(|| std::env::var_os("USERPROFILE")).unwrap_or_default();
    PathBuf::from(home).join("Downloads")
}
