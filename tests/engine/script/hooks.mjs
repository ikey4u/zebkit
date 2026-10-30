const view = new zebkit.View('');
if (JSON.stringify(view.settings) !== '{}') throw new Error('unexpected initial settings');
if (!view.setSettings({colorScheme: 'dark', reducedMotion: true})) throw new Error('settings rejected');
const before = JSON.stringify(view.settings);
let invalidSettings = false;
try { view.setSettings({colorScheme: 'light', javaScript: 5}); } catch (error) { invalidSettings = error instanceof TypeError; }
if (!invalidSettings || JSON.stringify(view.settings) !== before) throw new Error('invalid settings changed the view');
if (!view.setSettings({colorScheme: null, reducedMotion: null}) || JSON.stringify(view.settings) !== '{}')
  throw new Error('settings reset failed');
if (!view.setPopupPolicy('block')) throw new Error('block policy rejected');
let rejected = false;
try { view.setPopupPolicy('invalid'); } catch (error) { rejected = error instanceof TypeError; }
if (!rejected) throw new Error('invalid policy accepted');
const blocked = new Promise(resolve => view.on('popup-blocked', resolve));
view.load("data:text/html,<script>window.open('https://popup.invalid','target')</script>");
const event = await blocked;
if (event.url !== 'https://popup.invalid/' || event.frameName !== 'target' || event.userGesture !== false)
  throw new Error(`unexpected blocked popup: ${JSON.stringify(event)}`);
if (!view.setPopupPolicy('allow')) throw new Error('runtime policy change rejected');
const [base] = zebkit.args;
const origin = base;
if (!view.setPermission('geolocation', 'denied', origin) || view.getPermission('geolocation', origin) !== 'denied')
  throw new Error('native permission rule failed');
for (const action of [() => view.setPermission('geolocation', 'invalid', origin),
                      () => view.setPermission('geolocation\0notifications', 'granted', origin),
                      () => view.setPermission('geolocation', 'granted', 'data:text/plain,opaque'),
                      () => view.setPermissionTimeout(0), () => view.setPermissionTimeout(1.5)]) {
  let invalid = false;
  try { action(); } catch (error) { invalid = error instanceof TypeError; }
  if (!invalid) throw new Error('invalid permission input accepted');
}
if (view.getPermission('geolocation', origin) !== 'denied') throw new Error('invalid permission update changed rule');
view.resetPermissions();
view.setPermissionTimeout(5000);
let savedRequest, count = 0;
view.onPermission(request => {
  ++count;
  savedRequest = request;
  if (JSON.stringify(request.info) !== JSON.stringify(request.metadata) || request.metadata.permission !== 'geolocation')
    throw new Error('permission metadata mismatch');
  if (!request.respond('denied') || request.respond('granted') || request.info !== null)
    throw new Error('permission reply was not exactly once');
  view.onPermission(null);
});
const outcome = new Promise(resolve => view.on('permission-result', resolve));
const loaded = new Promise(resolve => view.on('load', (event, url) => { if (event === 'finished' && url === base + '/permissions.html') resolve(); }));
view.load(base + '/permissions.html');
await loaded;
await view.evaluate('askGeo()');
const result = await outcome;
if (count !== 1 || result.id !== savedRequest.metadata.id || result.reason !== 'answered' || result.state !== 'denied')
  throw new Error('permission outcome mismatch');
if (await view.evaluate("permissionState('notifications')") !== 'denied') throw new Error('handler removal failed');
for (const timeout of [0, 300001, 1.5, true]) {
  let invalid = false;
  try { view.setNavigationTimeout(timeout); } catch (error) { invalid = error instanceof TypeError; }
  if (!invalid) throw new Error('invalid navigation timeout accepted');
}
view.setNavigationTimeout(5000);
let navigationRequest, navigationCount = 0;
const replacementUrl = base + '/settings.html?navigation-binding';
view.onNavigation(request => {
  ++navigationCount;
  navigationRequest = request;
  if (!Object.isFrozen(request) || !Object.isFrozen(request.metadata) || request.metadata.phase !== 'start' ||
      JSON.stringify(request.info) !== JSON.stringify(request.metadata)) throw new Error('navigation metadata mismatch');
  if (request.respond('redirect', 'not a URL') || request.info === null) throw new Error('invalid redirect settled request');
  let nulRejected = false;
  try { request.respond('redirect', base + '/nul\0url'); } catch (error) { nulRejected = error instanceof TypeError; }
  if (!nulRejected || !request.respond('redirect', replacementUrl) || request.respond() || request.info !== null)
    throw new Error('navigation reply was not exactly once');
  view.onNavigation(null);
});
const navigationOutcome = new Promise(resolve => view.on('navigation-result', resolve));
const navigationLoaded = new Promise(resolve => view.on('load', (event, url) => {
  if (event === 'finished' && url === replacementUrl) resolve();
}));
view.load(base + '/permissions.html?original-navigation');
const [navigationResult] = await Promise.all([navigationOutcome, navigationLoaded]);
if (navigationCount !== 1 || navigationResult.id !== navigationRequest.metadata.id || navigationResult.action !== 'redirect' ||
    navigationResult.replacementUrl !== replacementUrl || view.url !== replacementUrl) throw new Error('navigation outcome mismatch');
view.close();
console.log('ok JavaScript native settings, popup policy, blocked event, permissions and navigation');
