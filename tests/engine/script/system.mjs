// The system's services and the newer page hooks in host JavaScript
// (docs/SYSTEM_INTEGRATION.md, 9 to 14): zebkit.displays(), the Dock and the
// taskbar, keeping awake, opening with the system's handler refused for what
// must not be, the tray refusing what is not a PNG, power events; and a page's
// EyeDropper.open(), navigator.share() and navigator.usb.requestDevice() answered
// by the view's functions. Argument: the base URL of a local HTTP server.
const [base] = zebkit.args;
const fail = (what) => { throw new Error(what); };
const throws = (action) => { try { action(); } catch (error) { return true; } return false; };

const displays = zebkit.displays();
if (!Array.isArray(displays)) fail('displays() is not a list');
if (!zebkit.app.setBadge('2') || !zebkit.app.setProgress(0.25) || zebkit.app.setProgress(5)) fail('badge or progress');
if (!zebkit.app.setBadge(null) || !zebkit.app.setProgress(null)) fail('badge or progress not cleared');
zebkit.app.requestAttention();
// The Dock's menu is macOS's: refused elsewhere, taken there; either way no crash.
throws(() => zebkit.app.setDockMenu([{title: 'New', id: 'new'}], () => {}));
const token = zebkit.preventSleep('a test');
if (!token || !zebkit.allowSleep(token) || zebkit.allowSleep(token)) fail('sleep tokens');
if (!throws(() => zebkit.openExternal('javascript:alert(1)'))) fail('a javascript: URL was opened');
if (!throws(() => new zebkit.Tray(new Uint8Array([1, 2, 3]), 'nothing'))) fail('a tray took what is not a PNG');
if (zebkit.onPowerEvent(() => {}) !== true || zebkit.onPowerEvent(null) !== true) fail('power events');
if (zebkit.onDisplaysChanged(() => {}) !== true || zebkit.onDisplaysChanged(null) !== true) fail('display events');

const view = new zebkit.View('');
const loaded = new Promise(resolve => view.on('load', (event, url) => { if (event === 'finished' && url === base + '/') resolve(); }));
view.load(base + '/');
await loaded;
view.focus();
await new Promise(resolve => setTimeout(resolve, 300));
// After the evaluation's activation reached the browser (device_chooser.py).
const asked = (script) => view.evaluate(`new Promise(r => setTimeout(r, 200)).then(() => ${script})`);

let dropper;
view.onEyeDropper(request => { dropper = request.metadata; request.respond('#112233'); });
const colour = await asked("new EyeDropper().open().then(r => r.sRGBHex, e => e.name)");
if (colour !== '#112233' || !dropper || dropper.origin !== base) fail(`eye dropper: ${colour}`);

let shared;
view.onShare(request => { shared = request.metadata; request.respond(true); });
const share = await asked("navigator.share({title: 'T', url: location.href}).then(() => 'shared', e => e.name)");
if (share !== 'shared' || !shared || shared.title !== 'T') fail(`share: ${share}`);

let chooser;
view.onDeviceChooser(request => {
  chooser = request.metadata;
  if (request.respond('no-such-device')) fail('a device not listed was taken');
  request.respond(null);
});
const usb = await asked("navigator.usb.requestDevice({filters: []}).then(() => 'device', e => e.name)");
if (usb !== 'NotFoundError' || !chooser || chooser.kind !== 'usb' || !Array.isArray(chooser.devices))
  fail(`device chooser: ${usb} ${JSON.stringify(chooser)}`);
view.close();
console.log('ok JavaScript system services, Dock and taskbar, tray, eye dropper, share and device chooser');
