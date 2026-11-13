// zebkit-js: a page's window.open() adopted as a View of the application's: the
// decision and creation events, the window's metadata, its opener, its console,
// and a life of its own once the opener is closed. Argument: the local server's
// base URL (tests/engine/pages).
import {finish, verify as check, within} from './helpers.mjs';

const [base] = zebkit.args;
const parent = new zebkit.View('');
const load = (view, url) => within(new Promise(resolve => {
  view.on('load', (phase, actual) => { if (phase === 'finished' && actual === url) resolve(); });
  view.load(url);
}), 15000, url);
await load(parent, base + '/permissions.html');
check(JSON.stringify(parent.windowOpenInfo) === '{}', 'an ordinary view has no popup metadata', parent.windowOpenInfo);
parent.setPopupPolicy('allow');
const decisions = [];
const created = new Promise(resolve => parent.on('window-open', event => {
  if (event.phase === 'decision') decisions.push(event);
  if (event.phase === 'created') resolve(event);
}));
check(await parent.evaluate("window.child=window.open('/settings.html?adopted','child','left=0,top=0,width=320,height=240'); !!child"),
      'the popup is not blocked');
const event = await within(created, 15000, "window-open's created event");
const child = event.view;
check(child instanceof zebkit.View, 'the created event has the adopted View');
check(decisions.some(value => value.allowed && value.url === base + '/settings.html?adopted'),
      'an allowed decision came first', decisions);
const snapshot = child.windowOpenInfo;
check(snapshot.targetId === event.targetId && snapshot.features.bounds.x === 0 && snapshot.features.hasX &&
      snapshot.frameName === 'child' && snapshot.hasOpener, "the popup's metadata: target, features, name, opener", snapshot);
await load(child, base + '/permissions.html?child-moved');
check(JSON.stringify(child.windowOpenInfo) === JSON.stringify(snapshot), 'the metadata is of its creation, kept as it navigates');
check(await child.evaluate("window.opener.fromChild='delivered'; window.opener !== null"), 'the popup keeps its opener');
check(await parent.evaluate('window.fromChild') === 'delivered', 'and reaches the opener through it');
const consoleMessage = new Promise(resolve => child.on('console', resolve));
await child.evaluate("console.warn('child console'); true");
const consoleEvent = await within(consoleMessage, 10000, "the child's console");
check(consoleEvent.message === 'child console' && consoleEvent.level === 'warning' && consoleEvent.mainFrame &&
      consoleEvent.origin === base, "the adopted child's console", consoleEvent);
parent.close();
check(await child.evaluate('1+2') === 3, 'the child lives on without its opener');
const closed = new Promise(resolve => child.on('close', resolve));
child.close();
await within(closed, 10000, "the child's close");
finish('JavaScript original popup adoption, window metadata, opener messaging and independent lifecycle');
