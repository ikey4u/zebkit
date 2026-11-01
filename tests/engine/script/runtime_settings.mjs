// Runtime settings through host JavaScript; run by tests/engine/runtime_settings.py.
const { RUNTIME_BASE: base } = JSON.parse(zebkit.args[0]);
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const must = (ok, what) => { if (!ok) throw new Error(what); };
async function until(test, what, ms = 15000) {
  for (const end = Date.now() + ms; Date.now() < end; await sleep(5)) if (await test()) return;
  throw new Error(`timed out: ${what}`);
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
  must(thrown, `took ${what}`);
}
let name = 0;
async function cookiesBecome(view, expected) {
  for (let i = 0; i < 60; i++) {
    if (await view.evaluate(`probe('c${++name}')`) === expected) return;
    await sleep(50);
  }
  throw new Error(`third-party cookies never became ${expected}`);
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

  zebkit.setEngineSettings({ spareRenderer: true });
  must(zebkit.engineSettings.effective.spareRendererRunning === true, 'spare');
  zebkit.setEngineSettings({ spareRenderer: false });
  must(zebkit.engineSettings.effective.spareRendererRunning === false, 'no spare');
  zebkit.setEngineSettings({ spareRenderer: null });
  view.close();
  other.close();
}
test().then(() => { console.log('ok javascript'); zebkit.quit(); },
            error => { console.log(`FAIL ${error && error.stack || error}`); zebkit.quit(); });
