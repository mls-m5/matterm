// Runs the pretend system in the browser (index.html?demo): the intro, a key to
// skip it, commands, and an animation found on disk. Screenshots go next to
// out.png (-intro, -fire). No server is involved.
// usage: node e2e-demo.js URL out.png
const { chromium } = require('playwright-core');
const crypto = require('crypto');
let failures = 0;
const check = (name, ok, detail) => {
  console.log((ok ? 'PASS ' : 'FAIL ') + name + (ok ? '' : '  ' + (detail || '')));
  if (!ok) failures++;
};
(async () => {
  const [url, out] = process.argv.slice(2);
  const browser = await chromium.launch({ args: ['--use-gl=angle','--use-angle=swiftshader','--enable-unsafe-swiftshader','--ignore-gpu-blocklist','--no-sandbox'] });
  const ctx = await browser.newContext({ viewport: { width: 1000, height: 640 }, permissions: ['clipboard-read', 'clipboard-write'] });
  const page = await ctx.newPage();
  page.on('pageerror', e => console.log('pageerror:', e.message));
  const shot = async suffix => out && page.screenshot({ path: out.replace(/\.png$/, '') + suffix + '.png' });
  const canvas = async () => crypto.createHash('md5').update(await page.locator('#canvas').screenshot()).digest('hex');
  const text = async () => {
    await page.evaluate(() => document.getElementById('canvas').focus());
    await page.keyboard.press('Control+Shift+A');
    await page.keyboard.press('Control+Shift+C');
    await page.waitForTimeout(300);
    return page.evaluate(() => navigator.clipboard.readText());
  };
  await page.goto(url);
  await page.waitForTimeout(1800);
  await shot('-intro');
  const intro = await canvas();
  await page.waitForTimeout(300);
  check('the intro animates', (await canvas()) !== intro);
  await page.keyboard.press('Space'); // Any key skips it.
  await page.waitForTimeout(500);
  check('the prompt follows the intro', (await text()).includes('guest@matterm'));
  await page.keyboard.type('echo hello | wc -w\n', { delay: 30 });
  await page.waitForTimeout(800);
  const piped = await text();
  check('a pipeline runs', /\n\s+1\s*\n/.test(piped), JSON.stringify(piped.slice(-160)));
  // help is long and scrolls its first lines away; its last line is enough.
  await page.keyboard.type('help\n', { delay: 30 });
  await page.waitForTimeout(600);
  const shown = await text();
  check('help runs', shown.includes('is in your PATH.'), JSON.stringify(shown.slice(-120)));
  await page.keyboard.type('fire\n', { delay: 30 });
  await page.waitForTimeout(400);
  check('fire is not in the PATH', (await text()).includes('fire: command not found'));
  await page.keyboard.type('/usr/games/fire\n', { delay: 30 });
  await page.waitForTimeout(1200);
  const first = await canvas();
  await page.waitForTimeout(600);
  check('the fire burns', (await canvas()) !== first);
  await shot('-fire');
  await page.keyboard.press('q');
  await page.waitForTimeout(500);
  check('q returns to the prompt', (await text()).trimEnd().endsWith('$'));
  await browser.close();
  if (failures) { console.error(failures + ' check(s) failed'); process.exit(1); }
})().catch(e => { console.error('FAIL', e.message.split('\n')[0]); process.exit(1); });
