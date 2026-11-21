// The terminal ids this browser remembers, and nothing else: never tokens,
// terminal output, parser state or titles. An id selects a terminal on the
// server in the Hello message only; it is not a credential. Two lists:
//
//   own    (sessionStorage) the terminals this window had open. A reload of the
//          window attaches to them again after the login.
//   known  (localStorage)   every id any window of this browser had open. Another
//          window may offer them, and attaching takes them over: an explicit
//          choice by the user, never automatic.
//
// Storage may be missing or throw; then nothing is remembered. Pure, so Node
// can test it (web/tools/test-sessionids.js).
(function (root, factory) {
  "use strict";
  if (typeof module === "object" && module.exports) {
    module.exports = factory();
  } else {
    root.mattermSessionIds = factory();
  }
})(typeof self !== "undefined" ? self : this, function () {
  "use strict";
  const ID = /^[0-9a-f]{32}$/;
  const OWN = "matterm.sessions.own";
  const KNOWN = "matterm.sessions.known";
  const LIMIT = 16;

  function valid(id) {
    return typeof id === "string" && ID.test(id);
  }

  // Valid ids only, once each, at most LIMIT; anything else yields [].
  function parse(text) {
    let value;
    try {
      value = JSON.parse(text);
    } catch (error) {
      return [];
    }
    if (!Array.isArray(value)) {
      return [];
    }
    const ids = [];
    for (const id of value) {
      if (valid(id) && ids.indexOf(id) < 0 && ids.length < LIMIT) {
        ids.push(id);
      }
    }
    return ids;
  }

  function create(ownStorage, knownStorage) {
    function read(storage, key) {
      try {
        return storage ? parse(storage.getItem(key)) : [];
      } catch (error) {
        return [];
      }
    }
    function write(storage, key, ids) {
      try {
        if (storage) {
          storage.setItem(key, JSON.stringify(ids.filter(valid).slice(0, LIMIT)));
        }
      } catch (error) {
        // Not remembered; the page works without it.
      }
    }
    return {
      ownIds: function () {
        return read(ownStorage, OWN);
      },
      knownIds: function () {
        return read(knownStorage, KNOWN);
      },
      // The ids of the terminals this window shows now, in tab order. They are
      // also remembered as known; ids that are only known stay known.
      setOpen: function (ids) {
        const open = ids.filter(valid).filter(function (id, i, all) {
          return all.indexOf(id) === i;
        }).slice(0, LIMIT);
        write(ownStorage, OWN, open);
        const kept = read(knownStorage, KNOWN).filter(function (id) {
          return open.indexOf(id) < 0;
        });
        // Newest first, so a full list drops the oldest.
        write(knownStorage, KNOWN, open.concat(kept));
      },
      // The server has certainly ended this terminal.
      forget: function (id) {
        write(ownStorage, OWN, read(ownStorage, OWN).filter(function (x) { return x !== id; }));
        write(knownStorage, KNOWN, read(knownStorage, KNOWN).filter(function (x) { return x !== id; }));
      },
      // Logout or close-all succeeded: no terminal is left.
      forgetAll: function () {
        write(ownStorage, OWN, []);
        write(knownStorage, KNOWN, []);
      },
      // Known ids that this window does not have open: what another window had.
      offered: function () {
        const own = read(ownStorage, OWN);
        return read(knownStorage, KNOWN).filter(function (id) {
          return own.indexOf(id) < 0;
        });
      },
    };
  }

  return { valid: valid, parse: parse, create: create, LIMIT: LIMIT };
});
