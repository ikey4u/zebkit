const [expected = 'none'] = zebkit.args;
const view = new zebkit.View('about:blank');
const info = view.nativeWindow;
if (expected === 'none') {
  if (info !== null) throw new Error('a headless view exposed a system window');
} else {
  if (!info || info.kind !== expected || typeof info.handle !== 'bigint' || info.handle <= 0n)
    throw new Error('missing platform type or lossless system handle');
}
const closed = new Promise(resolve => view.on('close', resolve));
view.close();
await closed;
console.log('ok JavaScript native window platform type and BigInt handle: '+expected);
