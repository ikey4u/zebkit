const [base, username, password, mode = 'allow'] = zebkit.args;
const view = new zebkit.View('');
for (const invalid of [0, 300001, 1.5, true]) {
  let rejected = false;
  try { view.setHttpAuthTimeout(invalid); } catch (error) { rejected = error instanceof TypeError; }
  if (!rejected) throw new Error('invalid HTTP auth timeout accepted');
}
view.setHttpAuthTimeout(5000);
let request;
const closed = new Promise(resolve => view.on('close', resolve));
const result = new Promise(resolve => view.on('http-auth-result', event => {
  if (JSON.stringify(event).includes(password)) throw new Error('credentials added to outcome');
  if (mode === 'close-result') view.close();
  resolve(event);
}));
const loaded = new Promise(resolve => view.on('load', (phase, url) => {
  if (phase === 'finished' && url === base + '/auth/javascript-' + mode || phase === 'failed') resolve();
}));
view.onHttpAuth(value => {
  request = value;
  if (!Object.isFrozen(value) || !Object.isFrozen(value.metadata) ||
      JSON.stringify(value.info) !== JSON.stringify(value.metadata) ||
      JSON.stringify(value.metadata).includes(password)) throw new Error('HTTP auth metadata mismatch');
  for (const [user, secret] of [[null, 'x'], [true, 'x'], ['bad\0', 'x'], ['x'.repeat(65537), 'x']]) {
    let rejected = false;
    try { value.respond(user, secret); } catch (error) { rejected = error instanceof TypeError; }
    if (!rejected) throw new Error('invalid HTTP auth credentials accepted');
  }
  if (value.info === null) throw new Error('invalid inputs consumed the request');
  if (mode === 'cancel-handler') {
    view.onHttpAuth(null);
    if (value.info !== null || value.respond(username, password)) throw new Error('removed handler left a valid request');
  } else if (mode === 'close-request') view.close();
  else {
    if (!value.respond(username, password) || value.respond() || value.info !== null)
      throw new Error('HTTP auth reply must be exactly once');
    view.onHttpAuth(null);
  }
});
view.load(base + '/auth/javascript-' + mode);
if (mode === 'close-request' || mode === 'close-result') await closed;
else {
  const [, outcome] = await Promise.all([loaded, result]);
  if (mode === 'allow' && (view.title !== 'auth-ok' || !outcome.provided)) throw new Error('HTTP auth not accepted');
  if (mode === 'cancel-handler' && (view.title === 'auth-ok' || outcome.provided)) throw new Error('cancelled auth succeeded');
  view.close(); await closed;
}
if (!request || request.info !== null || request.respond()) throw new Error('closed or settled request remained valid');
console.log('ok JavaScript HTTP auth ' + mode);
