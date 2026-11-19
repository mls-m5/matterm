// The framed terminal window: margins, rounded corners and shadow on a desktop
// viewport, edge to edge on a phone, the full-screen toggle (remembered across a
// reload) and a grid that matches the visible canvas in both layouts.
// usage: node e2e-frame.js URL PASSWORD [outdir]
const { chromium } = require('playwright-core');
const path = require('path');

let failures = 0;
const check = (name, ok, detail) => {
  console.log((ok ? 'PASS ' : 'FAIL ') + name + (ok ? '' : '  ' + (detail || '')));
  if (!ok) failures++;
};
const args = ['--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist', '--no-sandbox'];

(async () => {
  const [url, pw, out] = process.argv.slice(2);
  const browser = await chromium.launch({ args });

  const open = async (context) => {
    const page = await context.newPage();
    page.on('pageerror', e => console.log('pageerror:', e.message));
    await page.goto(url);
    await page.fill('#password', pw);
    await page.press('#password', 'Enter');
    await page.waitForTimeout(3500);
    return page;
  };
  const box = page => page.evaluate(() => {
    const r = document.getElementById('canvas').getBoundingClientRect();
    return { left: r.left, top: r.top, right: r.right, bottom: r.bottom, width: r.width, height: r.height,
      vw: innerWidth, vh: innerHeight, backing: [canvas.width, canvas.height] };
  });
  // `stty size` prints "rows cols"; the grid must fill the canvas to within one
  // cell. The cell size is estimated from the 18 px default font (JetBrains
  // Mono: 0.6 em wide, about 1.32 em high) with a tolerance of one cell.
  const gridFits = async (page, b) => {
    await page.keyboard.type('clear; stty size\n', { delay: 15 });
    await page.waitForTimeout(800);
    await page.keyboard.press('Control+Shift+A');
    await page.keyboard.press('Control+Shift+C');
    await page.waitForTimeout(300);
    const text = await page.evaluate(() => navigator.clipboard.readText());
    const m = text.match(/^(\d+) (\d+)$/m);
    if (!m) return { ok: false, detail: JSON.stringify(text.slice(0, 80)) };
    const rows = Number(m[1]), cols = Number(m[2]);
    // Cells are whole device pixels (about 11 x 24 at 18 px), and the scrollbar
    // strip takes 12 px of the width.
    const cw = 11, ch = 24, strip = 12;
    const ok = Math.abs(cols - Math.floor((b.width - strip) / cw)) <= 1 && Math.abs(rows - Math.floor(b.height / ch)) <= 1;
    return { ok, detail: `${rows}x${cols} in ${b.width}x${b.height}` };
  };

  // Desktop, framed by default.
  const desktop = await browser.newContext({ viewport: { width: 1280, height: 800 }, permissions: ['clipboard-read', 'clipboard-write'] });
  let page = await open(desktop);
  let b = await box(page);
  check('framed: margins on all sides', b.left > 8 && b.top > 8 && b.vw - b.right > 8 && b.vh - b.bottom > 8, JSON.stringify(b));
  check('framed: narrower than the viewport', b.width < b.vw - 16, JSON.stringify(b));
  const style = await page.evaluate(() => {
    const s = getComputedStyle(document.getElementById('window'));
    return { radius: parseFloat(s.borderTopLeftRadius), shadow: s.boxShadow, overflow: s.overflow,
      page: getComputedStyle(document.body).backgroundColor };
  });
  check('framed: rounded corners and a shadow, clipped', style.radius >= 8 && style.shadow !== 'none' && style.overflow === 'hidden', JSON.stringify(style));
  check('framed: page color differs from the terminal', style.page !== 'rgb(0, 0, 0)', style.page);
  check('framed: backing store is device pixels of the canvas', Math.abs(b.backing[0] - b.width) <= 1, JSON.stringify(b.backing));
  let fit = await gridFits(page, b);
  check('framed: stty size matches the canvas', fit.ok, fit.detail);
  if (out) await page.screenshot({ path: path.join(out, 'frame.png') });

  // The toggle is hidden until the mouse is near the top edge.
  const button = page.locator('#layout');
  check('toggle is hidden away from the top edge', Number(await button.evaluate(e => getComputedStyle(e).opacity)) === 0);
  await page.mouse.move(b.right - 40, b.top + 14);
  await page.waitForTimeout(300);
  check('toggle appears near the top edge', Number(await button.evaluate(e => getComputedStyle(e).opacity)) === 1);
  await button.click();
  await page.waitForTimeout(800);
  b = await box(page);
  check('toggle: edge to edge', b.left === 0 && b.top === 0 && b.width === b.vw && b.height === b.vh, JSON.stringify(b));
  check('toggle: no radius', await page.evaluate(() => parseFloat(getComputedStyle(document.getElementById('window')).borderTopLeftRadius)) === 0);
  fit = await gridFits(page, b);
  check('full: stty size matches the canvas', fit.ok, fit.detail);
  check('toggle choice is stored', await page.evaluate(() => localStorage.getItem('matterm.layout')) === 'full');
  if (out) await page.screenshot({ path: path.join(out, 'frame-full.png') });

  // Remembered after a reload; toggling back restores the frame.
  await page.reload();
  await page.fill('#password', pw);
  await page.press('#password', 'Enter');
  await page.waitForTimeout(3500);
  b = await box(page);
  check('full screen survives a reload', b.left === 0 && b.width === b.vw, JSON.stringify(b));
  await page.mouse.move(b.right - 40, b.top + 14);
  await page.waitForTimeout(300);
  await page.locator('#layout').click();
  await page.waitForTimeout(800);
  b = await box(page);
  check('toggle back: framed again', b.left > 8 && b.width < b.vw - 16, JSON.stringify(b));
  await desktop.close();

  // ?fullscreen starts edge to edge.
  const query = await browser.newContext({ viewport: { width: 1280, height: 800 } });
  const qp = await query.newPage();
  await qp.goto(url + (url.includes('?') ? '&' : '?') + 'fullscreen');
  await qp.fill('#password', pw);
  await qp.press('#password', 'Enter');
  await qp.waitForTimeout(3500);
  b = await box(qp);
  check('?fullscreen: edge to edge', b.left === 0 && b.top === 0 && b.width === b.vw, JSON.stringify(b));
  await query.close();

  // Resize with the frame: the canvas follows the viewport.
  const resizing = await browser.newContext({ viewport: { width: 1280, height: 800 } });
  page = await open(resizing);
  const before = await box(page);
  await page.setViewportSize({ width: 1000, height: 700 });
  await page.waitForTimeout(800);
  const after = await box(page);
  check('resize: the framed canvas follows the viewport', after.width < before.width && after.height < before.height && after.left > 8, JSON.stringify([before.width, after.width]));
  await resizing.close();

  // Phone: edge to edge, no toggle, the key bar keeps its place.
  const phone = await browser.newContext({ viewport: { width: 400, height: 700 }, deviceScaleFactor: 2, isMobile: true, hasTouch: true });
  page = await open(phone);
  b = await box(page);
  const bar = await page.evaluate(() => document.getElementById('keybar').getBoundingClientRect().height);
  check('phone: edge to edge', b.left === 0 && b.top === 0 && b.width === b.vw, JSON.stringify(b));
  check('phone: the key bar takes its height from the canvas', Math.abs(b.height - (b.vh - bar)) <= 1, JSON.stringify([b.height, b.vh, bar]));
  check('phone: no radius and no toggle', await page.evaluate(() =>
    parseFloat(getComputedStyle(document.getElementById('window')).borderTopLeftRadius) === 0 &&
    getComputedStyle(document.getElementById('layout')).display === 'none'));
  check('phone: backing store is device pixels', Math.abs(b.backing[0] - b.width * 2) <= 1, JSON.stringify(b.backing));
  if (out) await page.screenshot({ path: path.join(out, 'frame-phone.png') });
  await phone.close();

  await browser.close();
  if (failures) { console.error(failures + ' check(s) failed'); process.exit(1); }
})().catch(e => { console.error('FAIL', e.message.split('\n')[0]); process.exit(1); });
