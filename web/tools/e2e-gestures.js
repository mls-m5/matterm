// Touch gestures with CDP touch events (headless Chromium, hasTouch): a tap is
// a click, a one-finger drag scrolls (primary screen, alternate screen, mouse
// reporting), a long press selects a word and copies it, a pinch changes the
// font size and the choice survives a reload.
// usage: node e2e-gestures.js URL PASSWORD [out.png]
const { chromium } = require('playwright-core');
const crypto = require('crypto');

let failures = 0;
const check = (name, ok, detail) => {
  console.log((ok ? 'PASS ' : 'FAIL ') + name + (ok ? '' : '  ' + (detail || '')));
  if (!ok) failures++;
};

(async () => {
  const [url, pw, shot] = process.argv.slice(2);
  const browser = await chromium.launch({ args: ['--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist', '--no-sandbox'] });
  const ctx = await browser.newContext({ viewport: { width: 400, height: 700 }, deviceScaleFactor: 2, isMobile: true, hasTouch: true, permissions: ['clipboard-read', 'clipboard-write'] });
  const page = await ctx.newPage();
  page.on('pageerror', e => console.log('pageerror:', e.message));
  const cdp = await ctx.newCDPSession(page);

  const login = async () => {
    await page.goto(url);
    await page.fill('#password', pw);
    await page.tap('button[type=submit]');
    await page.waitForTimeout(3500);
  };
  // Fingers are [x, y] pairs; ids are their indexes.
  const send = (type, fingers) => cdp.send('Input.dispatchTouchEvent', {
    type, touchPoints: fingers.map(([x, y], id) => ({ x, y, id })),
  });
  const drag = async (from, to, steps = 8) => {
    await send('touchStart', [from]);
    for (let i = 1; i <= steps; i++) {
      await send('touchMove', [[from[0] + (to[0] - from[0]) * i / steps, from[1] + (to[1] - from[1]) * i / steps]]);
      await page.waitForTimeout(16);
    }
    await send('touchEnd', []);
    await page.waitForTimeout(300);
  };
  const type = async text => {
    await page.evaluate(t => document.getElementById('softkeys').dispatchEvent(
      new InputEvent('beforeinput', { inputType: 'insertText', data: t, cancelable: true, bubbles: true })), text);
  };
  const enter = () => page.evaluate(() => document.getElementById('softkeys').dispatchEvent(
    new InputEvent('beforeinput', { inputType: 'insertLineBreak', cancelable: true, bubbles: true })));
  const run = async command => { await type(command); await enter(); await page.waitForTimeout(700); };
  const canvasHash = async () => crypto.createHash('md5').update(await page.locator('#canvas').screenshot()).digest('hex');
  // Everything the terminal shows, through select-all and copy.
  const screenText = async () => {
    await page.evaluate(() => document.getElementById('canvas').focus());
    await page.keyboard.press('Control+Shift+A');
    await page.keyboard.press('Control+Shift+C');
    await page.waitForTimeout(300);
    return page.evaluate(() => navigator.clipboard.readText());
  };
  const size = async () => {
    await run('clear; stty size');
    const m = (await screenText()).match(/^(\d+) (\d+)$/m);
    return m ? [Number(m[1]), Number(m[2])] : null;
  };

  await login();
  await page.tap('#canvas', { position: { x: 200, y: 300 } }); // Opens the keyboard field.
  check('tap focuses the keyboard field', await page.evaluate(() => document.activeElement.id) === 'softkeys');

  // A drag scrolls the scrollback and neither moves focus nor selects.
  await run('seq 1 300');
  // The cursor blinks, so accept either phase as "the same picture".
  const bottom = await canvasHash();
  await page.waitForTimeout(520);
  const bottomOther = await canvasHash();
  await drag([200, 200], [200, 600]);
  const scrolled = await canvasHash();
  check('drag down scrolls back', scrolled !== bottom);
  check('drag keeps the keyboard field focused', await page.evaluate(() => document.activeElement.id) === 'softkeys');
  await drag([200, 600], [200, 200]);
  { const h = await canvasHash(); check('drag up scrolls forward again', h === bottom || h === bottomOther); }

  // Alternate screen: a drag sends arrow keys, as the wheel does.
  await run("printf '\\033[?1049h'; stty -icanon -echo; cat -v");
  await drag([200, 200], [200, 500]);
  let text = await screenText();
  check('alternate screen drag sends Up keys', /\^\[\[A|\^\[OA/.test(text), JSON.stringify(text.slice(0, 80)));
  await page.keyboard.press('Control+C');
  await page.waitForTimeout(400); // Key events reach the terminal on the next frame; text does not wait.
  await run("printf '\\033[?1049l'; stty sane");

  // Application mouse reporting: drag gives wheel presses, tap gives a click.
  await run("clear; printf '\\033[?1000h\\033[?1006h'; stty -icanon -echo; cat -v");
  await drag([200, 200], [200, 500]);
  text = await screenText();
  check('mouse reporting drag sends wheel-up presses', /\^\[\[<64;/.test(text), JSON.stringify(text.slice(0, 80)));
  await send('touchStart', [[100, 100]]); await send('touchEnd', []);
  await page.waitForTimeout(300);
  text = await screenText();
  check('mouse reporting tap sends press and release', /\^\[\[<0;\d+;\d+M\^\[\[<0;\d+;\d+m/.test(text), JSON.stringify(text.slice(-80)));
  await page.keyboard.press('Control+C');
  await page.waitForTimeout(400); // Key events reach the terminal on the next frame; text does not wait.
  await run("printf '\\033[?1000l\\033[?1049l'; stty echo");

  // Long press selects the word under the finger and copies it.
  await run('clear; echo alpha beta gamma');
  await page.evaluate(() => navigator.clipboard.writeText('unset'));
  // Cells are about 11 x 24 CSS px at the default 18 px font; "beta" is columns
  // 6-9 of row 0 (`clear` leaves the echo output on the first row).
  const x = 82, y = 12;
  await send('touchStart', [[x, y]]);
  await page.waitForTimeout(700);
  await send('touchEnd', []);
  await page.waitForTimeout(400);
  const word = (await page.evaluate(() => navigator.clipboard.readText())).trim();
  check('long press copies a word', ['alpha', 'beta', 'gamma'].includes(word), JSON.stringify(word) + ' screen: ' + JSON.stringify((await screenText()).slice(0, 120)));

  // Pinch out grows the font (fewer rows and columns), pinch in shrinks it.
  const before = await size();
  const pinch = async (from, to) => {
    await send('touchStart', [[200 - from, 350], [200 + from, 350]]);
    for (let i = 1; i <= 6; i++) {
      const d = from + (to - from) * i / 6;
      await send('touchMove', [[200 - d, 350], [200 + d, 350]]);
      await page.waitForTimeout(16);
    }
    await send('touchEnd', []);
    await page.waitForTimeout(600);
  };
  await pinch(40, 120);
  const bigger = await size();
  check('pinch out gives fewer columns', before && bigger && bigger[1] < before[1], JSON.stringify([before, bigger]));
  check('pinch size is stored', Number(await page.evaluate(() => localStorage.getItem('matterm.fontSize'))) > 18);
  await pinch(120, 40);
  const smaller = await size();
  check('pinch in gives more columns again', smaller && bigger && smaller[1] > bigger[1], JSON.stringify([bigger, smaller, await page.evaluate(() => localStorage.getItem('matterm.fontSize'))]));

  // The size is restored after a reload.
  await pinch(40, 120);
  const stored = await page.evaluate(() => localStorage.getItem('matterm.fontSize'));
  await login();
  await page.tap('#canvas', { position: { x: 200, y: 300 } });
  const restored = await size();
  check('font size survives a reload', restored && restored[1] < before[1] && stored, JSON.stringify([stored, restored, before]));

  if (shot) await page.screenshot({ path: shot });
  await browser.close();
  if (failures) { console.error(failures + ' check(s) failed'); process.exit(1); }
})().catch(e => { console.error('FAIL', e.message.split('\n')[0]); process.exit(1); });
