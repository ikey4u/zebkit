// Shared by puppeteer.js and playwright.js: a list of named steps, each of
// which must not throw, and a summary line like tests/cdp/protocol.py prints.

'use strict';

const assert = require('assert');

const [cdpPort, pagePort, otherPort] = process.argv.slice(2, 5).map(Number);
const only = process.argv.slice(5);

exports.assert = assert;
exports.endpoint = `http://127.0.0.1:${cdpPort}`;
exports.pages = `http://127.0.0.1:${pagePort}`;
exports.other = `http://localhost:${otherPort}`;
// An address nothing listens on.
exports.closed = `http://127.0.0.1:${otherPort + 7}/`;

exports.run = async function run(steps, cleanup) {
  let failures = 0;
  let ran = 0;
  for (const [name, step] of steps) {
    if (only.length && !only.includes(name))
      continue;
    ran++;
    const started = Date.now();
    try {
      await Promise.race([
        step(),
        new Promise((_, reject) => setTimeout(() => reject(new Error('step timed out after 60 s')), 60000)),
      ]);
      console.log(`ok    ${name} (${((Date.now() - started) / 1000).toFixed(1)}s)`);
    } catch (error) {
      failures++;
      console.log(`FAIL  ${name}: ${process.env.CDP_TRACE ? error.stack : error.message}`);
    }
  }
  console.log(`${ran - failures} passed, ${failures} failed`);
  try {
    await cleanup();
  } catch (error) {
    // The browser is killed by the caller anyway.
  }
  process.exit(failures ? 1 : 0);
};
