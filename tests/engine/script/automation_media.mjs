// zebkit-js's <zebkit/automation.h> and the media headers: Playwright's API
// inside the engine, its events, pixels and endpoint; what a page plays,
// listed, captured and tapped; FFmpeg's tools. Arguments: the local server's
// base URL, a media clip, a directory to write to.
import {finish, loaded, until as poll, verify as check, within} from './helpers.mjs';

const until = (condition, ms = 15000) => poll(condition, ms);

const [base, clip, scratch] = zebkit.args;
const auto = zebkit.automation;

// ---- automation.h ----
const commands = auto.commands();
check(Array.isArray(commands) ? commands.length > 50 : Object.keys(commands ?? {}).length > 50, 'automation.commands');
const view = new zebkit.View('', {width: 600, height: 500});
const events = [];
view.on('automation', (event) => events.push(event));
await auto.call(view, 'Frame.goto', {url: base + '/automation.html'});
check(view.title === 'Automation', 'Frame.goto', view.title);
await auto.call(view, 'Frame.fill', {selector: '#name', strict: true, value: 'Ada'});
await auto.call(view, 'Frame.click', {selector: '#go', strict: true});
const text = await auto.call(view, 'Frame.textContent', {selector: '#out', strict: true});
check(JSON.stringify(text).includes('clicked 1'), 'Frame.fill, Frame.click and Frame.textContent', text);
const value = await auto.call(view, 'Frame.inputValue', {selector: '#name', strict: true});
check(JSON.stringify(value).includes('Ada'), 'Frame.inputValue', value);
await auto.call(view, 'Frame.evaluateExpression', {expression: "() => console.log('from the page')", isFunction: true});
check(await until(() => events.includes('BrowserContext.console'), 5000), "the page's console as an 'automation' event", events);
const shot = await auto.call(view, 'Page.screenshot', {type: 'png'});
check(typeof shot?.binary === 'string' && atob(shot.binary).startsWith('\x89PNG'), 'Page.screenshot');
const pixels = await auto.screenshotPixels(view);
check(pixels.width > 0 && pixels.data.length === pixels.height * pixels.stride, 'screenshotPixels', {width: pixels.width, height: pixels.height});
const id = auto.pageId(view);
check(typeof id === 'string' && id.length > 0 && auto.page(id) === view, 'pageId and page');
check(await auto.call(view, 'Frame.nonsense', {}).then(() => false, () => true), 'an unknown command is refused');
const endpoint = await auto.listen(0);
check(typeof endpoint === 'string' && endpoint.startsWith('ws://127.0.0.1:'), 'automation.listen', endpoint);
view.close();

// ---- media ----
zebkit.media.keepHistory(true);
const player = new zebkit.View('', {width: 400, height: 300});
let done = loaded(player);
player.load(base + '/media.html');
await within(done, 15000, 'media.html');
await player.evaluate("file('video', 'media/clip.mp4')");
await until(async () => await player.evaluate('ready()'));
await player.evaluate('play()');
let media = [];
check(await until(async () => {
  media = await player.mediaList();
  return media.length === 1 && media[0].playing;
}), 'mediaList', media);
const at = await player.mediaAt(20, 20);
check(at && at.id === media[0]?.id, 'mediaAt', at);
if (media.length === 1) {
  const out = scratch + '/js-captured.mp4';
  let progressed = false, finished = null;
  const capture = player.mediaCapture(media[0].id, out, {}, {
    onProgress: () => { progressed = true; },
    onDone: (state) => { finished = state; },
  });
  await until(() => progressed, 10000);
  // Until what it wrote is media, not only the file's header: under load the
  // clip can play late.
  await until(() => (capture.status?.bytes ?? 0) > 1000, 20000);
  check(JSON.stringify(capture.status).includes('"state"'), 'mediaCapture status', capture.status);
  capture.stop();
  check(await until(() => finished !== null, 20000) && progressed, 'mediaCapture: progress, stop, done', finished);
  const written = await zebkit.fs.stat(out).catch(() => null);
  check(written && written.size > 1000, 'the captured file', written);
  capture.free();
  const kinds = {};
  const tap = player.mediaTap(media[0].id, {packets: true, video: true, audio: false, includeBuffered: true},
                              (event) => { kinds[event.type] = (kinds[event.type] ?? 0) + 1; });
  check(await until(() => kinds.packet > 0 && kinds.video > 0), "the tap's packets and video", kinds);
  tap.stop();
  check(await until(() => kinds.done > 0), 'the tap stops, then done', kinds);
  tap.free();
}
player.close();

// FFmpeg's tools, on a worker thread.
const probe = await zebkit.media.probe(clip);
check(JSON.stringify(probe).includes('streams'), 'media.probe');
const streams = await zebkit.media.ffprobe(['-show_streams', '-of', 'json', clip]);
check(JSON.stringify(streams).includes('codec_type'), 'media.ffprobe');
let calls = 0;
const remux = await zebkit.media.ffmpeg(['-y', '-i', clip, '-c', 'copy', '--', scratch + '/js-remuxed.mkv'], () => { calls++; return true; });
const remuxed = await zebkit.fs.stat(scratch + '/js-remuxed.mkv').catch(() => null);
check(remux === 'completed' && calls > 0 && remuxed?.size > 1000, 'media.ffmpeg with progress', {remux, calls});
const cancelled = await zebkit.media.ffmpeg(['-y', '-i', clip, '-c', 'copy', '--', scratch + '/js-cancelled.mkv'], () => false);
check(cancelled === 'cancelled', 'a progress function that returns false cancels', cancelled);
check(await zebkit.media.ffmpeg(['-i', clip, '-c', 'copy']).then(() => false, () => true), 'ffmpeg refuses what it cannot do');

finish('JavaScript automation, media capture, taps and FFmpeg');
zebkit.quit();
