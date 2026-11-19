// Runs the in-browser simulator (index.html?demo) and screenshots it.
// usage: node e2e-demo.js URL out.png
const { chromium } = require('playwright-core');
(async () => {
  const browser = await chromium.launch({ args: ['--use-gl=angle','--use-angle=swiftshader','--enable-unsafe-swiftshader','--ignore-gpu-blocklist','--no-sandbox'] });
  const page = await (await browser.newContext({ viewport: { width: 1000, height: 640 } })).newPage();
  page.on('pageerror', e => console.log('pageerror:', e.message));
  await page.goto(process.argv[2]);
  await page.waitForTimeout(3000);
  await page.keyboard.type('help\ncolors\n', { delay: 30 });
  await page.waitForTimeout(1500);
  await page.screenshot({ path: process.argv[3] });
  await browser.close();
})().catch(e => { console.error('FAIL', e.message.split('\n')[0]); process.exit(1); });
