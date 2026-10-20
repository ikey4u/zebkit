// Imported by app.mjs: modules load from files, relative to the importer.
export const results = [];

export async function check(name, test) {
  try {
    const value = await test();
    results.push([name, value === true ? true : `unexpected: ${JSON.stringify(value)}`]);
  } catch (e) {
    results.push([name, String(e && e.message || e)]);
  }
  const [, outcome] = results[results.length - 1];
  console.log(`${outcome === true ? 'ok  ' : 'FAIL'} ${name}${outcome === true ? '' : ': ' + outcome}`);
}

export const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
