// Puppeteer driving the engine over CDP: puppeteer.js CDP_PORT PAGE_PORT OTHER_PORT
// Run through scripts/cdp.sh <profile> --clients.

'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');
const puppeteer = require('puppeteer-core');
const { assert, endpoint, pages, other, closed, run } = require('./harness');

let browser;
let page;

const steps = [
  ['connect', async () => {
    browser = await puppeteer.connect({ browserURL: endpoint, defaultViewport: { width: 1024, height: 768 } });
    assert.match(await browser.version(), /^Zebkit\//);
    assert.ok((await browser.pages()).length >= 1, 'the existing page is visible');
  }],
  ['new page and goto', async () => {
    page = await browser.newPage();
    const response = await page.goto(`${pages}/index.html`);
    assert.strictEqual(response.status(), 200);
    assert.strictEqual(response.url(), `${pages}/index.html`);
    assert.strictEqual(await page.title(), 'CDP test page');
    assert.deepStrictEqual(await page.evaluate(() => [innerWidth, innerHeight]), [1024, 768]);
  }],
  ['evaluate', async () => {
    assert.strictEqual(await page.evaluate((a, b) => a * b, 6, 7), 42);
    assert.deepStrictEqual(await page.evaluate(async () => ({ list: [1, 'two'], when: await Promise.resolve('later') })), { list: [1, 'two'], when: 'later' });
    await assert.rejects(page.evaluate(() => { throw new TypeError('boom'); }), /boom/);
    const handle = await page.evaluateHandle(() => document.body);
    assert.strictEqual(await handle.evaluate(body => body.tagName), 'BODY');
    await handle.dispose();
    assert.ok((await page.content()).includes('<button id="box">Click me</button>'));
  }],
  ['selectors and click', async () => {
    const box = await page.waitForSelector('#box');
    assert.deepStrictEqual(await box.boundingBox(), { x: 100, y: 80, width: 200, height: 50 });
    await page.click('#box');
    assert.strictEqual(await page.$eval('#box', element => element.textContent), 'Clicked 1');
    await page.locator('#box').click();
    assert.strictEqual(await page.evaluate(() => window.clicks), 2);
    assert.strictEqual((await page.$$('button')).length, 2);
    await page.click('#far');
    assert.strictEqual(await page.evaluate(() => window.farClicked), true, 'click scrolls the target into view');
  }],
  ['typing', async () => {
    await page.type('#name', 'Hello, Wörld 你好');
    assert.strictEqual(await page.$eval('#name', element => element.value), 'Hello, Wörld 你好');
    // On macOS, Select All is on Command, and what a key does to the text
    // is not the browser's to know: Puppeteer has the test say it.
    const onMac = process.platform === 'darwin';
    await page.keyboard.down(onMac ? 'Meta' : 'Control');
    await page.keyboard.press('a', onMac ? { commands: ['SelectAll'] } : {});
    await page.keyboard.up(onMac ? 'Meta' : 'Control');
    await page.keyboard.press('Backspace');
    assert.strictEqual(await page.$eval('#name', element => element.value), '');
    await page.focus('#area');
    await page.keyboard.type('line 1');
    await page.keyboard.press('Enter');
    await page.keyboard.type('line 2');
    assert.strictEqual(await page.$eval('#area', element => element.value), 'line 1\nline 2');
  }],
  ['frames', async () => {
    const child = page.frames().find(frame => frame.name() === 'child');
    assert.ok(child, 'child frame is known');
    assert.strictEqual(child.url(), `${pages}/frame.html`);
    assert.strictEqual(child.parentFrame(), page.mainFrame());
    const inner = await child.waitForSelector('#inner');
    await inner.click();
    assert.strictEqual(await child.evaluate(() => window.innerClicks), 1, 'click lands inside the frame');
    const element = await page.$('#child');
    assert.strictEqual(await element.contentFrame(), child);
  }],
  ['screenshot', async () => {
    await page.evaluate(() => scrollTo(0, 0));
    const image = await page.screenshot();
    assert.deepStrictEqual([image.readUInt32BE(16), image.readUInt32BE(20)], [1024, 768]);
    const full = await page.screenshot({ fullPage: true });
    assert.ok(full.readUInt32BE(20) >= 3040, `full page height ${full.readUInt32BE(20)}`);
    const element = await (await page.$('#box')).screenshot();
    assert.deepStrictEqual([element.readUInt32BE(16), element.readUInt32BE(20)], [200, 50]);
  }],
  ['navigation by click, back and forward', async () => {
    await Promise.all([page.waitForNavigation(), page.click('#link')]);
    assert.strictEqual(page.url(), `${pages}/second.html?from=link`);
    assert.strictEqual(await page.evaluate(() => window.query), '?from=link');
    await page.goBack();
    assert.strictEqual(page.url(), `${pages}/index.html`);
    await page.goForward();
    assert.strictEqual(await page.title(), 'Second page');
    await page.reload({ waitUntil: 'networkidle0' });
    assert.strictEqual(await page.title(), 'Second page');
  }],
  ['other origin', async () => {
    await page.goto(`${other}/index.html`, { waitUntil: 'domcontentloaded' });
    assert.strictEqual(await page.evaluate(() => location.origin), other);
    await page.click('#box');
    assert.strictEqual(await page.evaluate(() => window.clicks), 1);
    await assert.rejects(page.goto(closed), /ERR_CONNECTION_REFUSED/);
  }],
  ['console, errors and requests', async () => {
    const messages = [];
    const errors = [];
    const requests = [];
    const onConsole = message => messages.push(`${message.type()}:${message.text()}`);
    const onError = error => errors.push(error.message);
    const onRequest = request => requests.push(request.url());
    page.on('console', onConsole);
    page.on('pageerror', onError);
    page.on('request', onRequest);
    await page.goto(`${pages}/second.html`, { waitUntil: 'networkidle0' });
    await page.evaluate(() => { setTimeout(() => { throw new Error('thrown later'); }, 0); });
    await new Promise(resolve => setTimeout(resolve, 300));
    assert.ok(messages.some(message => message.startsWith('log:second loaded 42')), `console: ${messages}`);
    assert.ok(errors.some(error => error.includes('thrown later')), `errors: ${errors}`);
    assert.ok(requests.includes(`${pages}/pixel.png`), `requests: ${requests}`);
    page.off('console', onConsole);
    page.off('pageerror', onError);
    page.off('request', onRequest);
  }],
  ['expose function and init script', async () => {
    const calls = [];
    await page.exposeFunction('askHost', value => { calls.push(value); return value * 2; });
    await page.evaluateOnNewDocument(() => { window.early = 'set'; });
    await page.goto(`${pages}/index.html`);
    assert.strictEqual(await page.evaluate(() => window.early), 'set');
    assert.strictEqual(await page.evaluate(() => window.askHost(21)), 42);
    assert.deepStrictEqual(calls, [21]);
  }],
  ['dialogs', async () => {
    await page.goto(`${pages}/dialogs.html`);
    page.once('dialog', dialog => dialog.accept('typed'));
    await page.click('#prompt');
    assert.strictEqual(await page.evaluate(() => window.answer), 'typed');
    page.once('dialog', dialog => dialog.dismiss());
    await page.click('#confirm');
    assert.strictEqual(await page.evaluate(() => window.confirmed), false);
  }],
  ['popup', async () => {
    const [popup] = await Promise.all([
      new Promise(resolve => page.once('popup', resolve)),
      page.click('#open'),
    ]);
    await popup.waitForSelector('#heading');
    assert.strictEqual(popup.url(), `${pages}/second.html?popup=1`);
    await popup.close();
  }],
  ['cookies and user agent', async () => {
    await browser.setCookie({ name: 'token', value: 'secret', domain: '127.0.0.1', path: '/' });
    await page.setUserAgent('PuppeteerOnZebkit/1.0');
    await page.goto(`${pages}/second.html`);
    assert.strictEqual(await page.evaluate(() => document.cookie), 'token=secret');
    assert.strictEqual(await page.evaluate(() => navigator.userAgent), 'PuppeteerOnZebkit/1.0');
    const cookies = await browser.cookies();
    assert.ok(cookies.some(cookie => cookie.name === 'token' && cookie.value === 'secret'));
    await browser.deleteCookie(...cookies);
    assert.strictEqual(await page.evaluate(() => document.cookie), '');
  }],
  ['file upload', async () => {
    await page.goto(`${pages}/index.html`);
    const file = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'zebkit-cdp-')), 'upload.txt');
    fs.writeFileSync(file, 'twelve bytes');
    const input = await page.$('#file');
    await input.uploadFile(file);
    await page.waitForFunction(() => window.files);
    assert.deepStrictEqual(await page.evaluate(() => window.files), ['upload.txt:12']);
  }],
  ['isolated context', async () => {
    const context = await browser.createBrowserContext();
    const isolated = await context.newPage();
    await isolated.goto(`${pages}/second.html`);
    await isolated.evaluate(() => { document.cookie = 'private=1; path=/'; });
    await page.goto(`${pages}/second.html`);
    assert.strictEqual(await page.evaluate(() => document.cookie), '');
    await context.close();
  }],
  ['close page', async () => {
    const count = (await browser.pages()).length;
    await page.close();
    assert.strictEqual((await browser.pages()).length, count - 1);
  }],
];

run(steps, async () => browser && browser.disconnect());
