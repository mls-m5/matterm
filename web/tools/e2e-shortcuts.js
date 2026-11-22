// Keyboard shortcut help, which-key box and the palette's key column in the browser.
// Screenshots go to OUTDIR (shortcuts-palette.png, shortcuts-whichkey.png, shortcuts-help.png) so
// that they can be looked at; the checks compare pictures and that no key reaches the shell.
// usage: node e2e-shortcuts.js URL PASSWORD OUTDIR
const { chromium } = require('playwright-core');
const fs = require('fs');
const os = require('os');
const path = require('path');
const crypto = require('crypto');
const fails = [];
function check(name, ok, extra) {
  console.log((ok ? 'PASS ' : 'FAIL ') + name + (ok ? '' : ' ' + (extra || '')));
  if (!ok) fails.push(name);
}
const hash = (buf) => crypto.createHash('sha1').update(buf).digest('hex');
(async () => {
  const [url, pw, out] = process.argv.slice(2);
  const browser = await chromium.launch({ args: ['--use-gl=angle','--use-angle=swiftshader','--enable-unsafe-swiftshader','--ignore-gpu-blocklist','--no-sandbox'] });
  const ctx = await browser.newContext({ viewport: { width: 1000, height: 640 } });
  const page = await ctx.newPage();
  page.on('pageerror', (e) => check('no page error', false, e.message));
  await page.goto(url);
  await page.fill('#password', pw);
  await page.click('button');
  await page.waitForTimeout(3500);
  const marker = path.join(os.tmpdir(), `matterm-shortcuts-${process.pid}-${Date.now()}`);
  // The bottom right corner only: the text cursor blinks at the top left and the shell may redraw
  // there, but the which-key box lives in this corner.
  const corner = { x: 640, y: 330, width: 360, height: 270 };
  const shot = async (name, whole) => {
    const b = await page.screenshot({ path: `${out}/${name}.png`, ...(whole ? {} : { clip: corner }) });
    return hash(b);
  };
  const plain = await shot('shortcuts-plain');
  const plainWhole = await shot('shortcuts-plain-whole', true);
  // The palette lists commands with their keys.
  await page.keyboard.press('Control+b');
  await page.waitForTimeout(150);
  await page.keyboard.press(':');
  await page.waitForTimeout(500);
  const palette = await shot('shortcuts-palette', true);
  check('the palette opens', palette !== plainWhole);
  await page.keyboard.press('Escape');
  await page.waitForTimeout(300);
  // Which-key: the prefix alone, held for a moment.
  await page.keyboard.press('Control+b');
  await page.waitForTimeout(100);
  const early = await shot('shortcuts-early');
  await page.waitForTimeout(700);
  const whichKey = await shot('shortcuts-whichkey');
  check('no box right after the prefix', early === plain);
  check('the which-key box appears after about half a second', whichKey !== plain);
  await page.keyboard.press('Escape'); // unbound: the prefix and the key go to the shell, harmlessly
  await page.waitForTimeout(2500);
  await page.keyboard.press('Control+u');
  // Ctrl+B ? opens the help view; it takes the keys; Escape closes.
  await page.keyboard.press('Control+b');
  await page.waitForTimeout(150);
  await page.keyboard.type('?', { delay: 20 });
  await page.waitForTimeout(500);
  const help = await shot('shortcuts-help', true);
  check('Ctrl+B ? opens the shortcut view', help !== plainWhole);
  await page.keyboard.press('PageDown');
  await page.waitForTimeout(300);
  const scrolled = await shot('shortcuts-help-scrolled', true);
  check('PageDown scrolls the list', scrolled !== help);
  await page.keyboard.type(`touch ${marker}`, { delay: 15 }); // goes to the view, not the shell
  await page.keyboard.press('Escape');
  await page.waitForTimeout(400);
  await page.keyboard.press('Enter');
  await page.waitForTimeout(800);
  check('the view took the keys', !fs.existsSync(marker));
  fs.rmSync(marker, { force: true });
  await browser.close();
  console.log(fails.length ? `${fails.length} failed` : 'all passed');
  process.exit(fails.length ? 1 : 0);
})().catch((e) => { console.error(e); process.exit(1); });
