const [url] = zebkit.args;
const view = new zebkit.View('');
const valid = {userAgent:'zebkit-binding-UA/1',acceptLanguage:'en-US',platform:'NativePlatform',
  userAgentMetadata:{brands:[{brand:'Native',version:'1'}],fullVersionList:[{brand:'Native',version:'1.0'}],
    platform:'Linux',platformVersion:'1',architecture:'x86',model:'',mobile:false}};
const loaded = new Promise(resolve => view.on('load',(phase,source) => {
  if (phase === 'finished' && source === url) resolve();
}));
const update = view.setUserAgentOverride(valid);
view.load(url);
await update;
await loaded;
const state = () => view.evaluate(`navigator.userAgentData.getHighEntropyValues(['platformVersion','architecture']).then(hints=>({ua:navigator.userAgent,platform:navigator.platform,hints}))`);
const before = await state();
if (before.ua !== valid.userAgent || before.platform !== valid.platform ||
    before.hints.architecture !== 'x86' || before.hints.platformVersion !== '1') throw new Error('native UA metadata mismatch');
for (const options of [{...valid,userAgent:'bad\r\nagent'}, {...valid,userAgent:'',userAgentMetadata:valid.userAgentMetadata},
    {userAgent:42},{userAgent:'changed',unknown:true},{userAgent:'x'.repeat(65537)}]) {
  let rejected = false;
  try { await view.setUserAgentOverride(options); } catch { rejected = true; }
  if (!rejected || JSON.stringify(await state()) !== JSON.stringify(before)) throw new Error('rejected UA update changed state');
}
const closed = new Promise(resolve=>view.on('close',resolve));
view.close();
await closed;
console.log('ok JavaScript UA/client hints and atomic rejection');
