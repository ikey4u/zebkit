const [base] = zebkit.args;
const parent = new zebkit.View('');
const load = (view, url) => new Promise(resolve => {
  view.on('load', (phase, actual) => { if (phase === 'finished' && actual === url) resolve(); });
  view.load(url);
});
await load(parent, base + '/permissions.html');
if (JSON.stringify(parent.windowOpenInfo) !== '{}') throw new Error('ordinary view has popup metadata');
parent.setPopupPolicy('allow');
const decisions = [];
const created = new Promise(resolve => parent.on('window-open', event => {
  if (event.phase === 'decision') decisions.push(event);
  if (event.phase === 'created') resolve(event);
}));
if (!await parent.evaluate("window.child=window.open('/settings.html?adopted','child','left=0,top=0,width=320,height=240'); !!child"))
  throw new Error('popup was blocked');
const event = await created;
const child = event.view;
if (!(child instanceof zebkit.View)) throw new Error('created event has no adopted View');
if (!decisions.some(value => value.allowed && value.url === base + '/settings.html?adopted'))
  throw new Error('missing allowed decision');
const snapshot = child.windowOpenInfo;
if (snapshot.targetId !== event.targetId || snapshot.features.bounds.x !== 0 || !snapshot.features.hasX ||
    snapshot.frameName !== 'child' || !snapshot.hasOpener) throw new Error('creation metadata mismatch');
await load(child, base + '/permissions.html?child-moved');
if (JSON.stringify(child.windowOpenInfo) !== JSON.stringify(snapshot)) throw new Error('creation snapshot changed');
if (!await child.evaluate("window.opener.fromChild='delivered'; window.opener !== null"))
  throw new Error('popup lost opener');
if (await parent.evaluate('window.fromChild') !== 'delivered') throw new Error('opener messaging failed');
const consoleMessage = new Promise(resolve => child.on('console', resolve));
await child.evaluate("console.warn('child console'); true");
const consoleEvent = await consoleMessage;
if (consoleEvent.message !== 'child console' || consoleEvent.level !== 'warning' || !consoleEvent.mainFrame || consoleEvent.origin !== base)
  throw new Error('adopted child console metadata mismatch');
parent.close();
if (await child.evaluate('1+2') !== 3) throw new Error('child lifetime depends on its parent');
const closed = new Promise(resolve => child.on('close', resolve));
child.close();
await closed;
console.log('ok JavaScript original popup adoption, window metadata, opener messaging and independent lifecycle');
