// Exit shows a message, Enter returns to the login form, and a second login
// works. usage: node e2e-lifecycle.js URL PASSWORD OUTDIR
const { chromium } = require('playwright-core');
(async () => {
  const [url, pw, out] = process.argv.slice(2);
  const browser = await chromium.launch({ args: ['--use-gl=angle','--use-angle=swiftshader','--enable-unsafe-swiftshader','--ignore-gpu-blocklist','--no-sandbox'] });
  const page = await (await browser.newContext({ viewport: { width: 1000, height: 640 } })).newPage();
  await page.goto(url);
  await page.fill('#password', pw);
  await page.click('button');
  await page.waitForTimeout(3000);
  await page.keyboard.type('exit 3\n', { delay: 20 });
  await page.waitForTimeout(1500);
  await page.screenshot({ path: out + '/l1.png' });
  await page.keyboard.press('Enter');
  await page.waitForSelector('#password', { state: 'visible', timeout: 5000 });
  console.log('login form is back');
  await page.fill('#password', pw);
  await page.click('button');
  await page.waitForTimeout(3000);
  await page.keyboard.type('echo second-session-ok\n', { delay: 20 });
  await page.waitForTimeout(1000);
  await page.screenshot({ path: out + '/l2.png' });
  await browser.close();
})().catch(e => { console.error('FAIL', e.message.split('\n')[0]); process.exit(1); });
