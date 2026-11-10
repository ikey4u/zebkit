// zebkit-js's <zebkit/page.h> and <zebkit/offscreen.h>: dialogs held and
// answered, printing to PDF, closing, drag and drop from the application;
// off-screen views with their frames, pixels and input. Argument: the local
// server's base URL.
const [base] = zebkit.args;
let failures = 0;
const check = (ok, what, detail) => {
  console.log((ok ? 'ok   ' : 'FAIL ') + what + (ok || detail === undefined ? '' : ': ' + JSON.stringify(detail)));
  if (!ok) failures++;
};
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const within = (promise, ms, what) => Promise.race([promise, sleep(ms).then(() => { throw new Error('timed out: ' + what); })]);
const loaded = (view) => new Promise((resolve) => view.on('load', (event) => { if (event === 'finished') resolve(); }));
const until = async (condition, ms = 10000) => {
  for (const end = Date.now() + ms; Date.now() < end; await sleep(20)) if (await condition()) return true;
  return false;
};

// ---- page.h ----
const view = new zebkit.View('', {width: 400, height: 420});
let done = loaded(view);
view.load(base + '/page.html');
await within(done, 15000, 'page.html');

// Dialogs: held for the application, answered with text.
check(view.setDialogPolicy('hold') === 'hold' && view.dialogPolicy === 'hold', 'setDialogPolicy and dialogPolicy');
const opened = new Promise((resolve) => view.on('dialog', resolve));
const closed = new Promise((resolve) => view.on('dialog-closed', (id, accepted, input) => resolve({id, accepted, input})));
await view.evaluate("setTimeout(() => window.answer = prompt('Your name?', 'nobody'), 0); 1");
const dialog = await within(opened, 10000, 'dialog').catch(() => null);
check(dialog?.type === 'prompt' && dialog.message === 'Your name?' && dialog.defaultPrompt === 'nobody' && dialog.held,
      "a dialog, held for the application", dialog);
check(view.dialogs.length === 1, 'dialogs', view.dialogs);
check(dialog && view.respondDialog(dialog.id, true, 'Ada'), 'respondDialog');
const after = await within(closed, 10000, 'dialog-closed').catch(() => null);
check(after?.accepted === true && after.input === 'Ada', "'dialog-closed'", after);
check(await until(async () => await view.evaluate('window.answer') === 'Ada'), 'the page gets the answer');
view.setDialogPolicy('default');

// Printing to PDF.
const pdf = await view.printToPdf({landscape: false});
check(pdf instanceof Uint8Array && new TextDecoder().decode(pdf.subarray(0, 5)) === '%PDF-', 'printToPdf');

// Drag and drop from the application: onto the page's #drop (0, 220, 400 by 80).
check(view.isDragging === false, 'isDragging');
view.dragEnter({text: 'plain words', custom: {'application/x-mine': 'mine'}}, 50, 250, zebkit.DRAG.copy, 0);
const over = await view.dragOver(60, 255, zebkit.DRAG.copy, 0);
const dropped = await view.drop(60, 260, 0);
check(over === zebkit.DRAG.copy && dropped === zebkit.DRAG.copy, 'dragEnter, dragOver and drop', {over, dropped});
check(await until(async () => (await view.evaluate('JSON.stringify(window.dropped)'))?.includes('plain words')),
      'the page gets what was dropped');
view.dragEnter({text: 'nowhere'}, 390, 395, zebkit.DRAG.copy, 0);
view.dragLeave();
view.dragSourceEnd(0, 0, zebkit.DRAG.none);

// Closing as the person would: beforeunload's dialog answered by the policy.
const closing = new Promise((resolve) => view.on('close', resolve));
view.setDialogPolicy('accept');
view.requestClose();
check(await within(closing, 10000, 'close').then(() => true, () => false), 'requestClose closes the view');

// ---- offscreen.h ----
const off = zebkit.View.offscreen('', {width: 400, height: 300, scale: 1});
check(off !== null && off.isOffscreen, 'View.offscreen');
off.setFrameBuffers('shared-memory');
off.setMaxFramesInFlight(2);
off.setFrameRate(60);
const frames = [];
off.on('frame', (frame) => {
  if (frame.layer !== 'view') return;
  const pixels = frame.pixels();
  frames.push({width: frame.width, height: frame.height, kind: frame.kind, damage: frame.damage.length,
               stride: frame.stride, pixel: pixels ? [...pixels.subarray(30 * frame.stride + 30 * 4, 30 * frame.stride + 30 * 4 + 4)] : null});
});
let cursor = null;
off.on('cursor', (name) => { cursor = name; });
let caret = null;
off.on('caret', (rect) => { caret = rect; });
done = loaded(off);
off.load(base + '/offscreen.html');
await within(done, 15000, 'offscreen.html');
off.requestFrame();
check(await until(() => frames.some((f) => f.pixel)), 'frames with their pixels');
// A frame of the page once it has painted: the first can be the blank page's.
const red = (f) => f.pixel?.[2] === 255 && f.pixel?.[0] === 0;
await until(() => frames.some(red));
const frame = frames.find(red) ?? frames.find((f) => f.pixel) ?? {};
// The red box at (20, 20, 40 by 30): B, G, R, A at (30, 30).
check(frame.width === 400 && frame.height === 300 && frame.kind === 'shared-memory' && frame.damage > 0 &&
      frame.pixel?.[2] === 255 && frame.pixel?.[0] === 0, 'a frame of the page, its size and a pixel', frame);
off.setFocus(true);
off.sendMouse('move', 150, 40);
off.sendMouse('down', 150, 40, {button: 'left', clickCount: 1});
off.sendMouse('up', 150, 40, {button: 'left', clickCount: 1});
check(await until(async () => await off.evaluate('document.body.dataset.clicks') === '1'), 'sendMouse clicks the button');
check(await until(() => cursor === 'pointer', 3000), "'cursor' over the button", cursor);
off.sendMouse('down', 50, 115, {button: 'left', clickCount: 1});
off.sendMouse('up', 50, 115, {button: 'left', clickCount: 1});
off.sendKey({type: 'down', keyCode: 0x41, code: 'KeyA', key: 'a'});
off.sendKey({type: 'char', text: 'a', unmodifiedText: 'a'});
off.sendKey({type: 'up', keyCode: 0x41, code: 'KeyA', key: 'a'});
off.imeSetComposition('bc', 2, 2);
off.imeCommit('bc');
check(await until(async () => await off.evaluate("document.getElementById('field').value") === 'abc'),
      'sendKey and the input method type into the field', await off.evaluate("document.getElementById('field').value"));
check(await until(() => caret !== null, 3000), "'caret'", caret);
check(off.sendKeyWithCommands({type: 'down', keyCode: 0x41, code: 'KeyA', key: 'a', modifiers: zebkit.MODIFIERS.control},
                              [{name: 'selectAll'}]) === true, 'sendKeyWithCommands');
off.sendWheel(200, 150, 0, 500, {precise: true});
check(await until(async () => await off.evaluate('scrollY') > 0), 'sendWheel scrolls');
off.setSize(200, 100, 2);
check(await until(() => frames.some((f) => f.width === 400 && f.height === 200)), 'setSize at scale 2');
off.imeCancel();
off.close();

if (failures) throw new Error(`${failures} checks failed`);
console.log('ok JavaScript page dialogs, PDF, drag and drop, off-screen frames and input');
zebkit.quit();
