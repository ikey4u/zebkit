// A document's TLS errors through zebkit-js's View.onCertificate, one mode an
// engine; run by tests/engine/certificate.py, which has the modes and the server.
import {finish, verify} from './helpers.mjs';

const [base, mode = 'allow'] = zebkit.args;
const url = base + '/javascript-' + mode;
const view = new zebkit.View('');
const refuses = (call) => { try { call(); } catch (error) { return error instanceof TypeError; } return false; };
verify([0, 300001, 1.5, true].every(invalid => refuses(() => view.setCertificateTimeout(invalid))),
  'setCertificateTimeout refuses 0, 300001, 1.5 and true');
view.setCertificateTimeout(5000);
let request, count = 0;
const outcomes = [];
const closed = new Promise(resolve => view.on('close', resolve));
const outcome = new Promise(resolve => view.on('certificate-result', event => {
  outcomes.push(event);
  if (mode === 'close-result') view.close();
  resolve(event);
}));
const loaded = new Promise(resolve => view.on('load', (phase, where) => {
  if (phase === 'failed' || phase === 'finished' && where === url) resolve(phase);
}));
if (mode !== 'default') view.onCertificate(value => {
  request = value; ++count;
  verify(Object.isFrozen(value) && Object.isFrozen(value.metadata) &&
         JSON.stringify(value.info) === JSON.stringify(value.metadata) &&
         value.metadata.certificateChain[0].startsWith('-----BEGIN CERTIFICATE-----'),
    'the request: frozen, its info its metadata, the chain');
  verify(refuses(() => value.respond(1)) && value.info !== null,
    'a reply that is not a boolean is refused, and the request stays');
  if (mode === 'strict') {
    verify(value.metadata.strictEnforcement && !value.metadata.canIgnore, 'a pinned host: strict', value.metadata);
    verify(!value.respond(true) && value.info !== null && value.respond(false), 'its error cannot be allowed, only refused');
  } else if (mode === 'cancel-handler') {
    view.onCertificate(null);
    verify(value.info === null && !value.respond(true), 'removed from inside it: the request is gone');
  } else if (mode === 'close-request') view.close();
  else {
    const allow = mode === 'allow';
    if (allow) verify(value.metadata.canIgnore && !value.metadata.strictEnforcement, 'an error that can be allowed');
    verify(value.respond(allow) && !value.respond(!allow) && value.info === null,
      'answered once: the request is gone, a second reply refused');
    view.onCertificate(null);
  }
});
view.load(url);
if (mode === 'close-request' || mode === 'close-result') {
  await closed;
} else if (mode === 'allow') {
  await Promise.all([loaded, outcome]);
  verify(view.title === 'certificate-ok' && count === 1 && request.info === null &&
    outcomes.some(event => event.id === request.metadata.id && event.state === 'continued'),
    'the allowed page, its continued outcome', outcomes);
  // Once only: another view of the profile is refused, without being asked.
  const other = new zebkit.View('');
  const denied = new Promise(resolve => other.on('load', phase => { if (phase === 'failed') resolve(); }));
  other.load(url + '-other');
  await denied;
  verify(other.title !== 'certificate-ok' && count === 1, 'another view still refused, not asked');
  other.close();
} else {
  verify(await loaded === 'failed', 'the page refused');
  if (mode === 'default') verify(count === 0, 'without a function nobody is asked');
  else {
    await outcome;
    const reason = mode === 'cancel-handler' ? 'handler-changed' : 'answered';
    verify(outcomes.some(event => event.reason === reason), 'the outcome: ' + reason, outcomes);
  }
}
if (mode !== 'close-request' && mode !== 'close-result') { view.close(); await closed; }
if (mode !== 'default') verify(request && request.info === null && !request.respond(true), 'the request settled, none answerable');
finish('JavaScript certificate policy ' + mode);
