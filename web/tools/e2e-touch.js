// Touch and soft-keyboard emulation: tap opens the keyboard field, text, the
// key bar, latched Ctrl, backspace. usage: node e2e-touch.js URL out.png
// (password: the one run-e2e.sh uses; pass as argv[4], default below)
const { chromium } = require('playwright-core');
(async () => {
  const browser = await chromium.launch({ args: ['--use-gl=angle','--use-angle=swiftshader','--enable-unsafe-swiftshader','--ignore-gpu-blocklist','--no-sandbox'] });
  const ctx = await browser.newContext({ viewport: { width: 400, height: 700 }, deviceScaleFactor: 2, isMobile: true, hasTouch: true });
  const page = await ctx.newPage();
  page.on('pageerror', e => console.log('pageerror:', e.message));
  await page.goto(process.argv[2]);
  await page.fill('#password', (process.argv[4] || 'end to end test passphrase 0123456789'));
  await page.tap('button[type=submit]');
  await page.waitForTimeout(3500);
  console.log('keybar visible:', await page.evaluate(() => !document.getElementById('keybar').hidden));
  await page.tap('#canvas', { position: { x: 200, y: 300 } });
  await page.waitForTimeout(300);
  console.log('focused:', await page.evaluate(() => document.activeElement && document.activeElement.id));
  const input = (type, data) => page.evaluate(([t, d]) => document.getElementById('softkeys').dispatchEvent(new InputEvent('beforeinput', { inputType: t, data: d, cancelable: true, bubbles: true })), [type, data]);
  await input('insertText', 'echo soft-keyboard');
  await page.tap('#keybar button[data-text="|"]');
  await input('insertText', 'tr a-z A-Z');
  await input('insertLineBreak', null);
  await page.waitForTimeout(800);
  await input('insertText', 'sleep 30');
  await input('insertLineBreak', null);
  await page.waitForTimeout(300);
  await page.tap('#keybar button[data-mod=ctrl]');
  await input('insertText', 'c');            // Ctrl latched + c = interrupt
  await page.waitForTimeout(500);
  await input('insertText', 'ab');
  await input('deleteContentBackward', null); // backspace removes b
  await page.tap('#keybar button[data-key=ArrowLeft]');
  await input('insertText', 'X');
  await page.waitForTimeout(500);
  console.log('canvas', JSON.stringify(await page.evaluate(() => { const c = document.getElementById('canvas'); return [c.width, c.height, c.clientWidth, c.clientHeight]; })));
  await page.screenshot({ path: process.argv[3] });
  await browser.close();
})().catch(e => { console.error('FAIL', e.message.split('\n')[0]); process.exit(1); });
