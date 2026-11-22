// Runtime settings through host JavaScript; run by tests/engine/runtime_settings.py,
// which looks for its last line, "ok javascript". Each must() is a check; the
// first that fails ends the run, the rest depending on it.
import {finish, sleep, until as poll, verify} from './helpers.mjs';

const { RUNTIME_BASE: base, RUNTIME_MARKS: marksFile } = JSON.parse(zebkit.args[0]);
// For runtime_settings.py, which listens to the local network: when each part began
// and ended. zebkit.fs writes whole files, so all of them each time.
const marks = [];
async function mark(what) {
  marks.push(`${what} ${Date.now()}`);
  if (marksFile) await zebkit.fs.writeFile(marksFile, new TextEncoder().encode(marks.join('\n') + '\n'));
}
const must = (ok, what) => { if (!verify(ok, what)) throw new Error(what); };
async function until(test, what, ms = 15000) {
  if (!await poll(test, ms, 5)) throw new Error(`timed out: ${what}`);
}
const opened = () => new Promise(resolve => {
  const view = new zebkit.View('');
  const url = base + '/top';
  view.on('load', (phase, actual) => { if (phase === 'finished' && actual === url) resolve(view); });
  view.load(url);
});
function refused(fn, what) {
  let thrown = false;
  try { fn(); } catch (error) { thrown = error instanceof TypeError; }
  must(thrown, `refused ${what}`);
}
let name = 0;
async function cookiesBecome(view, expected) {
  for (let i = 0; i < 60; i++) {
    if (await view.evaluate(`probe('c${++name}')`) === expected) return verify(true, `third-party cookies became ${expected}`);
    await sleep(50);
  }
  must(false, `third-party cookies became ${expected}`);
}

async function test() {
  const view = await opened(), other = await opened();
  const effective = zebkit.engineSettings.effective;
  must(effective.thirdPartyCookies === 'block' && effective.downloads === 'save' && effective.spareRenderer === false &&
       JSON.stringify(zebkit.engineSettings.explicit) === '{}', 'defaults');

  // A bad batch changes nothing.
  view.setRuntimeSettings({ dialogs: 'accept' });
  const [engineBefore, viewBefore] = [JSON.stringify(zebkit.engineSettings), JSON.stringify(view.runtimeSettings)];
  for (const bad of [[], { nothing: 1 }, { thirdPartyCookies: true }, { downloads: 'deny', newWindows: 'nope' }]) {
    refused(() => zebkit.setEngineSettings(bad), JSON.stringify(bad));
    refused(() => view.setRuntimeSettings(bad), JSON.stringify(bad));
  }
  refused(() => view.setRuntimeSettings({ spareRenderer: true }), "a view's spare");
  must(JSON.stringify(zebkit.engineSettings) === engineBefore && JSON.stringify(view.runtimeSettings) === viewBefore, 'changed');
  view.setRuntimeSettings({ dialogs: null });

  await cookiesBecome(view, 'none');
  zebkit.setEngineSettings({ thirdPartyCookies: 'allow' });
  await cookiesBecome(view, 'cookie');
  must(other.runtimeSettings.effective.thirdPartyCookies === 'allow', 'for every view');
  zebkit.setEngineSettings({ thirdPartyCookies: 'block' });
  await cookiesBecome(view, 'none');
  zebkit.setEngineSettings({ thirdPartyCookies: null });

  view.setRuntimeSettings({ dialogs: 'dismiss' });
  other.setRuntimeSettings({ dialogs: 'accept' });
  must(await view.evaluate("confirm('q')") === false, 'dismiss');
  must(await other.evaluate("confirm('q')") === true, 'accept');
  view.setRuntimeSettings({ dialogs: null });
  other.setRuntimeSettings({ dialogs: null });

  const told = [];
  view.onDownloadProgress(event => told.push(event));
  const outcome = async file => {
    const done = () => told.find(item => item.url.endsWith(`name=${file}`) && item.done);
    await until(done, file);
    return done().state;
  };
  view.setRuntimeSettings({ downloads: 'deny' });
  await view.evaluate("save('denied.bin'); 1");
  must(await outcome('denied.bin') === 'cancelled', 'deny');
  view.setRuntimeSettings({ downloads: null });
  await view.evaluate("save('saved.bin'); 1");
  must(await outcome('saved.bin') === 'complete', 'save');

  view.setRuntimeSettings({ newWindows: 'block' });
  other.setRuntimeSettings({ newWindows: 'allow' });
  must(await view.evaluate('opens()') === false, 'block');
  must(await other.evaluate('opens()') === true, 'allow');
  view.setRuntimeSettings({ newWindows: null });
  zebkit.setEngineSettings({ newWindows: 'block' });
  must(await view.evaluate('opens()') === false, 'engine block');
  zebkit.setEngineSettings({ newWindows: null });
  must(await view.evaluate('opens()') === true, 'gone');

  // WebRTC's IP handling policy.
  const policy = 'webrtcIpHandlingPolicy';
  // Unset: Chromium's default. The quiet part first: a page that has gathered under
  // "default" goes on announcing its names for seconds after, which no setting takes back.
  must(zebkit.engineSettings.effective[policy] === 'default', 'unset: default');
  await mark('quiet-begin');
  zebkit.setEngineSettings({ [policy]: 'disable_non_proxied_udp' });
  must(await view.evaluate('rtc()') === 'none' && await other.evaluate('rtc()') === 'none', "the engine's: no local address");
  await sleep(3000);
  await mark('quiet-end');
  must(zebkit.engineSettings.explicit[policy] === 'disable_non_proxied_udp', 'engine explicit');
  must(other.runtimeSettings.effective[policy] === 'disable_non_proxied_udp' && !(policy in other.runtimeSettings.explicit),
       "the other view: the engine's");
  await mark('loud-begin');
  view.setRuntimeSettings({ [policy]: 'default' });
  must(await view.evaluate('rtc()') === 'host', "a view's own goes first");
  await sleep(1500);
  await mark('loud-end');
  must(await other.evaluate('rtc()') === 'none', 'the other view keeps none');
  must(view.runtimeSettings.explicit[policy] === 'default' && view.runtimeSettings.effective[policy] === 'default', 'view getters');
  await new Promise(resolve => {
    const url = base + '/top?again';
    view.on('load', (phase, actual) => { if (phase === 'finished' && actual === url) resolve(); });
    view.load(url);
  });
  must(await view.evaluate('rtc()') === 'host', 'a navigation keeps it');
  const [engineAt, viewAt] = [JSON.stringify(zebkit.engineSettings), JSON.stringify(view.runtimeSettings)];
  for (const bad of [{ [policy]: 'public' }, { [policy]: 5 }, { dialogs: 'accept', [policy]: 'nope' }]) {
    refused(() => zebkit.setEngineSettings(bad), JSON.stringify(bad));
    refused(() => view.setRuntimeSettings(bad), JSON.stringify(bad));
  }
  must(JSON.stringify(zebkit.engineSettings) === engineAt && JSON.stringify(view.runtimeSettings) === viewAt, 'a bad batch changed nothing');
  view.setRuntimeSettings({ [policy]: null });
  must(await view.evaluate('rtc()') === 'none' && !(policy in view.runtimeSettings.explicit), "null: the engine's");
  zebkit.setEngineSettings({ [policy]: null });
  must(await view.evaluate('rtc()') === 'host' && await other.evaluate('rtc()') === 'host', 'null: the default');
  must(!(policy in zebkit.engineSettings.explicit), 'engine null');

  zebkit.setEngineSettings({ spareRenderer: true });
  must(zebkit.engineSettings.effective.spareRendererRunning === true, 'spare');
  zebkit.setEngineSettings({ spareRenderer: false });
  must(zebkit.engineSettings.effective.spareRendererRunning === false, 'no spare');
  zebkit.setEngineSettings({ spareRenderer: null });
  view.close();
  other.close();
}
try {
  await test();
} catch (error) {
  verify(false, 'the run', String(error && error.stack || error));
}
try {
  finish('javascript');
} finally {
  zebkit.quit();
}
