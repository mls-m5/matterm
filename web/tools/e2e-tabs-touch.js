// Tabs and panes by touch (needs a persistent-sessions server for nothing but
// reuse of the server setup; any server works): the ^B key of the extra-keys
// bar arms the prefix, the soft keyboard types the second key, strip taps
// switch tabs, a narrow window shows one pane at a time, and a finger drags a
// divider. usage: node e2e-tabs-touch.js URL PASSWORD OUTDIR   (URL without ?debug)
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
const args = ['--use-gl=angle','--use-angle=swiftshader','--enable-unsafe-swiftshader','--ignore-gpu-blocklist','--no-sandbox'];
async function open(browser, url, pw, options) {
  const ctx = await browser.newContext(options);
  const page = await ctx.newPage();
  const plans = [];
  page.on('console', m => { if (m.text().includes('plan tabs')) plans.push(m.text()); });
  page.on('pageerror', e => check('no page error', false, e.message));
  await page.goto(url);
  await page.fill('#password', pw);
  await page.tap('button[type=submit]');
  await page.waitForTimeout(3500);
  const last = () => parse(plans[plans.length - 1]);
  const input = (type, data) => page.evaluate(([t, d]) => document.getElementById('softkeys').dispatchEvent(new InputEvent('beforeinput', { inputType: t, data: d, cancelable: true, bubbles: true })), [type, data]);
  const prefix = async (text) => { await page.tap('#keybar button[data-prefix]'); await page.waitForTimeout(150); await input('insertText', text); await page.waitForTimeout(700); };
  const canvas = () => page.evaluate(() => { const r = document.getElementById('canvas').getBoundingClientRect(); return { left: r.left, top: r.top, width: r.width, height: r.height }; });
  return { ctx, page, plans, last, input, prefix, canvas };
}
(async () => {
  const [url, pw, out] = process.argv.slice(2);
  const browser = await chromium.launch({ args });

  // A phone: 400 px wide.
  let t = await open(browser, url + '?debug', pw, { viewport: { width: 400, height: 700 }, deviceScaleFactor: 2, isMobile: true, hasTouch: true });
  check('the ^B button is shown in the key bar', await t.page.evaluate(() => { const b = document.querySelector('#keybar [data-prefix]'); return !!b && !b.hidden; }));
  await t.page.tap('#canvas', { position: { x: 200, y: 300 } });
  await t.page.waitForTimeout(300);
  const firstId = t.last() ? t.last().panes[0].id : 0; // the pane of Tab 1
  await t.prefix('c');
  let p = t.last();
  check('^B then c on the soft keyboard opens a second tab', p && p.tabs === 2 && p.strip > 0, JSON.stringify(p));
  const first = firstId;
  await t.prefix('"');
  p = t.last();
  check('^B then " splits; a 400 px window shows only the focused pane', p && p.total === 3 && p.panes.length === 1 && p.panes[0].w >= 700, JSON.stringify(p));
  const c = await t.canvas();
  await t.page.touchscreen.tap(c.left + 20, c.top + 6); // Tab 1 in the strip
  await t.page.waitForTimeout(800);
  p = t.last();
  check('a tap on Tab 1 in the strip shows its pane', p && p.panes.length === 1 && p.panes[0].id === first, JSON.stringify(p));
  await t.page.screenshot({ path: out + '/touch-narrow.png' });
  await t.ctx.close();

  // A wide touch screen: the divider drags with a finger.
  t = await open(browser, url + '?debug&touch', pw, { viewport: { width: 1000, height: 700 }, hasTouch: true });
  await t.page.tap('#canvas', { position: { x: 300, y: 300 } });
  await t.page.waitForTimeout(300);
  await t.prefix('%');
  p = t.last();
  check('^B then % splits side by side', p && p.panes.length === 2, JSON.stringify(p));
  const w0 = p ? p.panes[0].w : 0;
  const box = await t.canvas();
  const gx = box.left + p.panes[1].x - 6 - 12; // 12 px left of the gutter's middle: within the finger's reach
  const cdp = await t.ctx.newCDPSession(t.page);
  const touch = (type, x) => cdp.send('Input.dispatchTouchEvent', { type, touchPoints: type === 'touchEnd' ? [] : [{ x, y: box.top + 300 }] });
  await touch('touchStart', gx);
  for (let i = 1; i <= 8; i++) { await touch('touchMove', gx + i * 12); await t.page.waitForTimeout(40); }
  await touch('touchEnd', gx + 96);
  await t.page.waitForTimeout(800);
  p = t.last();
  check('a finger near the divider drags it', p && p.panes[0].w > w0 + 50, JSON.stringify(p));
  await t.page.screenshot({ path: out + '/touch-divider.png' });
  await t.ctx.close();
  await browser.close();
  if (fails.length) process.exit(1);
})().catch(e => { console.error('FAIL', e.message.split('\n')[0]); process.exit(1); });
