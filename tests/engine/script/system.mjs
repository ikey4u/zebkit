// The system's services and the newer page hooks in host JavaScript
// (docs/SYSTEM_INTEGRATION.md, 9 to 14): zebkit.displays(), the Dock and the
// taskbar, keeping awake, opening with the system's handler refused for what
// must not be, the tray refusing what is not a PNG, power events; and a page's
// EyeDropper.open(), navigator.share() and navigator.usb.requestDevice() answered
// by the view's functions. Argument: the base URL of a local HTTP server.
import {activated, finish, throws, verify as check, within} from './helpers.mjs';

const [base] = zebkit.args;

check(Array.isArray(zebkit.displays()), 'displays() is a list');
check(zebkit.app.setBadge('2') && zebkit.app.setProgress(0.25) && !zebkit.app.setProgress(5),
      'a badge and progress, progress past 1 refused');
check(zebkit.app.setBadge(null) && zebkit.app.setProgress(null), 'badge and progress cleared');
zebkit.app.requestAttention();
// The Dock's menu is macOS's: refused elsewhere, taken there; either way no
// crash, and not counted.
throws(() => zebkit.app.setDockMenu([{title: 'New', id: 'new'}], () => {}));
const token = zebkit.preventSleep('a test');
check(token && zebkit.allowSleep(token) && !zebkit.allowSleep(token), 'a sleep token, given back once');
check(throws(() => zebkit.openExternal('javascript:alert(1)')), 'a javascript: URL is not opened');
check(throws(() => new zebkit.Tray(new Uint8Array([1, 2, 3]), 'nothing')), 'a tray refuses what is not a PNG');
check(zebkit.onPowerEvent(() => {}) === true && zebkit.onPowerEvent(null) === true, 'power events: set and cleared');
check(zebkit.onDisplaysChanged(() => {}) === true && zebkit.onDisplaysChanged(null) === true,
      'display events: set and cleared');

const view = new zebkit.View('');
const loaded = new Promise(resolve => view.on('load', (event, url) => { if (event === 'finished' && url === base + '/') resolve(); }));
view.load(base + '/');
await within(loaded, 15000, base + '/');
view.focus();
// After the evaluation's activation reached the browser (device_chooser.py).
const asked = (script) => view.evaluate(activated(script));

let dropper;
view.onEyeDropper(request => { dropper = request.metadata; request.respond('#112233'); });
const colour = await asked("new EyeDropper().open().then(r => r.sRGBHex, e => e.name)");
check(colour === '#112233' && dropper && dropper.origin === base, "EyeDropper.open() answered by the view's function",
      {colour, dropper});

let shared;
view.onShare(request => { shared = request.metadata; request.respond(true); });
const share = await asked("navigator.share({title: 'T', url: location.href}).then(() => 'shared', e => e.name)");
check(share === 'shared' && shared && shared.title === 'T', "navigator.share() answered by the view's function",
      {share, shared});

let chooser, unlisted = null;
view.onDeviceChooser(request => {
  chooser = request.metadata;
  unlisted = request.respond('no-such-device');
  request.respond(null);
});
const usb = await asked("navigator.usb.requestDevice({filters: []}).then(() => 'device', e => e.name)");
check(usb === 'NotFoundError' && chooser && chooser.kind === 'usb' && Array.isArray(chooser.devices),
      "navigator.usb.requestDevice() answered with none by the view's function", {usb, chooser});
check(unlisted === false, 'a device not listed is not taken', unlisted);
view.close();
finish('JavaScript system services, Dock and taskbar, tray, eye dropper, share and device chooser');
