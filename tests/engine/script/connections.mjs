// zebkit-js's connection reuse (docs/CONNECTION_REUSE.md): zebkit.preconnect,
// zebkit.clearHttpCache and zebkit.fetch(url, {context}), which sends that
// Context's cookies (zebkit_fetch_in). Argument: the local server's base URL,
// whose /__zebkit_headers__ answers the request's headers.
import {finish, loaded, verify as check, within} from './helpers.mjs';

const [base] = zebkit.args;

check(zebkit.preconnect(base, {count: 2}) === true, 'preconnect to the engine profile');
check(zebkit.preconnect('ftp://127.0.0.1/') === false && zebkit.preconnect(base, {count: 0}) === false &&
      zebkit.preconnect(base, {count: 7}) === false && zebkit.preconnect('not a URL') === false,
      'preconnect refuses what is not http(s) and a count out of 1 to 6');

const context = new zebkit.Context();
check(zebkit.preconnect(base, {context}) === true, 'preconnect in a context');
const view = new zebkit.View('', {context});
const done = loaded(view);
view.load(base + '/page.html');
await within(done, 15000, 'the page in the context');
await view.evaluate("document.cookie = 'js=context; path=/'");

const headers = async (options) => {
  const response = await zebkit.fetch(base + '/__zebkit_headers__', options);
  return JSON.parse(new TextDecoder().decode(response.body));
};
const inside = await headers({context});
const outside = await headers();
check(inside.Cookie === 'js=context', "fetch({context}) sends the context's cookie", inside);
check(outside.Cookie === undefined, "fetch() sends the engine profile's, not the context's", outside);

check(await zebkit.clearHttpCache().then(() => true, (e) => e.message), 'clearHttpCache() of the engine profile');
check(await zebkit.clearHttpCache({context}).then(() => true, (e) => e.message), "clearHttpCache() of a context");

context.close();
const refused = (f) => { try { f(); return false; } catch (e) { return /closed/.test(e.message); } };
check(refused(() => zebkit.preconnect(base, {context})) && refused(() => zebkit.clearHttpCache({context})) &&
      refused(() => zebkit.fetch(base, {context})), 'a closed context is refused');
finish('the connection reuse script');
