// What a page gets of the host in Electron, as an application exposes it.
const { contextBridge, ipcRenderer } = require('electron');
contextBridge.exposeInMainWorld('host', { echo: text => ipcRenderer.invoke('echo', text) });
