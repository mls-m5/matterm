// Chat in the static demo against a local chatd (not part of run-e2e.sh):
//   chatd --origin http://127.0.0.1:8080 --listen 127.0.0.1:8097 &
//   (cd <copy of build-web/site with demo.html as index.html> && python3 -m http.server 8080 --bind 127.0.0.1 &)
//   node e2e-chat.js 'http://127.0.0.1:8080/?chat=ws://127.0.0.1:8097' outdir   (look at chat1.png, chat2.png)
const { chromium } = require('playwright-core');
(async () => {
  const browser = await chromium.launch({ args: ['--use-gl=angle','--use-angle=swiftshader','--enable-unsafe-swiftshader','--ignore-gpu-blocklist','--no-sandbox'] });
  const ctx = await browser.newContext({ viewport: { width: 1000, height: 700 } });
  const page = await ctx.newPage();
  page.on('pageerror', e => console.log('pageerror:', e.message));
  page.on('console', m => { if (m.type() === 'error') console.log('console error:', m.text()); });
  await page.goto(process.argv[2]);
  await page.waitForTimeout(9000);
  await page.click('#canvas');
  await page.keyboard.type('chat', { delay: 80 });
  await page.keyboard.press('Enter');
  await page.waitForTimeout(3000);
  await page.screenshot({ path: process.argv[3] + '/chat1.png' });
  await page.keyboard.type('y', { delay: 80 });
  await page.waitForTimeout(3000);
  await page.keyboard.type('hello from the browser', { delay: 60 });
  await page.waitForTimeout(2000);
  await page.keyboard.press('Enter');
  await page.waitForTimeout(2500);
  await page.screenshot({ path: process.argv[3] + '/chat2.png' });
  await browser.close();
})().catch(e => { console.error('FAIL', e.message.split('\n')[0]); process.exit(1); });
