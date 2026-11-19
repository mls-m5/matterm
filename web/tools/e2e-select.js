// Double click selects a word, triple click a line, shift+click extends, and
// Ctrl+Shift+A selects everything. usage: node e2e-select.js URL PASSWORD
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
  await page.keyboard.type('clear; echo alpha beta gamma\n', { delay: 15 });
  await page.waitForTimeout(800);
  const copied = async () => {
    await page.keyboard.press('Control+Shift+C');
    await page.waitForTimeout(300);
    return JSON.stringify((await page.evaluate(() => navigator.clipboard.readText())).trimEnd());
  };
  // Row 0 is "alpha beta gamma"; cells are about 12 px wide, 24 px tall.
  await page.mouse.dblclick(12 * 7, 12);               // inside "beta"
  console.log('word  :', await copied());
  await page.mouse.click(12 * 3, 12, { clickCount: 3 });
  console.log('line  :', await copied());
  await page.mouse.click(2, 12);                       // plain click, then
  await page.keyboard.down('Shift');
  await page.mouse.click(12 * 5, 12);                  // shift+click extends
  await page.keyboard.up('Shift');
  console.log('extend:', await copied());
  await page.keyboard.press('Control+Shift+A');
  const all = await copied();
  console.log('all   :', all.includes('alpha beta gamma') ? 'contains the output' : all);
  await browser.close();
})().catch(e => { console.error('FAIL', e.message.split('\n')[0]); process.exit(1); });
