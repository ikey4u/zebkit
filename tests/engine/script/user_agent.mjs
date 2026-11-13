// A view's user agent and client hints through zebkit-js's
// View.setUserAgentOverride, one mode an engine; run by tests/engine/user_agent.py.
import {finish, verify} from './helpers.mjs';

const [url, mode = 'hints'] = zebkit.args;
const view = new zebkit.View('');
const valid = {userAgent:'zebkit-binding-UA/1',acceptLanguage:'en-US',platform:'NativePlatform',
  userAgentMetadata:{brands:[{brand:'Native',version:'1'}],fullVersionList:[{brand:'Native',version:'1.0'}],
    platform:'Linux',platformVersion:'1',architecture:'x86',model:'',mobile:false}};
const loaded = new Promise(resolve => view.on('load',(phase,source) => {
  if (phase === 'finished' && source === url) resolve();
}));
// The load waits for the override.
const update = view.setUserAgentOverride(valid);
view.load(url);
verify(JSON.stringify(await update) === '{}', 'the override completed with {}');
await loaded;
const state = () => view.evaluate(`navigator.userAgentData.getHighEntropyValues(['platformVersion','architecture']).then(hints=>({ua:navigator.userAgent,platform:navigator.platform,first:window.firstUA.ua,hints}))`);
const before = await state();
verify(before.ua === valid.userAgent && before.first === valid.userAgent && before.platform === valid.platform &&
       before.hints.architecture === 'x86' && before.hints.platformVersion === '1',
  'the page has the override: its first script, agent, platform, hints', before);
if (mode === 'rejection') {
  for (const options of [{...valid,userAgent:'bad\r\nagent'}, {...valid,userAgent:'',userAgentMetadata:valid.userAgentMetadata},
      {userAgent:42},{userAgent:'changed',unknown:true},{userAgent:'x'.repeat(65537)}]) {
    let rejected = false;
    try { await view.setUserAgentOverride(options); } catch { rejected = true; }
    verify(rejected && JSON.stringify(await state()) === JSON.stringify(before),
      'refused, the state as it was: ' + JSON.stringify(options).slice(0, 60));
  }
}
const closed = new Promise(resolve=>view.on('close',resolve));
view.close();
await closed;
finish('JavaScript UA/client hints ' + mode);
