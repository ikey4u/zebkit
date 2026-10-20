// Hello world in Electron: one window with one page.
//
// With --check, the window stays hidden, the page's text is printed once it
// has loaded, and the program quits, so that a script can tell it works.

const { app, BrowserWindow } = require('electron');
const path = require('node:path');

const check = process.argv.includes('--check');

app.whenReady().then(() => {
  const win = new BrowserWindow({ width: 800, height: 600, show: !check });
  win.loadFile(path.join(__dirname, 'index.html'));
  if (check) {
    win.webContents.once('did-finish-load', async () => {
      const text = await win.webContents.executeJavaScript('document.body.textContent');
      console.log(`hello: ${text.trim()}`);
      app.quit();
    });
  }
});

app.on('window-all-closed', () => app.quit());
