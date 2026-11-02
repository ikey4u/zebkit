// The Electron host of scripts/electron-compare.sh: what ../host.c does, in
// Electron's API. One window, its content 1280 by 800, no menu bar (the zebkit
// host has none), showing BENCH_URL, with the same user agent as zebkit's;
// with BENCH_BIND set, the page can call echo(text) on the host.
const { app, BrowserWindow, Menu, ipcMain } = require('electron');
const path = require('node:path');

if (process.env.BENCH_UA) app.userAgentFallback = process.env.BENCH_UA;
const bind = !!process.env.BENCH_BIND;
Menu.setApplicationMenu(null);

app.whenReady().then(() => {
  if (bind) ipcMain.handle('echo', (event, text) => text);
  const win = new BrowserWindow({
    width: 1280,
    height: 800,
    useContentSize: true,
    webPreferences: {
      // Electron's PDF viewer is off unless asked for; zebkit's is on.
      plugins: true,
      preload: bind ? path.join(__dirname, 'preload.js') : undefined,
    },
  });
  win.on('closed', () => app.quit());
  win.loadURL(process.env.BENCH_URL);
});
