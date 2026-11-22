// Working-tab pulse: a tab whose terminal prints keeps alternating its colour once a second;
// when it falls silent the strip comes to rest; ?pulse=0 switches it off.
// usage: node e2e-pulse.js URL PASSWORD OUTDIR   (URL without query)
const { chromium } = require('playwright-core');
const crypto = require('crypto');
const fails = [];
function check(name, ok, extra) {
  console.log((ok ? 'PASS ' : 'FAIL ') + name + (ok ? '' : ' ' + (extra || '')));
  if (!ok) fails.push(name);
}
const hash = (b) => crypto.createHash('sha1').update(b).digest('hex');
async function open(browser, url, pw) {
  const ctx = await browser.newContext({ viewport: { width: 1000, height: 640 } });
  const page = await ctx.newPage();
  page.on('pageerror', (e) => check('no page error', false, e.message));
  await page.goto(url);
  await page.fill('#password', pw);
  await page.click('button');
  await page.waitForTimeout(3500);
  return page;
}
// Prints every 200 ms in the first tab, then opens a second tab (the first is hidden and works).
async function setup(page) {
  await page.keyboard.type('clear; for i in $(seq 1 70); do echo working $i; sleep 0.2; done\n', { delay: 10 });
  await page.waitForTimeout(500);
  await page.keyboard.press('Control+b');
  await page.waitForTimeout(150);
  await page.keyboard.press('c');
  await page.waitForTimeout(800);
  await page.waitForTimeout(5500); // output within 5 s of our own typing is ignored; the loop prints for 14 s
}
// The strip, a picture a little apart in time; hashes of the first tab's label area.
const strip = { x: 43, y: 44, width: 260, height: 24 }; // the page frames the terminal: the strip starts at (43, 44)
async function samples(page, n, gap, out, name) {
  const hs = [];
  for (let i = 0; i < n; i++) {
    const b = await page.screenshot({ clip: strip, path: i < 2 ? `${out}/${name}-${i}.png` : undefined });
    hs.push(hash(b));
    await page.waitForTimeout(gap);
  }
  return hs;
}
(async () => {
  const [url, pw, out] = process.argv.slice(2);
  const browser = await chromium.launch({ args: ['--use-gl=angle','--use-angle=swiftshader','--enable-unsafe-swiftshader','--ignore-gpu-blocklist','--no-sandbox'] });
  const page = await open(browser, url, pw);
  await setup(page);
  const working = await samples(page, 4, 1000, out, 'pulse-working'); // ~3 s apart: both phases
  check('a working tab alternates its colour', new Set(working).size >= 2, JSON.stringify(working));
  await page.waitForTimeout(9000); // the loop ends 14 s after typing; 3 s later nothing works
  const calm = await samples(page, 3, 1100, out, 'pulse-calm');
  check('after silence the strip rests', new Set(calm).size === 1, JSON.stringify(calm));
  const off = await open(browser, url + '?pulse=0', pw);
  await setup(off);
  const none = await samples(off, 4, 1000, out, 'pulse-off');
  check('?pulse=0: no alternating', new Set(none).size === 1, JSON.stringify(none));
  await browser.close();
  console.log(fails.length ? `${fails.length} failed` : 'all passed');
  process.exit(fails.length ? 1 : 0);
})().catch((e) => { console.error(e); process.exit(1); });
