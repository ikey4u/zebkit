// The forms of zebkit-js that do not wait on the engine's thread, and why a
// hook's reply was refused: cdp.listenAsync (zebkit_cdp_listen_async), a
// second listen asking for another port, openExternalAsync and
// showInFolderAsync (zebkit_open_external_async, zebkit_show_in_folder_async)
// refusing what they must, and request.error after a refused respond().
// Arguments: the local server's base URL, and a directory that is there
// (absolute, in this system's form), for a file that is not.
import {finish, loaded, verify as check, within} from './helpers.mjs';

const [base, directory] = zebkit.args;
check(typeof directory === 'string' && directory.length > 0, 'a directory to look in', zebkit.args);

// CDP served without waiting: the port and the browser target's address.
const served = await zebkit.cdp.listenAsync('127.0.0.1', 0);
check(served.port > 0 && zebkit.cdp.port === served.port &&
      served.url.startsWith(`ws://127.0.0.1:${served.port}/devtools/browser/`), 'cdp.listenAsync', served);
const again = await zebkit.cdp.listenAsync('127.0.0.1', 0);
check(again.port === served.port && again.url === served.url, 'a second listenAsync: what is served', again);
const other = served.port === 65535 ? 65534 : served.port + 1;
const conflict = await zebkit.cdp.listenAsync('127.0.0.1', other).then(() => 'served', e => String(e.message || e));
check(conflict.includes('already'), 'another port while served: an error', conflict);
let blocking = 'served';
try { zebkit.cdp.listen('127.0.0.1', other); } catch (e) { blocking = String(e.message || e); }
check(blocking.includes('already'), 'the blocking form says the same', blocking);
zebkit.cdp.stop();
check(zebkit.cdp.port === 0, 'cdp.stop');

// What the system is not asked for.
const relative = await zebkit.showInFolderAsync('relative/path').then(() => 'shown', e => String(e.message || e));
check(relative.includes('absolute'), 'showInFolderAsync: a relative path is refused', relative);
// Absolute on every system (a Windows path has its drive): what refuses it is
// only that it is not there.
const notThere = `${directory.replace(/[\\/]+$/, '')}/zebkit-not-there.txt`;
const missing = await zebkit.showInFolderAsync(notThere).then(() => 'shown', e => String(e.message || e));
check(missing.includes('no such file'), 'showInFolderAsync: a file that is not there', [notThere, missing]);
const script = await zebkit.openExternalAsync('javascript:alert(1)').then(() => 'opened', e => String(e.message || e));
check(script.includes('javascript:'), 'openExternalAsync: a javascript: URL is refused', script);

// request.error: the C API's reason for a refused reply.
const view = new zebkit.View('');
const done = loaded(view);
view.load(`${base}/`);
await within(done, 15000, 'the page');
const errors = [];
view.onNavigation(request => {
  const refused = request.respond('continue', 'http://127.0.0.1/');
  errors.push([refused, request.error]);
  request.respond('continue');
  errors.push([true, request.error]);
});
view.load(`${base}/?next`);
await within(loaded(view), 15000, 'the navigation');
view.onNavigation(null);
// Each of its phases asked once: the same both times.
check(errors.length >= 2 && errors[0][0] === false && String(errors[0][1]).includes('replacement URL') &&
      errors[1][1] === null, 'request.error: why, then null once answered', errors);
check(typeof view.onDownloadResult === 'function', 'view.onDownloadResult');
view.close();
finish('async forms');

