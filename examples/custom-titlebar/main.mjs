// The same window from host JavaScript (zebkit-js):
//
//   zebkit-js examples/custom-titlebar/main.mjs
const page = new URL('page.html', import.meta.url).href;
const view = new zebkit.View(page, {width: 900, height: 600, window: {
  frame: 'overlay', title: 'Custom title bar', background: '#1b1d2a', appearance: 'dark',
  controls: {height: 40, color: '#252838', symbolColor: '#e6e8f2'},
  dragRegions: {source: 'css', origins: ['file://']}, fallback: 'system',
}});
view.on('window-state', (state) => console.log(`window: ${state.state}, title bar area ${JSON.stringify(state.titlebarArea)}`));
view.on('close', () => zebkit.quit());
