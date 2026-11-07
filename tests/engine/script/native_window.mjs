const [expected = 'none'] = zebkit.args;
const view = new zebkit.View('about:blank');
const info = view.nativeWindow;
const exported = await view.exportNativeWindow();
if (expected === 'none') {
  if (info !== null) throw new Error('a headless view exposed a system window');
  if (exported !== null) throw new Error('a headless view exported a window: ' + exported);
} else if (expected === 'wayland') {
  if (info !== null) throw new Error('a Wayland view exposed an X11/HWND/NSWindow handle');
  if (typeof exported !== 'string' || !/^wayland:./.test(exported))
    throw new Error('no wayland:<handle> export: ' + exported);
} else {
  if (!info || info.kind !== expected || typeof info.handle !== 'bigint' || info.handle <= 0n)
    throw new Error('missing platform type or lossless system handle');
  if (exported !== 'x11:0x' + info.handle.toString(16))
    throw new Error('the export is not the same window: ' + exported);
}
const closed = new Promise(resolve => view.on('close', resolve));
view.close();
await closed;
console.log('ok JavaScript native window platform type, BigInt handle and portal export: ' + expected);
