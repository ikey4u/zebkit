// One JSON line per test as it ends, so that a run cut short still counts,
// and a progress line every 100 tests.
import fs from 'node:fs';
import path from 'node:path';

const clean = text => String(text || '').replace(/\u001b\[[0-9;]*m/g, '');

export default class Reporter {
  constructor(options) {
    this.file = fs.openSync(options.output, 'w');
    this.seen = 0;
    this.counts = {};
  }
  printsToStdio() { return true; }
  onBegin(config, suite) {
    this.root = config.rootDir;
    console.log(`${suite.allTests().length} tests, ${config.workers} workers`);
  }
  onTestEnd(test, result) {
    const titlePath = test.titlePath();
    const [, project, file, ...title] = titlePath;
    const outcome = test.outcome();
    this.counts[outcome] = (this.counts[outcome] || 0) + 1;
    fs.writeSync(this.file, JSON.stringify({
      project,
      file: path.relative(this.projectDir(test), test.location.file),
      title: title.join(' > '),
      line: test.location.line,
      status: result.status,
      outcome,
      duration: result.duration,
      worker: result.parallelIndex,
      annotations: test.annotations.map(a => [a.type, a.description]).filter(a => a[0] === 'skip' || a[0] === 'fixme' || a[0] === 'fail'),
      error: clean(result.error && (result.error.message || result.error.value)).split('\n').slice(0, 12).join('\n').slice(0, 1500),
    }) + '\n');
    if (++this.seen % 100 === 0)
      console.log(`${this.seen} ${JSON.stringify(this.counts)}`);
  }
  projectDir(test) {
    return test.parent.project().testDir;
  }
  onError(error) {
    console.log(`error: ${clean(error.message).split('\n').slice(0, 5).join(' | ')}`);
  }
  onEnd(result) {
    console.log(`end: ${result.status} ${this.seen} ${JSON.stringify(this.counts)}`);
    fs.closeSync(this.file);
  }
}
