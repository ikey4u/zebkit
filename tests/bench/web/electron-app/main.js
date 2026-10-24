// The Electron side of scripts/web-bench.sh: one window with the page in
// BENCH_URL, the way an application shows one, with the same user agent as
// zebkit's when BENCH_UA is set.
const { app, BrowserWindow } = require('electron');

if (process.env.BENCH_UA) app.userAgentFallback = process.env.BENCH_UA;

app.whenReady().then(() => {
  const win = new BrowserWindow({ width: 1280, height: 800 });
  win.loadURL(process.env.BENCH_URL);
});
