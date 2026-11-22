// Taking over a terminal of the laptop (the local daemon) from the browser, and taking it
// back. Needs a matterm-daemon on a temporary socket and a matterm-server started with
// --expose-local-sessions --daemon-socket SOCKET (run-e2e.sh does both).
//   1. the script creates two terminals in the daemon and shares one of them (control protocol)
//   2. the palette offers "Local terminals..."; the list shows only the shared one
//   3. choosing it opens a tab attached to that terminal; typing there runs in the daemon's shell
//   4. a "laptop window" (a terminal connection on the socket) takes it over: the browser pane
//      stops sending input; Enter in the browser takes it back
// usage: node e2e-local.js URL PASSWORD OUTDIR DAEMON_SOCKET   (URL without ?debug)
const { chromium } = require('playwright-core');
const net = require('net');
const fs = require('fs');
const os = require('os');
const path = require('path');
const fails = [];
function check(name, ok, extra) {
  console.log((ok ? 'PASS ' : 'FAIL ') + name + (ok ? '' : ' ' + (extra || '')));
  if (!ok) fails.push(name);
}
const u16 = (n) => Buffer.from([n >> 8, n & 255]);
const u32 = (n) => Buffer.from([(n >>> 24) & 255, (n >>> 16) & 255, (n >>> 8) & 255, n & 255]);
const u64 = (n) => Buffer.concat([u32(Math.floor(n / 4294967296)), u32(n >>> 0)]);
const str = (s) => Buffer.concat([u16(Buffer.byteLength(s)), Buffer.from(s)]);
const frame = (payload) => Buffer.concat([u32(payload.length), payload]);
const readString = (b, at) => { const n = b.readUInt16BE(at); return [b.toString('utf8', at + 2, at + 2 + n), at + 2 + n]; };

// A connection to the daemon's socket that hands out whole frames.
function connect(socket) {
  return new Promise((resolve, reject) => {
    const c = net.createConnection(socket);
    const frames = [];
    const waiters = [];
    let buf = Buffer.alloc(0);
    c.on('data', (d) => {
      buf = Buffer.concat([buf, d]);
      while (buf.length >= 4 && buf.length >= 4 + buf.readUInt32BE(0)) {
        const n = buf.readUInt32BE(0);
        frames.push(buf.subarray(4, 4 + n));
        buf = buf.subarray(4 + n);
      }
      while (waiters.length && frames.length) waiters.shift()(frames.shift());
    });
    c.on('error', reject);
    c.on('connect', () => resolve({
      send: (p) => c.write(frame(p)),
      next: () => new Promise((res) => (frames.length ? res(frames.shift()) : waiters.push(res))),
      all: () => frames.splice(0),
      end: () => c.destroy(),
    }));
  });
}
async function control(socket) {
  const c = await connect(socket);
  c.send(Buffer.concat([Buffer.from([0x40]), u16(2)])); // ControlHello
  const welcome = await c.next();
  if (welcome[0] !== 0xc0) throw new Error('no ControlWelcome');
  return c;
}
async function createSession(c) {
  c.send(Buffer.concat([Buffer.from([0x45]), str(os.tmpdir()), u16(80), u16(24)]));
  const r = await c.next();
  if (r[0] !== 0xc5) throw new Error('CreateSession failed: ' + r[0].toString(16));
  return readString(r, 1)[0];
}
async function setShared(c, id, on) {
  c.send(Buffer.concat([Buffer.from([0x42]), str(id), Buffer.from([on ? 1 : 0])]));
  const r = await c.next();
  if (r[0] !== 0xc1) throw new Error('SetShared failed: ' + r[0].toString(16));
}
const parse = (plan) => {
  const m = /plan tabs=(\d+) panes=(\d+) strip=(\d+) focus=(\d+) layers=(\d+)/.exec(plan || '');
  return m ? { tabs: +m[1], total: +m[2], strip: +m[3], layers: +m[5] } : null;
};
const until = async (fn, ms = 6000) => { const end = Date.now() + ms; while (Date.now() < end) { if (await fn()) return true; await new Promise((r) => setTimeout(r, 100)); } return false; };

(async () => {
  const [url, pw, out, socket] = process.argv.slice(2);
  const ctl = await control(socket);
  const sharedId = await createSession(ctl);
  const privateId = await createSession(ctl);
  await setShared(ctl, sharedId, true);
  const stamp = `${process.pid}-${Date.now()}`;
  const marker = (n) => path.join(os.tmpdir(), `matterm-local-${stamp}-${n}`);

  const browser = await chromium.launch({ args: ['--use-gl=angle','--use-angle=swiftshader','--enable-unsafe-swiftshader','--ignore-gpu-blocklist','--no-sandbox'] });
  const page = await (await browser.newContext({ viewport: { width: 1000, height: 640 } })).newPage();
  const plans = [];
  page.on('console', (m) => { if (m.text().includes('plan tabs')) plans.push(m.text()); });
  page.on('pageerror', (e) => check('no page error', false, e.message));
  const last = () => parse(plans[plans.length - 1]);
  await page.goto(url + '?debug');
  await page.fill('#password', pw);
  await page.click('button');
  await page.waitForTimeout(3500);
  const prefix = async (k) => { await page.keyboard.press('Control+b'); await page.waitForTimeout(150); await page.keyboard.press(k); await page.waitForTimeout(600); };

  // The server exposes them, so the palette has the entry; the list has the shared one only.
  const listed = await page.evaluate(async () => {
    const r = await fetch('/local-sessions', { method: 'POST', credentials: 'include' });
    return { status: r.status, body: r.ok ? await r.json() : null };
  });
  check('POST /local-sessions answers', listed.status === 200, JSON.stringify(listed));
  const ids = listed.body ? listed.body.sessions.map((s) => s.id) : [];
  check('only the shared terminal is listed', ids.includes(sharedId) && !ids.includes(privateId), JSON.stringify(ids));

  await prefix('Shift+;');
  check('the palette opens', last() && last().layers === 1, JSON.stringify(last()));
  await page.keyboard.type('local term', { delay: 30 });
  await page.waitForTimeout(300);
  await page.screenshot({ path: out + '/local-palette.png' });
  await page.keyboard.press('Enter');
  await page.waitForTimeout(1500); // The list is asked for, then shown as a layer.
  check('"Local terminals..." shows the list', last() && last().layers === 1 && last().tabs === 1, JSON.stringify(last()));
  await page.screenshot({ path: out + '/local-list.png' });
  await page.keyboard.press('Enter'); // The only shared terminal.
  await page.waitForTimeout(2000);
  check('choosing it opens a new tab', last() && last().layers === 0 && last().tabs === 2, JSON.stringify(last()));

  await page.keyboard.type(`touch ${marker(1)}\n`, { delay: 15 });
  check('typing runs in the daemon\'s shell', await until(() => fs.existsSync(marker(1))));
  await page.screenshot({ path: out + '/local-attached.png' });

  // The laptop's window takes it over.
  const laptop = await connect(socket);
  laptop.send(Buffer.concat([Buffer.from([0x01]), u16(1), u16(80), u16(24), u64(0), str(sharedId), u32(3)]));
  const welcome = await laptop.next();
  check('the laptop window attaches', welcome[0] === 0x81, welcome[0].toString(16));
  await page.waitForTimeout(1500);
  await page.keyboard.type(`touch ${marker(2)}\n`, { delay: 15 });
  await page.waitForTimeout(1000);
  check('the browser no longer controls it', !fs.existsSync(marker(2)));
  await page.screenshot({ path: out + '/local-taken-over.png' });

  // Enter takes it back; the laptop window is told.
  await page.keyboard.press('Enter');
  await page.waitForTimeout(2000);
  const told = laptop.all().some((f) => f[0] === 0x85);
  check('the laptop window was told it was replaced', told);
  await page.keyboard.type(`touch ${marker(3)}\n`, { delay: 15 });
  check('after Enter the browser controls it again', await until(() => fs.existsSync(marker(3))));
  await page.screenshot({ path: out + '/local-taken-back.png' });

  // Stopping to share cuts the browser off (the daemon says so); the terminal itself lives on.
  await setShared(ctl, sharedId, false);
  await page.waitForTimeout(1500);
  const again = await page.evaluate(async () => (await (await fetch('/local-sessions', { method: 'POST', credentials: 'include' })).json()).sessions.map((s) => s.id));
  check('after "stop sharing" the list is empty of it', !again.includes(sharedId), JSON.stringify(again));

  for (const f of [1, 2, 3]) { try { fs.unlinkSync(marker(f)); } catch (e) {} }
  laptop.end();
  ctl.end();
  await browser.close();
  if (fails.length) process.exit(1);
})().catch((e) => { console.error('FAIL', e.message.split('\n')[0]); process.exit(1); });
