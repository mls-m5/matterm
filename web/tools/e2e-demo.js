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
  await page.keyboard.press('Space'); // Any key skips it: the text goes, the background glides.
  await page.waitForTimeout(1700);
  check('the prompt follows the intro', (await text()).includes('guest@matterm'));
  await page.keyboard.type('echo hello | wc -w\n', { delay: 30 });
  await page.waitForTimeout(800);
  const piped = await text();
  check('a pipeline runs', /\n\s+1\s*\n/.test(piped), JSON.stringify(piped.slice(-160)));
  // help is long and scrolls its first lines away; its last line is enough.
  await page.keyboard.type('help\n', { delay: 30 });
  await page.waitForTimeout(2200); // The output is revealed at a pace.
  const shown = await text();
  check('help runs', shown.includes('is in your PATH.'), JSON.stringify(shown.slice(-120)));
  await page.keyboard.type('neofetch\n', { delay: 30 });
  await page.waitForTimeout(1200);
  const info = await text();
  check('neofetch names the system', info.includes('Mattrix OS 6.0 Professional Edition') && info.includes('MATTRIX'),
    JSON.stringify(info.slice(-200)));
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
  // ?embed: edge to edge for a page inside an iframe, with a start command and
  // no intro or reveal.
  const embedded = await (await browser.newContext({ viewport: { width: 700, height: 420 }, permissions: ['clipboard-read', 'clipboard-write'] })).newPage();
  embedded.on('pageerror', e => console.log('pageerror:', e.message));
  const base = url.split('?')[0];
  await embedded.goto(base + '?demo&embed&nointro&noreveal&theme=light&cmd=' + encodeURIComponent('uname -a'));
  await embedded.waitForTimeout(2500);
  const box = await embedded.evaluate(() => { const r = document.getElementById('canvas').getBoundingClientRect(); return [r.left, r.top, r.width, r.height, innerWidth, innerHeight]; });
  check('?embed fills the page', box[0] === 0 && box[1] === 0 && box[2] === box[4] && box[3] === box[5], JSON.stringify(box));
  await embedded.evaluate(() => document.getElementById('canvas').focus());
  await embedded.keyboard.press('Control+Shift+A');
  await embedded.keyboard.press('Control+Shift+C');
  await embedded.waitForTimeout(300);
  const ran = await embedded.evaluate(() => navigator.clipboard.readText());
  check('?cmd runs at the first prompt', ran.includes('uname -a') && ran.includes('6.8.0-mattrix'), JSON.stringify(ran.slice(0, 200)));
  check('?theme sets the page scheme', await embedded.evaluate(() => document.documentElement.dataset.scheme) === 'light');
  await browser.close();
  if (failures) { console.error(failures + ' check(s) failed'); process.exit(1); }
})().catch(e => { console.error('FAIL', e.message.split('\n')[0]); process.exit(1); });
