// One zebkit-run for each Playwright worker, at <base port> + index, restarted
// when it exits, so that a crash costs one worker's test and not the run.
// Each exit is logged: they are counted as crashes.
//
//   node engines.mjs <zebkit-run> <base port> <count> <work dir>
//
// Stops its engines on SIGTERM.
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';

const [executable, base, count, work] = process.argv.slice(2);
const children = new Set();
let stopping = false;
const crashes = path.join(work, 'engine-exits.log');

function start(index, generation) {
  const profile = path.join(work, 'profiles', `${index}-${generation}`);
  fs.mkdirSync(profile, { recursive: true });
  const log = fs.openSync(path.join(work, `engine-${index}.log`), 'a');
  const child = spawn(executable, ['--headless', '--no-sandbox', `--user-data-dir=${profile}`, `--playwright-port=${Number(base) + index}`],
      { stdio: ['ignore', log, log], detached: true });
  children.add(child);
  child.on('exit', (code, signal) => {
    children.delete(child);
    fs.closeSync(log);
    fs.rmSync(profile, { recursive: true, force: true, maxRetries: 3 });
    if (stopping)
      return;
    fs.appendFileSync(crashes, `${new Date().toISOString()} engine ${index} generation ${generation} exited code=${code} signal=${signal}\n`);
    setTimeout(() => start(index, generation + 1), 500);
  });
}
for (let i = 0; i < Number(count); ++i)
  start(i, 0);
for (const name of ['SIGTERM', 'SIGINT']) {
  process.on(name, () => {
    stopping = true;
    for (const child of children) {
      try { process.kill(-child.pid, 'SIGKILL'); } catch {}
    }
    setTimeout(() => process.exit(0), 300);
  });
}
