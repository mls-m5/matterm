// Copy / scroll mode in the browser: Ctrl+B [ enters it, the keys move a virtual cursor and
// select, y copies, nothing reaches the shell meanwhile, Esc leaves.
// usage: node e2e-copymode.js URL PASSWORD OUTDIR
const { chromium } = require('playwright-core');
const fs = require('fs');
const os = require('os');
const path = require('path');
const fails = [];
function check(name, ok, extra) {
  console.log((ok ? 'PASS ' : 'FAIL ') + name + (ok ? '' : ' ' + (extra || '')));
  if (!ok) fails.push(name);
}
(async () => {
  const [url, pw, out] = process.argv.slice(2);
  const browser = await chromium.launch({ args: ['--use-gl=angle','--use-angle=swiftshader','--enable-unsafe-swiftshader','--ignore-gpu-blocklist','--no-sandbox'] });
  const ctx = await browser.newContext({ viewport: { width: 1000, height: 640 }, permissions: ['clipboard-read', 'clipboard-write'] });
  const page = await ctx.newPage();
  page.on('pageerror', (e) => check('no page error', false, e.message));
  await page.goto(url);
  await page.fill('#password', pw);
  await page.click('button');
  await page.waitForTimeout(3500);
  const marker = path.join(os.tmpdir(), `matterm-copymode-${process.pid}-${Date.now()}`);
  await page.keyboard.type('clear; echo first-line-marker; seq 1 200\n', { delay: 15 });
  await page.waitForTimeout(1500);
  const prefix = async (k) => { await page.keyboard.press('Control+b'); await page.waitForTimeout(150); await page.keyboard.press(k); await page.waitForTimeout(500); };
  await prefix('[');
  await page.screenshot({ path: out + '/copymode-enter.png' });
  // Typed text must not reach the shell while the mode is on.
  await page.keyboard.type(`zzz`, { delay: 20 });
  await page.keyboard.press('g'); // oldest line: "first-line-marker" (or what is left of it)
  await page.waitForTimeout(300);
  await page.keyboard.type('v$', { delay: 30 }); // select to the end of the line
  await page.waitForTimeout(300);
  await page.screenshot({ path: out + '/copymode-select.png' });
  await page.keyboard.press('y');
  await page.waitForTimeout(500);
  const clip = await page.evaluate(() => navigator.clipboard.readText());
  check('y copies the selected text', /^\S+$/.test(clip.trim()) && clip.trim().length > 0, JSON.stringify(clip));
  // The mode is over: typing reaches the shell again, and "zzz" never did.
  await page.keyboard.type(`touch ${marker}\n`, { delay: 15 });
  await page.waitForTimeout(1200);
  check('after y the shell gets keys again', fs.existsSync(marker));
  // Search: "/" finds the marker line, "v$" selects to its end, y copies it.
  await prefix('[');
  await page.keyboard.type('/line-marker', { delay: 25 });
  await page.keyboard.press('Enter');
  await page.waitForTimeout(300);
  await page.keyboard.type('bve', { delay: 30 }); // back to the word start, select, to its end
  await page.keyboard.press('y');
  await page.waitForTimeout(500);
  const found = await page.evaluate(() => navigator.clipboard.readText());
  check('search finds text in the history and y copies from there', found.includes('marker') || found.includes('line'), JSON.stringify(found));
  // Esc leaves too, and h/j/k/l are not typed into the shell.
  await prefix('[');
  await page.keyboard.type('kkhl', { delay: 20 });
  await page.keyboard.press('Escape');
  await page.waitForTimeout(300);
  await page.keyboard.type(`rm -f ${marker}\n`, { delay: 15 });
  await page.waitForTimeout(1000);
  check('Esc leaves; the earlier keys were not typed into the shell', !fs.existsSync(marker));
  try { fs.unlinkSync(marker); } catch (e) {}
  await browser.close();
  if (fails.length) process.exit(1);
})().catch((e) => { console.error('FAIL', e.message.split('\n')[0]); process.exit(1); });
