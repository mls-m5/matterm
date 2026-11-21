// Unit tests for web/sessionids.js. usage: node test-sessionids.js
const assert = require('assert');
const ids = require('../sessionids.js');
const A = 'a'.repeat(32), B = 'b'.repeat(32), C = 'c'.repeat(32);
const memory = () => {
  const data = new Map();
  return { getItem: (k) => (data.has(k) ? data.get(k) : null), setItem: (k, v) => { data.set(k, String(v)); }, data };
};
let count = 0;
const test = (name, fn) => { fn(); count++; console.log('ok  ' + name); };

test('only 32 lowercase hex characters are an id', () => {
  assert.ok(ids.valid(A));
  for (const bad of ['', A + 'a', A.slice(1), A.toUpperCase(), 'g'.repeat(32), A + '\n', ' ' + A, null, undefined, 5, [], {}]) {
    assert.strictEqual(ids.valid(bad), false);
  }
});

test('parsing survives garbage and keeps only valid, distinct ids', () => {
  for (const bad of [null, '', 'nope', '{}', '"x"', '5', '[1,2]', '[{"a":1}]']) {
    assert.deepStrictEqual(ids.parse(bad), []);
  }
  assert.deepStrictEqual(ids.parse(JSON.stringify([A, 'x', A, B, 7, null, '../x'])), [A, B]);
  const many = Array.from({ length: 40 }, (_, i) => String(i).padStart(32, '0'));
  assert.strictEqual(ids.parse(JSON.stringify(many)).length, ids.LIMIT);
});

test('a window remembers what it has open, and the browser what any window had', () => {
  const own = memory(), known = memory();
  const store = ids.create(own, known);
  assert.deepStrictEqual(store.ownIds(), []);
  store.setOpen([A, B]);
  assert.deepStrictEqual(store.ownIds(), [A, B]);
  assert.deepStrictEqual(store.knownIds(), [A, B]);
  // Closing one in this window keeps it known (it may still run), not own.
  store.setOpen([A]);
  assert.deepStrictEqual(store.ownIds(), [A]);
  assert.deepStrictEqual(store.knownIds().sort(), [A, B]);
  assert.deepStrictEqual(store.offered(), [B]);
});

test('a second window is offered the first one\'s terminals, not given them', () => {
  const known = memory();
  const first = ids.create(memory(), known);
  first.setOpen([A, B]);
  const second = ids.create(memory(), known); // New window: empty sessionStorage.
  assert.deepStrictEqual(second.ownIds(), []); // Nothing is attached automatically.
  assert.deepStrictEqual(second.offered().sort(), [A, B]);
  second.setOpen([C]);
  assert.deepStrictEqual(second.offered().sort(), [A, B]);
});

test('forgetting removes an id everywhere; forgetting all empties both', () => {
  const store = ids.create(memory(), memory());
  store.setOpen([A, B, C]);
  store.forget(B);
  assert.deepStrictEqual(store.ownIds(), [A, C]);
  assert.deepStrictEqual(store.knownIds(), [A, C]);
  store.forgetAll();
  assert.deepStrictEqual(store.ownIds(), []);
  assert.deepStrictEqual(store.knownIds(), []);
});

test('the lists are bounded and drop the oldest', () => {
  const store = ids.create(memory(), memory());
  const all = Array.from({ length: 20 }, (_, i) => String(i).padStart(32, '0'));
  store.setOpen(all.slice(0, 16));
  store.setOpen(all.slice(16, 20));
  assert.ok(store.knownIds().length <= ids.LIMIT);
  assert.deepStrictEqual(store.knownIds().slice(0, 4), all.slice(16, 20));
});

test('invalid ids are never written', () => {
  const own = memory(), known = memory();
  const store = ids.create(own, known);
  store.setOpen([A, 'not an id', '<script>', A]);
  assert.deepStrictEqual(store.ownIds(), [A]);
  for (const [, value] of [...own.data, ...known.data]) {
    assert.ok(/^\["[0-9a-f]{32}"(,"[0-9a-f]{32}")*\]$|^\[\]$/.test(value), value);
  }
});

test('missing or failing storage means nothing is remembered, without errors', () => {
  const broken = { getItem() { throw new Error('blocked'); }, setItem() { throw new Error('blocked'); } };
  for (const store of [ids.create(null, null), ids.create(broken, broken), ids.create(undefined, broken)]) {
    store.setOpen([A]);
    store.forget(A);
    store.forgetAll();
    assert.deepStrictEqual(store.ownIds(), []);
    assert.deepStrictEqual(store.knownIds(), []);
    assert.deepStrictEqual(store.offered(), []);
  }
});

test('stored garbage from an older or hostile page is ignored', () => {
  const own = memory(), known = memory();
  own.setItem('matterm.sessions.own', '{"x":1}');
  known.setItem('matterm.sessions.known', '["' + A + '","javascript:alert(1)"]');
  const store = ids.create(own, known);
  assert.deepStrictEqual(store.ownIds(), []);
  assert.deepStrictEqual(store.knownIds(), [A]);
});

console.log(count + ' passed');
