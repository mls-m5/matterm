// Ctrl+click opens an http(s) link in a new tab; plain click and non-http
// schemes do not. usage: node e2e-link.js URL PASSWORD
const { chromium } = require('playwright-core');
(async () => {
  const [url, pw] = process.argv.slice(2);
  const browser = await chromium.launch({ args: ['--use-gl=angle','--use-angle=swiftshader','--enable-unsafe-swiftshader','--ignore-gpu-blocklist','--no-sandbox'] });
  const ctx = await browser.newContext({ viewport: { width: 1000, height: 640 } });
  const page = await ctx.newPage();
  await ctx.addInitScript(() => { window.__opens = []; window.open = (...a) => { window.__opens.push(a); return null; }; });
  await page.goto(url);
  await page.fill('#password', pw);
  await page.click('button');
  await page.waitForTimeout(3000);
  await page.keyboard.type("clear; echo see http://localhost:1/docs?a=1 now; echo js \'javascript:alert(1)\'\n", { delay: 10 });
  await page.waitForTimeout(800);
  // Row 0, the link starts after "see " (column 4); cells are ~12 px wide.
  await page.mouse.move(12 * 8, 12, { steps: 2 });
  await page.keyboard.down('Control');
  await page.mouse.move(12 * 8 + 1, 12, { steps: 3 });   // hover is evaluated on motion
  await page.waitForFunction(() => document.title.startsWith('http://localhost:1/'), null, { timeout: 5000 })
    .catch(() => {});
  console.log('hover title:', await page.title());
  await page.mouse.click(12 * 8 + 1, 12);
  await page.keyboard.up('Control');
  await page.waitForTimeout(1500);
  console.log('window.open calls after ctrl+click:', JSON.stringify(await page.evaluate(() => window.__opens)));
  await page.mouse.click(12 * 8, 12);              // plain click
  await page.waitForTimeout(500);
  console.log('after plain click:', (await page.evaluate(() => window.__opens)).length);
  await page.keyboard.down('Control');
  await page.mouse.click(12 * 8, 36);              // the javascript: row
  await page.keyboard.up('Control');
  await page.waitForTimeout(500);
  console.log('after javascript: row:', (await page.evaluate(() => window.__opens)).length);
  await browser.close();
})().catch(e => { console.error('FAIL', e.message.split('\n')[0]); process.exit(1); });
