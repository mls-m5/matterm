// End-to-end check in headless Chromium: log in, type a command, screenshot.
// usage: node e2e.js URL PASSWORD out.png  (needs playwright-core, see web/README.md)
const { chromium } = require('playwright-core');
(async () => {
  const [url, pw, out] = process.argv.slice(2);
  const browser = await chromium.launch({ args: ['--use-gl=angle','--use-angle=swiftshader','--enable-unsafe-swiftshader','--ignore-gpu-blocklist','--no-sandbox'] });
  const page = await browser.newPage({ viewport: { width: 1000, height: 640 } });
  page.on('console', m => console.log('console:', m.text()));
  page.on('pageerror', e => console.log('pageerror:', e.message));
  page.on('requestfailed', r => console.log('requestfailed:', r.url(), r.failure().errorText));
  await page.goto(url);
  await page.screenshot({ path: out.replace('.png', '-login.png') });
  await page.fill('#password', pw);
  await page.click('button');
  await page.waitForTimeout(4000);
  await page.keyboard.type('echo hello-from-the-browser; ls --color=always /\n', { delay: 30 });
  await page.waitForTimeout(2000);
  await page.screenshot({ path: out });
  await browser.close();
})().catch(e => { console.error('FAIL', e.message.split('\n')[0]); process.exit(1); });
