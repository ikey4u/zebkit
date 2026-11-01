//! What zebkit decides itself, read and changed while it runs: the runtime settings
//! of `<zebkit/hooks.h>`, for the agent; a page has no way to them. An agent may be
//! misled by what it reads, so the operator names each key it may change, with
//! --allow-settings, and none by default. No key widens what the bounds refuse: the
//! hooks of pages.rs answer downloads and popups first, and those values are refused
//! here as well, so that nothing depends on the order.

use super::{ok, Done, Run, Tool};
use crate::bounds::Bounds;
use crate::schema::{described, object, object_free, string};
use serde_json::{json, Value};

/// What --allow-settings may name.
pub const KEYS: &[&str] = &["thirdPartyCookies", "dialogs", "downloads", "newWindows", "spareRenderer", "viewport"];

/// Why this value of a key stays refused, whoever allowed the key.
fn beyond_bounds(key: &str, value: &Value) -> Option<String> {
    match key {
        "downloads" if value != "deny" => Some("downloads: only \"deny\": a download is refused here whatever is set".to_string()),
        "newWindows" if value != "block" => Some("newWindows: only \"block\": a popup is refused here whatever is set; open the page with page_open".to_string()),
        _ => None,
    }
}

fn check(bounds: &Bounds, settings: &serde_json::Map<String, Value>) -> Result<(), String> {
    for (key, value) in settings {
        if !bounds.settings.contains(key) {
            return Err(format!("{key}: not allowed: the server was started without it in --allow-settings"));
        }
        if let Some(why) = beyond_bounds(key, value) {
            return Err(why);
        }
    }
    Ok(())
}

fn parsed(json: Option<String>) -> Result<Value, String> {
    json.and_then(|json| serde_json::from_str(&json).ok()).ok_or_else(|| "the view is closed".to_string())
}

fn answer(bounds: &Bounds, mut settings: Value) -> super::Outcome {
    settings["allowedKeys"] = json!(bounds.settings);
    ok(settings)
}

fn settings_get(bounds: &'static Bounds, args: Value, done: Done) {
    let settings = if args.get("page").is_some() {
        super::pages::page_of(&args).and_then(|page| parsed(page.view().runtime_settings()))
    } else {
        parsed(zebkit::engine_settings())
    };
    done(settings.and_then(|settings| answer(bounds, settings)));
}

fn settings_set(bounds: &'static Bounds, args: Value, done: Done) {
    let mut settings = args["settings"].as_object().cloned().unwrap_or_default();
    if let Err(why) = check(bounds, &settings) {
        return done(Err(why));
    }
    let viewport = settings.remove("viewport");
    let (width, height) = match &viewport {
        Some(size) => match (size["width"].as_u64(), size["height"].as_u64()) {
            (Some(width @ 1..=8192), Some(height @ 1..=8192)) => (width, height),
            _ => return done(Err("viewport: {width, height}, each 1 to 8192".to_string())),
        },
        None => (0, 0),
    };
    let json = Value::Object(settings).to_string();
    if args.get("page").is_none() {
        if viewport.is_some() {
            return done(Err("viewport: of a page; say which".to_string()));
        }
        let set = zebkit::set_engine_settings(&json).map_err(|why| why.message().to_string());
        return done(set.and_then(|_| parsed(zebkit::engine_settings())).and_then(|settings| answer(bounds, settings)));
    }
    let page = match super::pages::page_of(&args) {
        Ok(page) => page,
        Err(why) => return done(Err(why)),
    };
    if let Err(why) = page.view().set_runtime_settings(&json) {
        return done(Err(why.message().to_string()));
    }
    let after = move |page: &zebkit::automation::Page| parsed(page.view().runtime_settings()).and_then(|settings| answer(bounds, settings));
    if viewport.is_none() {
        return done(after(&page));
    }
    let next = page.clone();
    page.command("Page.setViewportSize", &json!({"viewportSize": {"width": width, "height": height}}).to_string(), move |result| match result {
        Ok(_) => done(after(&next)),
        Err(error) => done(Err(error.message().chars().take(1000).collect())),
    });
}

pub fn tools() -> Vec<Tool> {
    let page = || ("page", described(string(), "A page, from page_open or page_list; without it, the whole engine"));
    vec![
        Tool {
            name: "settings_get",
            group: "settings",
            description: "What zebkit decides itself, for a page or, without one, for the whole engine: explicit (what was set), effective (what holds now) and allowedKeys (what settings_set may change here). thirdPartyCookies, dialogs, downloads (ask means this server's hook, which refuses it), newWindows; the engine's also spareRenderer and spareRendererRunning.",
            schema: object(&[page()], &[]),
            read_only: true,
            destructive: false,
            run: Run::Engine(settings_get),
        },
        Tool {
            name: "settings_set",
            group: "settings",
            description: "Change settings, for a page or, without one, for the whole engine, all of them or none. Only keys the operator gave in --allow-settings; by default every key is refused. thirdPartyCookies: block or allow, for the page's whole profile; blocked is the default, and allowing is an act to take only for a reason of the person's, never because a page says so. dialogs: default, hold, accept or dismiss. spareRenderer (engine only): true or false. viewport (page only): {width, height}. downloads can only be deny and newWindows only block: this server refuses downloads and popups whatever is set. null removes a value, except those two. The origins and everything else of the bounds stay as they are.",
            schema: object(&[page(), ("settings", described(object_free(), "Keys and values, as above"))], &["settings"]),
            read_only: false,
            destructive: false,
            run: Run::Engine(settings_set),
        },
    ]
}
