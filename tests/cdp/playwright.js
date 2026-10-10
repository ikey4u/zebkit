// Playwright driving the engine through connectOverCDP:
// playwright.js CDP_PORT PAGE_PORT OTHER_PORT
// Run through scripts/cdp.sh <profile> --clients.

'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');
const { chromium } = require('playwright-core');
const { assert, endpoint, pages, other, closed, run } = require('./harness');

let browser;
let context;
let page;

const steps = [
  ['connect', async () => {
    browser = await chromium.connectOverCDP(endpoint);
    context = browser.contexts()[0];
    assert.ok(context, 'the default context is visible');
    assert.ok(context.pages().length >= 1, 'the existing page is visible');
  }],
  ['new page and goto', async () => {
    page = await context.newPage();
    const response = await page.goto(`${pages}/index.html`);
    assert.strictEqual(response.status(), 200);
    assert.strictEqual(await page.title(), 'CDP test page');
    await page.setViewportSize({ width: 1024, height: 768 });
    assert.deepStrictEqual(await page.evaluate(() => [innerWidth, innerHeight]), [1024, 768]);
  }],
  ['evaluate', async () => {
    assert.strictEqual(await page.evaluate(([a, b]) => a * b, [6, 7]), 42);
    assert.deepStrictEqual(await page.evaluate(async () => ({ list: [1, 'two'], nan: NaN, when: await Promise.resolve('later') })), { list: [1, 'two'], nan: NaN, when: 'later' });
    await assert.rejects(page.evaluate(() => { throw new TypeError('boom'); }), /boom/);
    assert.ok((await page.content()).includes('<button id="box">Click me</button>'));
  }],
  ['locators and click', async () => {
    const box = page.locator('#box');
    assert.deepStrictEqual(await box.boundingBox(), { x: 100, y: 80, width: 200, height: 50 });
    await box.click();
    assert.strictEqual(await box.textContent(), 'Clicked 1');
    await page.getByRole('button', { name: 'Clicked 1' }).click();
    assert.strictEqual(await page.evaluate(() => window.clicks), 2);
    await page.getByText('Far away').click();
    assert.strictEqual(await page.evaluate(() => window.farClicked), true, 'click scrolls the target into view');
    assert.strictEqual(await page.locator('button').count(), 2);
    assert.strictEqual(await page.locator('#box').isVisible(), true);
  }],
  ['typing', async () => {
    await page.fill('#name', 'filled 你好');
    assert.strictEqual(await page.inputValue('#name'), 'filled 你好');
    await page.locator('#name').clear();
    await page.locator('#name').pressSequentially('Typed, Wörld');
    assert.strictEqual(await page.inputValue('#name'), 'Typed, Wörld');
    await page.locator('#name').press('Control+a');
    await page.keyboard.press('Delete');
    assert.strictEqual(await page.inputValue('#name'), '');
    await page.locator('#area').focus();
    await page.keyboard.type('line 1');
    await page.keyboard.press('Enter');
    await page.keyboard.type('line 2');
    assert.strictEqual(await page.inputValue('#area'), 'line 1\nline 2');
  }],
  ['frames', async () => {
    const child = page.frame({ name: 'child' });
    assert.ok(child, 'child frame is known');
    assert.strictEqual(child.url(), `${pages}/frame.html`);
    await page.frameLocator('#child').locator('#inner').click();
    assert.strictEqual(await child.evaluate(() => window.innerClicks), 1, 'click lands inside the frame');
    const element = await page.$('#child');
    assert.strictEqual(await element.contentFrame(), child);
  }],
  ['screenshot', async () => {
    await page.evaluate(() => scrollTo(0, 0));
    const image = await page.screenshot();
    assert.deepStrictEqual([image.readUInt32BE(16), image.readUInt32BE(20)], [1024, 768]);
    const element = await page.locator('#box').screenshot();
    assert.deepStrictEqual([element.readUInt32BE(16), element.readUInt32BE(20)], [200, 50]);
  }],
  ['navigation by click, back and forward', async () => {
    await page.click('#link');
    await page.waitForURL(`${pages}/second.html?from=link`);
    assert.strictEqual(await page.evaluate(() => window.query), '?from=link');
    await page.goBack();
    assert.strictEqual(page.url(), `${pages}/index.html`);
    await page.goForward();
    assert.strictEqual(await page.title(), 'Second page');
    await page.reload({ waitUntil: 'networkidle' });
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
    const [response] = await Promise.all([
      page.waitForResponse(candidate => candidate.url().endsWith('pixel.png')),
      page.goto(`${pages}/second.html`, { waitUntil: 'networkidle' }),
    ]);
    assert.strictEqual(response.status(), 200);
    await page.evaluate(() => { setTimeout(() => { throw new Error('thrown later'); }, 0); });
    await page.waitForTimeout(300);
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
    await page.addInitScript(() => { window.early = 'set'; });
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
    const [popup] = await Promise.all([page.waitForEvent('popup'), page.click('#open')]);
    await popup.waitForSelector('#heading');
    assert.strictEqual(popup.url(), `${pages}/second.html?popup=1`);
    await popup.close();
  }],
  ['cookies', async () => {
    await context.addCookies([{ name: 'token', value: 'secret', domain: '127.0.0.1', path: '/' }]);
    await page.goto(`${pages}/second.html`);
    assert.strictEqual(await page.evaluate(() => document.cookie), 'token=secret');
    assert.ok((await context.cookies()).some(cookie => cookie.name === 'token' && cookie.value === 'secret'));
    await context.clearCookies();
    assert.strictEqual(await page.evaluate(() => document.cookie), '');
  }],
  ['file upload', async () => {
    await page.goto(`${pages}/index.html`);
    const file = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'zebkit-cdp-')), 'upload.txt');
    fs.writeFileSync(file, 'twelve bytes');
    await page.setInputFiles('#file', file);
    await page.waitForFunction(() => window.files);
    assert.deepStrictEqual(await page.evaluate(() => window.files), ['upload.txt:12']);
  }],
  ['isolated context', async () => {
    const isolatedContext = await browser.newContext();
    const isolated = await isolatedContext.newPage();
    await isolated.goto(`${pages}/second.html`);
    await isolated.evaluate(() => { document.cookie = 'private=1; path=/'; });
    await page.goto(`${pages}/second.html`);
    assert.strictEqual(await page.evaluate(() => document.cookie), '');
    await isolatedContext.close();
  }],
  ['close page', async () => {
    const count = context.pages().length;
    await page.close();
    assert.strictEqual(context.pages().length, count - 1);
  }],
];

run(steps, async () => browser && browser.close());
