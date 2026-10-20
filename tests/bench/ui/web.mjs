// The same as native.mjs, as a web page in a view: plain DOM, the fastest
// a page gets.
const n = Number(zebkit.args[0] || 1000);
const started = Date.now();
const page = `<!doctype html><meta charset=utf-8><body style="margin:0;font:14px sans-serif">
<div id=list style="display:flex;flex-direction:column;gap:4px;padding:8px"></div><script>
const list = document.getElementById('list');
for (let i = 0; i < ${n}; ++i) {
  const row = document.createElement('div');
  row.style.cssText = 'display:flex;gap:8px;align-items:center';
  const button = document.createElement('button');
  button.textContent = 'Open';
  const text = document.createElement('span');
  text.textContent = 'row ' + i;
  row.append(button, text);
  list.append(row);
}
list.offsetHeight;
</script>`;
const view = new zebkit.View('data:text/html;base64,' +
    btoa(String.fromCharCode(...new TextEncoder().encode(page))), {width: 600, height: 800});
view.on('load', async (event) => {
  if (event !== 'finished') return;
  const shown = Date.now() - started;
  const updated = await view.evaluate(`(() => {
    const start = performance.now();
    for (const span of document.querySelectorAll('span')) span.textContent += '!';
    document.body.offsetHeight;
    return Math.round(performance.now() - start);
  })()`);
  console.log(`RESULT ${JSON.stringify({shown, updated})}`);
});
