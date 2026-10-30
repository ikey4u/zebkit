const [base] = zebkit.args;
const view = new zebkit.View('');
for (const invalid of [0, 300001, 1.5, true]) {
  let rejected = false;
  try { view.setCertificateTimeout(invalid); } catch (error) { rejected = error instanceof TypeError; }
  if (!rejected) throw new Error('invalid certificate deadline accepted');
}
view.setCertificateTimeout(5000);
let request, count = 0;
view.onCertificate(value => {
  request = value; ++count;
  if (!Object.isFrozen(value) || !Object.isFrozen(value.metadata) ||
      JSON.stringify(value.info) !== JSON.stringify(value.metadata) || !value.metadata.canIgnore ||
      !value.metadata.certificateChain[0].startsWith('-----BEGIN CERTIFICATE-----'))
    throw new Error('certificate request metadata mismatch');
  let rejected = false;
  try { value.respond(1); } catch (error) { rejected = error instanceof TypeError; }
  if (!rejected || !value.respond(true) || value.respond(false) || value.info !== null)
    throw new Error('certificate reply must be valid and exactly once');
  view.onCertificate(null);
});
const outcomes = [];
let resolveOutcome;
const firstOutcome = new Promise(resolve => { resolveOutcome = resolve; });
view.on('certificate-result', event => { outcomes.push(event); if (event.state === 'continued') resolveOutcome(); });
const loaded = new Promise(resolve => view.on('load', (phase, url) => {
  if (phase === 'finished' && url === base + '/javascript-allowed') resolve();
}));
view.load(base + '/javascript-allowed');
await Promise.all([loaded, firstOutcome]);
if (view.title !== 'certificate-ok' || count !== 1 || request.info !== null ||
    !outcomes.some(event => event.id === request.metadata.id && event.state === 'continued'))
  throw new Error('allowed TLS page or outcome missing');
const other = new zebkit.View('');
const denied = new Promise(resolve => other.on('load', phase => { if (phase === 'failed') resolve(); }));
other.load(base + '/javascript-other-denied');
await denied;
if (other.title === 'certificate-ok') throw new Error('certificate exception leaked to another view');
view.onCertificate(value => {
  view.onCertificate(null);
  if (value.info !== null || value.respond(true)) throw new Error('removed handler left a valid request');
});
const replaced = new Promise(resolve => view.on('load', phase => { if (phase === 'failed') resolve(); }));
view.load(base + '/javascript-replaced');
await replaced;
view.close(); other.close();
console.log('ok JavaScript certificate policy: actual TLS, metadata, single-use reply, isolation and callback removal');
