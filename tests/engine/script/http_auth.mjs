// HTTP authentication through zebkit-js's View.onHttpAuth, one mode an engine;
// run by tests/engine/http_auth.py, which has the modes and the server.
import {finish, until, verify} from './helpers.mjs';

const [base, username, password, mode = 'allow'] = zebkit.args;
const view = new zebkit.View('');
const refuses = (call) => { try { call(); } catch (error) { return error instanceof TypeError; } return false; };
verify([0, 300001, 1.5, true].every(invalid => refuses(() => view.setHttpAuthTimeout(invalid))),
  'setHttpAuthTimeout refuses 0, 300001, 1.5 and true');
view.setHttpAuthTimeout(mode === 'timeout' ? 50 : 5000);
// Held: answered by the mode's step outside the function, or by nobody.
const held = ['timeout', 'stop', 'navigation', 'remove', 'replace'].includes(mode);
let request, asked;
const waiting = new Promise(resolve => { asked = resolve; });
const outcomes = [];
const closed = new Promise(resolve => view.on('close', resolve));
const result = new Promise(resolve => view.on('http-auth-result', event => {
  outcomes.push(event);
  verify(!JSON.stringify(event).includes(password), 'no credentials in the outcome');
  if (mode === 'close-result') view.close();
  resolve(event);
}));
const path = base + '/auth/javascript-' + mode;
const loaded = new Promise(resolve => view.on('load', (phase, url) => {
  if (phase === 'finished' && url === path || phase === 'failed') resolve();
}));
if (mode !== 'default') view.onHttpAuth(value => {
  request = value;
  verify(Object.isFrozen(value) && Object.isFrozen(value.metadata) &&
         JSON.stringify(value.info) === JSON.stringify(value.metadata) &&
         !JSON.stringify(value.metadata).includes(password), 'the request: frozen, its info its metadata, no password');
  verify([[null, 'x'], [true, 'x'], ['bad\0', 'x'], ['x'.repeat(65537), 'x']].every(([user, secret]) =>
    refuses(() => value.respond(user, secret))) && value.info !== null,
    'invalid credentials are refused, and the request stays');
  asked();
  if (held) return;
  if (mode === 'cancel-handler') {
    view.onHttpAuth(null);
    verify(value.info === null && !value.respond(username, password), 'removed from inside it: the request is gone');
  } else if (mode === 'close-request') view.close();
  else if (mode === 'deny') {
    verify(value.respond() && value.info === null, 'cancelled with no credentials');
  } else {
    verify(value.respond(username, password) && !value.respond() && value.info === null,
      'answered once: the request is gone, a second reply refused');
    view.onHttpAuth(null);
  }
});
view.load(path);
if (mode === 'close-request' || mode === 'close-result') await closed;
else if (held) {
  await waiting;
  if (mode === 'stop') view.stop();
  else if (mode === 'navigation') view.load('about:blank');
  else if (mode === 'remove') view.onHttpAuth(null);
  else if (mode === 'replace') view.onHttpAuth(() => {});
  const reason = {timeout: 'timeout', remove: 'handler-changed', replace: 'handler-changed'}[mode];
  const settled = await until(() => request.info === null &&
    (!reason || outcomes.some(event => event.id === request.metadata.id && event.reason === reason)), 10000);
  verify(settled, 'the waiting request ' + (reason ? 'ended: ' + reason : 'cancelled'), outcomes);
  verify(!request.respond(username, password), 'and cannot be answered');
  view.close(); await closed;
} else if (mode === 'default') {
  await loaded;
  verify(view.title !== 'auth-ok' && request === undefined, 'without a function the challenge is cancelled', view.title);
  view.close(); await closed;
} else {
  const [, outcome] = await Promise.all([loaded, result]);
  if (mode === 'allow' || mode === 'proxy') verify(view.title === 'auth-ok' && outcome.state === 'provided', 'authenticated', outcome);
  else verify(view.title !== 'auth-ok' && outcome.state === 'cancelled', 'cancelled: no credentials provided', outcome);
  view.close(); await closed;
}
if (mode !== 'default') verify(request && request.info === null && !request.respond(), 'the request settled, none answerable');
finish('JavaScript HTTP auth ' + mode);
