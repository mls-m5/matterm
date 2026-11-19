// Mouse reporting, paste event and unfocused cursor; writes OUTDIR/m1..m3.png.
// usage: node e2e-input.js URL PASSWORD OUTDIR
const { chromium } = require('playwright-core');
(async () => {
  const [url, pw] = process.argv.slice(2);
  const browser = await chromium.launch({ args: ['--use-gl=angle','--use-angle=swiftshader','--enable-unsafe-swiftshader','--ignore-gpu-blocklist','--no-sandbox'] });
  const ctx = await browser.newContext({ viewport: { width: 1000, height: 640 }, permissions: ['clipboard-read','clipboard-write'] });
  const page = await ctx.newPage();
  page.on('console', m => console.log('console:', m.text()));
  page.on('pageerror', e => console.log('pageerror:', e.message));
  await page.goto(url);
  await page.fill('#password', pw);
  await page.click('button');
  await page.waitForTimeout(3000);
  await page.keyboard.type("printf '\\033[?1000h\\033[?1006h'; cat -v\n", { delay: 20 });
  await page.waitForTimeout(800);
  await page.mouse.click(200, 300);
  await page.mouse.wheel(0, -100);
  await page.waitForTimeout(800);
  await page.screenshot({ path: process.argv[4] + '/m1.png' });
  await page.keyboard.press('Control+c');
  await page.keyboard.type("printf '\\033[?1000l'; echo\n", { delay: 20 });
  await page.waitForTimeout(500);
  await page.evaluate(() => navigator.clipboard.writeText('pasted-text-123'));
  await page.keyboard.type('echo ', { delay: 20 });
  await page.evaluate(() => { const dt = new DataTransfer(); dt.setData('text/plain', 'pasted-text-123'); document.dispatchEvent(new ClipboardEvent('paste', { clipboardData: dt, bubbles: true, cancelable: true })); });
  await page.keyboard.press('Enter');
  await page.waitForTimeout(800);
  await page.screenshot({ path: process.argv[4] + '/m2.png' });
  await page.evaluate(() => { document.getElementById('canvas').blur(); window.dispatchEvent(new Event('blur')); });
  await page.waitForTimeout(800);
  await page.screenshot({ path: process.argv[4] + '/m3.png' });
  await browser.close();
})().catch(e => { console.error('FAIL', e.message.split('\n')[0]); process.exit(1); });
