// zebkit-js: a view's settings, its popup policy and the blocked-popup event,
// permissions and their function, and navigations answered by the application.
// Argument: the local server's base URL (tests/engine/pages).
import {finish, throws, verify as check, within} from './helpers.mjs';

const [base] = zebkit.args;
const origin = base;
const view = new zebkit.View('');
check(JSON.stringify(view.settings) === '{}', 'a new view has no settings', view.settings);
check(view.setSettings({colorScheme: 'dark', reducedMotion: true}), 'setSettings takes a colour scheme and reduced motion');
const before = JSON.stringify(view.settings);
check(throws(() => view.setSettings({colorScheme: 'light', javaScript: 5}), TypeError) &&
      JSON.stringify(view.settings) === before, 'invalid settings are refused and change nothing', view.settings);
check(view.setSettings({colorScheme: null, reducedMotion: null}) && JSON.stringify(view.settings) === '{}',
      'settings reset with null', view.settings);

check(view.setPopupPolicy('block'), "setPopupPolicy('block')");
check(throws(() => view.setPopupPolicy('invalid'), TypeError), 'an invalid popup policy is refused');
const blocked = new Promise(resolve => view.on('popup-blocked', resolve));
view.load("data:text/html,<script>window.open('https://popup.invalid','target')</script>");
const event = await within(blocked, 15000, 'popup-blocked').catch(error => ({error: String(error)}));
check(event.url === 'https://popup.invalid/' && event.frameName === 'target' && event.userGesture === false,
      'popup-blocked: the URL, the frame name and no gesture', event);
check(view.setPopupPolicy('allow'), 'the popup policy changed at run time');

check(view.setPermission('geolocation', 'denied', origin) && view.getPermission('geolocation', origin) === 'denied',
      'a permission rule of the application');
const invalid = [() => view.setPermission('geolocation', 'invalid', origin),
                 () => view.setPermission('geolocation\0notifications', 'granted', origin),
                 () => view.setPermission('geolocation', 'granted', 'data:text/plain,opaque'),
                 () => view.setPermissionTimeout(0), () => view.setPermissionTimeout(1.5)];
check(invalid.every(action => throws(action, TypeError)), 'invalid permission input is refused');
check(view.getPermission('geolocation', origin) === 'denied', 'and changes no rule');
view.resetPermissions();
view.setPermissionTimeout(5000);
let savedRequest, count = 0, metadataSame = false, once = false;
view.onPermission(request => {
  ++count;
  savedRequest = request;
  metadataSame = JSON.stringify(request.info) === JSON.stringify(request.metadata) && request.metadata.permission === 'geolocation';
  once = request.respond('denied') && !request.respond('granted') && request.info === null;
  view.onPermission(null);
});
const outcome = new Promise(resolve => view.on('permission-result', resolve));
const loaded = new Promise(resolve => view.on('load', (event, url) => { if (event === 'finished' && url === base + '/permissions.html') resolve(); }));
view.load(base + '/permissions.html');
await within(loaded, 15000, 'permissions.html');
await view.evaluate('askGeo()');
const result = await within(outcome, 15000, 'permission-result').catch(error => ({error: String(error)}));
check(metadataSame, "a permission request's metadata: geolocation, info the same");
check(once, 'a permission request is answered exactly once');
check(count === 1 && savedRequest && result.id === savedRequest.metadata.id && result.reason === 'answered' &&
      result.state === 'denied', 'permission-result: answered, denied', result);
// Notifications are undecided without a decision (docs/SYSTEM_INTEGRATION.md, 12).
check(await view.evaluate("permissionState('notifications')") === 'prompt', 'with the function removed, a permission is undecided');

check([0, 300001, 1.5, true].every(timeout => throws(() => view.setNavigationTimeout(timeout), TypeError)),
      'an invalid navigation timeout is refused');
view.setNavigationTimeout(5000);
let navigationRequest, navigationCount = 0, navigationMetadata = false, navigationOnce = false;
const replacementUrl = base + '/settings.html?navigation-binding';
view.onNavigation(request => {
  ++navigationCount;
  navigationRequest = request;
  navigationMetadata = Object.isFrozen(request) && Object.isFrozen(request.metadata) && request.metadata.phase === 'start' &&
      JSON.stringify(request.info) === JSON.stringify(request.metadata);
  const invalidKept = !request.respond('redirect', 'not a URL') && request.info !== null;
  const nulRefused = throws(() => request.respond('redirect', base + '/nul\0url'), TypeError);
  navigationOnce = invalidKept && nulRefused && request.respond('redirect', replacementUrl) && !request.respond() &&
      request.info === null;
  view.onNavigation(null);
});
const navigationOutcome = new Promise(resolve => view.on('navigation-result', resolve));
const navigationLoaded = new Promise(resolve => view.on('load', (event, url) => {
  if (event === 'finished' && url === replacementUrl) resolve();
}));
view.load(base + '/permissions.html?original-navigation');
const [navigationResult] = await within(Promise.all([navigationOutcome, navigationLoaded]), 15000, 'the redirected navigation')
  .catch(error => [{error: String(error)}]);
check(navigationMetadata, "a navigation request's metadata: frozen, the start, info the same");
check(navigationOnce, 'a navigation request: an invalid reply settles nothing, a redirect exactly once');
check(navigationCount === 1 && navigationRequest && navigationResult.id === navigationRequest.metadata.id &&
      navigationResult.state === 'redirected' && navigationResult.replacementUrl === replacementUrl && view.url === replacementUrl,
      'navigation-result: redirected, and loaded there', navigationResult);
view.close();
finish('JavaScript native settings, popup policy, blocked event, permissions and navigation');
