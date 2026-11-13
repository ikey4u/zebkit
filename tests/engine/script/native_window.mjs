// zebkit-js: a view's system window (nativeWindow) and its export for portals
// (exportNativeWindow). Argument: none (headless), x11 or wayland.
import {finish, verify as check, within} from './helpers.mjs';

const [expected = 'none'] = zebkit.args;
const view = new zebkit.View('about:blank');
const info = view.nativeWindow;
const exported = await view.exportNativeWindow();
if (expected === 'none') {
  check(info === null, 'a headless view exposes no system window', info);
  check(exported === null, 'and exports none', exported);
} else if (expected === 'wayland') {
  check(info === null, 'a Wayland view exposes no X11, HWND or NSWindow handle', info);
  check(typeof exported === 'string' && /^wayland:./.test(exported), 'it exports wayland:<handle>', exported);
} else {
  check(info && info.kind === expected && typeof info.handle === 'bigint' && info.handle > 0n,
        'the platform type and the handle, a BigInt', info && {kind: info.kind, handle: String(info.handle)});
  check(info && exported === 'x11:0x' + info.handle.toString(16), 'the export is the same window', exported);
}
const closed = new Promise(resolve => view.on('close', resolve));
view.close();
await within(closed, 10000, "the view's close");
finish('JavaScript native window platform type, BigInt handle and portal export: ' + expected);
