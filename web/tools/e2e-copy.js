// Selection copy (Ctrl+Shift+C) reaches the clipboard. usage: node e2e-copy.js URL PASSWORD
const { chromium } = require('playwright-core');
(async () => {
  const [url, pw] = process.argv.slice(2);
  const browser = await chromium.launch({ args: ['--use-gl=angle','--use-angle=swiftshader','--enable-unsafe-swiftshader','--ignore-gpu-blocklist','--no-sandbox'] });
  const ctx = await browser.newContext({ viewport: { width: 1000, height: 640 }, permissions: ['clipboard-read','clipboard-write'] });
  const page = await ctx.newPage();
  await page.goto(url);
  await page.fill('#password', pw);
  await page.click('button');
  await page.waitForTimeout(3000);
  await page.keyboard.type("clear; echo copy-me-please\n", { delay: 20 });
  await page.waitForTimeout(800);
  await page.mouse.move(2, 12); await page.mouse.down(); await page.mouse.move(400, 12, {steps: 5}); await page.mouse.up();
  await page.keyboard.press('Control+Shift+C');
  await page.waitForTimeout(500);
  console.log('clipboard:', JSON.stringify(await page.evaluate(() => navigator.clipboard.readText())));
  await browser.close();
})().catch(e => { console.error('FAIL', e.message.split('\n')[0]); process.exit(1); });
