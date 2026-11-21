// Layout behavior in server mode, checked through the ?debug plan lines:
// split, divider drag, new tab, strip clicks, and restore after reload + login.
// usage: node e2e-tabs-layout.js URL PASSWORD OUTDIR   (URL without ?debug)
const { chromium } = require('playwright-core');
const fails = [];
function check(name, ok, extra) {
  console.log((ok ? 'PASS ' : 'FAIL ') + name + (ok ? '' : ' ' + (extra || '')));
  if (!ok) fails.push(name);
}
const parse = (plan) => {
  const m = /plan tabs=(\d+) panes=(\d+) strip=(\d+) focus=(\d+)/.exec(plan || '');
  const panes = [...(plan || '').matchAll(/\[pane (\d+) (\d+),(\d+) (\d+)x(\d+)/g)].map(a => ({ id: +a[1], x: +a[2], y: +a[3], w: +a[4], h: +a[5] }));
  return m ? { tabs: +m[1], total: +m[2], strip: +m[3], focus: +m[4], panes } : null;
};
(async () => {
  const [url, pw, out] = process.argv.slice(2);
  const browser = await chromium.launch({ args: ['--use-gl=angle','--use-angle=swiftshader','--enable-unsafe-swiftshader','--ignore-gpu-blocklist','--no-sandbox'] });
  const page = await (await browser.newContext({ viewport: { width: 1000, height: 640 } })).newPage();
  const plans = [];
  page.on('console', m => { if (m.text().includes('plan tabs')) plans.push(m.text()); });
  page.on('pageerror', e => check('no page error', false, e.message));
  const last = () => parse(plans[plans.length - 1]);
  const login = async () => { await page.fill('#password', pw); await page.click('button'); await page.waitForTimeout(3500); };
  await page.goto(url + '?debug'); await login();
  const prefix = async (k) => { await page.keyboard.press('Control+b'); await page.waitForTimeout(150); await page.keyboard.press(k); await page.waitForTimeout(700); };
  await prefix('Shift+5');
  let p = last();
  check('split side by side gives two panes', p && p.panes.length === 2 && p.panes[0].x < p.panes[1].x, JSON.stringify(p));
  const w0 = p ? p.panes[0].w : 0;
  const gx = 43 + p.panes[1].x - 6; // the canvas starts 43 px into the 1000 px page
  await page.mouse.move(gx, 300); await page.mouse.down(); await page.mouse.move(gx + 100, 300, { steps: 8 }); await page.mouse.up();
  await page.waitForTimeout(700);
  p = last();
  check('divider drag widens the left pane', p && p.panes[0].w > w0 + 50, JSON.stringify(p));
  const dragged = p ? p.panes[0].w : 0;
  await prefix('c');
  p = last();
  check('Ctrl+B c: second tab, strip shown', p && p.tabs === 2 && p.strip > 0, JSON.stringify(p));
  await page.mouse.click(90, 56); await page.waitForTimeout(800);
  p = last();
  check('click on Tab 1 shows its two panes', p && p.panes.length === 2, JSON.stringify(p));
  await page.reload(); await page.waitForTimeout(1500);
  const mark = plans.length;
  await login();
  p = last();
  check('layout restored after reload and login (2 tabs, dragged width)', p && plans.length > mark && p.tabs === 2 && p.panes.length === 2 && Math.abs(p.panes[0].w - dragged) < 30, JSON.stringify(p));
  await page.screenshot({ path: out + '/layout-reload.png' });
  await browser.close();
  if (fails.length) process.exit(1);
})().catch(e => { console.error('FAIL', e.message.split('\n')[0]); process.exit(1); });
