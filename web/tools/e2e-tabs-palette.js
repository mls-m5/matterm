// The command palette in the browser: Ctrl+B ':' opens it, typing filters, Enter
// runs the command, Escape closes, the strip's button opens it, a click outside
// closes it. Reads the ?debug plan lines ("layers=N").
// usage: node e2e-tabs-palette.js URL PASSWORD OUTDIR   (URL without ?debug)
const { chromium } = require('playwright-core');
const fails = [];
function check(name, ok, extra) {
  console.log((ok ? 'PASS ' : 'FAIL ') + name + (ok ? '' : ' ' + (extra || '')));
  if (!ok) fails.push(name);
}
const parse = (plan) => {
  const m = /plan tabs=(\d+) panes=(\d+) strip=(\d+) focus=(\d+) layers=(\d+)/.exec(plan || '');
  return m ? { tabs: +m[1], total: +m[2], strip: +m[3], layers: +m[5] } : null;
};
(async () => {
  const [url, pw, out] = process.argv.slice(2);
  const browser = await chromium.launch({ args: ['--use-gl=angle','--use-angle=swiftshader','--enable-unsafe-swiftshader','--ignore-gpu-blocklist','--no-sandbox'] });
  const page = await (await browser.newContext({ viewport: { width: 1000, height: 640 } })).newPage();
  const plans = [];
  page.on('console', m => { if (m.text().includes('plan tabs')) plans.push(m.text()); });
  page.on('pageerror', e => check('no page error', false, e.message));
  const last = () => parse(plans[plans.length - 1]);
  await page.goto(url + '?debug');
  await page.fill('#password', pw);
  await page.click('button');
  await page.waitForTimeout(3500);
  const prefix = async (k) => { await page.keyboard.press('Control+b'); await page.waitForTimeout(150); await page.keyboard.press(k); await page.waitForTimeout(600); };
  await prefix('Shift+;');
  let p = last();
  check('Ctrl+B : opens the palette', p && p.layers === 1, JSON.stringify(p));
  await page.screenshot({ path: out + '/palette-open.png' });
  await page.keyboard.type('new tab', { delay: 30 });
  await page.waitForTimeout(300);
  await page.screenshot({ path: out + '/palette-filtered.png' });
  await page.keyboard.press('Enter');
  await page.waitForTimeout(800);
  p = last();
  check('Enter runs "New tab" and closes the palette', p && p.layers === 0 && p.tabs === 2, JSON.stringify(p));
  const box = await page.evaluate(() => { const r = document.getElementById('canvas').getBoundingClientRect(); return { left: r.left, top: r.top, width: r.width, height: r.height }; });
  await page.mouse.click(box.left + box.width - 12, box.top + 8); // the button at the end of the strip
  await page.waitForTimeout(600);
  p = last();
  check('the strip button opens the palette', p && p.layers === 1, JSON.stringify(p));
  await page.keyboard.press('Escape');
  await page.waitForTimeout(500);
  p = last();
  check('Escape closes it', p && p.layers === 0, JSON.stringify(p));
  await prefix('Shift+;');
  await page.mouse.click(box.left + box.width - 10, box.top + box.height - 10);
  await page.waitForTimeout(500);
  p = last();
  check('a click outside closes it', p && p.layers === 0 && p.tabs === 2, JSON.stringify(p));
  await prefix('Shift+;');
  await page.keyboard.type('split side', { delay: 30 });
  const cellH = last().strip; // the strip is one row (device pixels = CSS pixels here)
  await page.mouse.click(box.left + box.width / 2 - 100, box.top + cellH * 3 + cellH / 2); // first entry: layer row 2, window row 3
  await page.waitForTimeout(800);
  p = last();
  check('a click on an entry runs it (split)', p && p.layers === 0 && p.total === 3, JSON.stringify(p));
  await page.screenshot({ path: out + '/palette-after.png' });
  await browser.close();
  if (fails.length) process.exit(1);
})().catch(e => { console.error('FAIL', e.message.split('\n')[0]); process.exit(1); });
