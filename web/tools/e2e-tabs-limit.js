// Pane limit and session cleanup (needs a persistent-sessions server): 16 panes
// are allowed, the 17th is refused with a notice, and closed panes free their
// server sessions (a Close is sent), so new ones connect again.
// usage: node e2e-tabs-limit.js URL PASSWORD OUTDIR   (URL without ?debug)
const { chromium } = require('playwright-core');
const fails = [];
function check(name, ok, extra) {
  console.log((ok ? 'PASS ' : 'FAIL ') + name + (ok ? '' : ' ' + (extra || '')));
  if (!ok) fails.push(name);
}
const parse = (plan) => {
  const m = /plan tabs=(\d+) panes=(\d+) strip=(\d+) focus=(\d+)/.exec(plan || '');
  const panes = [...(plan || '').matchAll(/\[pane (\d+) [^\]]*state=(\d+)\]/g)].map(a => ({ id: +a[1], state: +a[2] }));
  return m ? { tabs: +m[1], total: +m[2], focus: +m[4], panes } : null;
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
  await page.waitForTimeout(3000);
  const prefix = async (k, wait = 350) => { await page.keyboard.press('Control+b'); await page.waitForTimeout(100); await page.keyboard.press(k); await page.waitForTimeout(wait); };
  const live = () => { const p = last(); return p && p.panes.length > 0 && p.panes.every(x => x.state === 0); };
  for (let i = 0; i < 15; i++) await prefix('c');
  await page.waitForTimeout(1500);
  let p = last();
  check('16 tabs (one pane each) are allowed and live', p && p.tabs === 16 && live(), JSON.stringify(p));
  await prefix('c', 800);
  p = last();
  check('the 17th terminal is refused', p && p.tabs === 16, JSON.stringify(p));
  await page.screenshot({ path: out + '/limit-refused.png' });
  // Close them all but one, then open 15 again: leaked sessions would hit the
  // server's limits and the new panes would be refused ("disconnected").
  for (let i = 0; i < 15; i++) await prefix('x', 250);
  await page.waitForTimeout(2000);
  p = last();
  check('closed down to one tab', p && p.tabs === 1, JSON.stringify(p));
  for (let i = 0; i < 15; i++) await prefix('c');
  await page.waitForTimeout(2500);
  p = last();
  check('after closing, 16 live panes again (sessions were freed)', p && p.tabs === 16 && live(), JSON.stringify(p));
  await page.screenshot({ path: out + '/limit-again.png' });
  await browser.close();
  if (fails.length) process.exit(1);
})().catch(e => { console.error('FAIL', e.message.split('\n')[0]); process.exit(1); });
