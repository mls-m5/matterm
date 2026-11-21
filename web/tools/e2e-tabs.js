// Tabs and panes in server mode: prefix keys (Ctrl+B), new tab, split, switch,
// close; writes OUTDIR/tabs1..4.png. Fails on page errors.
// usage: node e2e-tabs.js URL PASSWORD OUTDIR
const { chromium } = require('playwright-core');
(async () => {
  const [url, pw, out] = process.argv.slice(2);
  const browser = await chromium.launch({ args: ['--use-gl=angle','--use-angle=swiftshader','--enable-unsafe-swiftshader','--ignore-gpu-blocklist','--no-sandbox'] });
  const ctx = await browser.newContext({ viewport: { width: 1000, height: 640 } });
  const page = await ctx.newPage();
  let errors = 0;
  page.on('console', m => console.log('console:', m.text()));
  page.on('pageerror', e => { errors++; console.log('pageerror:', e.message); });
  await page.goto(url);
  await page.fill('#password', pw);
  await page.click('button');
  await page.waitForTimeout(3000);
  const prefix = async (k) => { await page.keyboard.press('Control+b'); await page.waitForTimeout(150); await page.keyboard.press(k); await page.waitForTimeout(700); };
  await page.keyboard.type('echo first-tab\n', { delay: 20 });
  await page.waitForTimeout(500);
  await prefix('c');
  await page.keyboard.type('echo second-tab\n', { delay: 20 });
  await page.waitForTimeout(500);
  await page.screenshot({ path: out + '/tabs1.png' });
  await prefix('Shift+5'); // %: split side by side
  await page.keyboard.type('echo split-pane\n', { delay: 20 });
  await page.waitForTimeout(500);
  await page.screenshot({ path: out + '/tabs2.png' });
  await prefix('p');
  await page.screenshot({ path: out + '/tabs3.png' });
  await prefix('x');
  await page.waitForTimeout(500);
  await page.screenshot({ path: out + '/tabs4.png' });
  await browser.close();
  if (errors) { console.error('FAIL page errors:', errors); process.exit(1); }
  console.log('PASS tabs: no page errors, screenshots in ' + out);
})().catch(e => { console.error('FAIL', e.message.split('\n')[0]); process.exit(1); });
