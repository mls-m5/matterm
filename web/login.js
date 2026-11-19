// Logs in with POST /login (the session cookie is HttpOnly, so this script
// never sees it), then starts the terminal, which opens the WebSocket itself.
(function () {
  const form = document.getElementById("login");
  const password = document.getElementById("password");
  const message = document.getElementById("message");
  const canvas = document.getElementById("canvas");
  const terminalWindow = document.getElementById("window");
  const layoutButton = document.getElementById("layout");
  const pageParams = new URLSearchParams(location.search);
  // ?embed: the page is shown inside another page (an iframe): edge to edge, no
  // toggle. ?theme=light|dark: the page colors around the terminal.
  const embed = pageParams.has("embed");
  const theme = pageParams.get("theme");
  if (theme === "light" || theme === "dark") {
    document.documentElement.dataset.scheme = theme;
  }
  if (embed) {
    document.body.classList.add("embed");
  }

  // Film-terminal pips for revealed and typed text (the demo only), made with
  // WebAudio: nothing to download. Browsers allow sound only after a user
  // gesture, so the audio context is created at the first key or click. On by
  // default in the demo page, off in an embedded one; ?sound=1 turns it on
  // anywhere, ?mute or ?sound=0 off.
  window.mattermSoundWanted = (function () {
    const p = pageParams;
    if (p.has("mute") || p.get("sound") === "0") {
      return false;
    }
    return p.has("sound") || !embed;
  })();
  (function setupSound() {
    let ctx = null;
    let playing = 0;
    const unlock = function () {
      if (!ctx) {
        const Audio = window.AudioContext || window.webkitAudioContext;
        if (!Audio) {
          return;
        }
        try {
          ctx = new Audio();
        } catch (e) {
          return;
        }
      }
      if (ctx.state === "suspended") {
        ctx.resume().catch(function () {});
      }
    };
    ["pointerdown", "keydown", "touchstart"].forEach(function (name) {
      window.addEventListener(name, unlock, { passive: true });
    });
    window.mattermBeep = function (frequency, ms, volume) {
      if (!window.mattermSoundWanted || !ctx || ctx.state !== "running" || playing > 24) {
        return;
      }
      const t = ctx.currentTime;
      const seconds = Math.max(0.008, ms / 1000);
      const osc = ctx.createOscillator();
      const filter = ctx.createBiquadFilter();
      const gain = ctx.createGain();
      osc.type = "square";
      osc.frequency.setValueAtTime(frequency, t);
      filter.type = "lowpass";
      filter.frequency.setValueAtTime(3200, t);
      gain.gain.setValueAtTime(0, t);
      gain.gain.linearRampToValueAtTime(Math.min(1, volume) * 0.036, t + 0.003);
      gain.gain.linearRampToValueAtTime(0, t + seconds);
      osc.connect(filter);
      filter.connect(gain);
      gain.connect(ctx.destination);
      ++playing;
      osc.onended = function () {
        --playing;
      };
      osc.start(t);
      osc.stop(t + seconds + 0.01);
    };
  })();

  function start(demo) {
    form.hidden = true;
    terminalWindow.hidden = false;
    document.body.classList.add("started");
    fitCanvas();
    setupLayout();
    setupPause();
    // Demo: no server link, so the page runs the built-in simulator.
    if (!demo) {
      window.mattermWs =
        (location.protocol === "https:" ? "wss://" : "ws://") + location.host + "/ws";
    }
    window.Module = { canvas: canvas };
    // Ctrl+Shift+V: the browser's paste event carries the clipboard text.
    document.addEventListener("paste", function (event) {
      const text = event.clipboardData && event.clipboardData.getData("text");
      if (text && window.Module && window.Module._matterm_paste) {
        window.Module.ccall("matterm_paste", null, ["string"], [text]);
      }
      event.preventDefault();
    });
    canvas.focus();
    setupTouchKeyboard();
    const script = document.createElement("script");
    script.src = "index.js";
    document.body.appendChild(script);
  }

  // Touch devices: tapping the terminal focuses a hidden field so the on-screen
  // keyboard appears; its text goes to the terminal through matterm_text. Keys a
  // phone keyboard lacks come from the bar. Hardware keys are handled by SDL.
  function setupTouchKeyboard() {
    const touch = window.matchMedia("(pointer: coarse)").matches ||
      new URLSearchParams(location.search).has("touch");
    if (!touch) {
      return;
    }
    const field = document.getElementById("softkeys");
    const bar = document.getElementById("keybar");
    field.hidden = false;
    bar.hidden = false;
    document.body.classList.add("touch");
    fitCanvas(); // The bar takes room from the canvas.
    const latched = { ctrl: false, alt: false };
    const mods = function () {
      return (latched.ctrl ? 1 : 0) | (latched.alt ? 2 : 0);
    };
    const release = function () {
      latched.ctrl = latched.alt = false;
      bar.querySelectorAll("[data-mod]").forEach(function (b) {
        b.setAttribute("aria-pressed", "false");
      });
    };
    const mod = function () { return window.Module; };
    const text = function (value) {
      if (value && mod() && mod().ccall) {
        mod().ccall("matterm_text", null, ["string", "number"], [value, mods()]);
        release();
      }
    };
    const key = function (name) {
      if (mod() && mod().ccall) {
        mod().ccall("matterm_key", null, ["string", "number"], [name, mods()]);
        release();
      }
    };
    // Tapping the terminal (not a drag or scroll) opens the keyboard. This
    // needs a user gesture, so it happens in the event handler itself.
    // Only a short single-finger tap counts: a drag scrolls, a long press
    // selects and a pinch zooms (main.cpp), none of which may toggle the keyboard.
    let downAt = null;
    const touching = new Set();
    canvas.addEventListener("pointerdown", function (event) {
      if (event.pointerType !== "touch") {
        return;
      }
      touching.add(event.pointerId);
      downAt = touching.size === 1
        ? [event.clientX, event.clientY, event.timeStamp] : null;
    });
    canvas.addEventListener("pointerup", function (event) {
      touching.delete(event.pointerId);
      if (downAt && event.timeStamp - downAt[2] < 400 &&
          Math.hypot(event.clientX - downAt[0], event.clientY - downAt[1]) < 10) {
        field.focus();
      }
      downAt = null;
    });
    canvas.addEventListener("pointercancel", function (event) {
      touching.delete(event.pointerId);
      downAt = null;
    });
    // A long press must not open the browser's context menu.
    canvas.addEventListener("contextmenu", function (event) {
      event.preventDefault();
    });
    // Printable text and, on keyboards that report no key codes, deletion.
    let namedKeyDown = false;
    field.addEventListener("keydown", function (event) {
      // SDL sees named keys (Enter, Backspace, arrows) itself.
      namedKeyDown = event.key.length > 1 && event.key !== "Unidentified";
    }, true);
    field.addEventListener("beforeinput", function (event) {
      if (event.inputType === "insertText" || event.inputType === "insertCompositionText") {
        if (event.data) {
          text(event.data);
        }
      } else if (event.inputType === "deleteContentBackward" && !namedKeyDown) {
        key("Backspace");
      } else if (event.inputType === "insertLineBreak" && !namedKeyDown) {
        key("Enter");
      }
      namedKeyDown = false;
      event.preventDefault(); // The field stays empty; the terminal is the state.
    });
    bar.addEventListener("pointerdown", function (event) {
      event.preventDefault(); // Keep the keyboard open.
    });
    bar.addEventListener("click", function (event) {
      const button = event.target.closest("button");
      if (!button) {
        return;
      }
      if (button.dataset.mod) {
        const on = !latched[button.dataset.mod];
        latched[button.dataset.mod] = on;
        button.setAttribute("aria-pressed", String(on));
      } else if (button.dataset.key) {
        key(button.dataset.key);
      } else if (button.dataset.text) {
        text(button.dataset.text);
      }
    });
  }

  // Framed (default on wide desktop viewports, see style.css) or edge to edge.
  // `?fullscreen` and the toggle choose edge to edge; the choice is remembered
  // per browser. localStorage can be unavailable, then the choice lasts for the
  // page only.
  function setupLayout() {
    const params = new URLSearchParams(location.search);
    let stored = null;
    try {
      stored = localStorage.getItem("matterm.layout");
    } catch (e) {}
    const apply = function (full) {
      if (full) {
        document.body.dataset.layout = "full";
      } else {
        delete document.body.dataset.layout;
      }
      layoutButton.setAttribute("aria-pressed", String(full));
      snapWindow();
      fitCanvas();
    };
    // ?embed (a page inside an <iframe>): edge to edge, no toggle, nothing remembered.
    if (params.has("embed")) {
      layoutButton.remove();
      apply(true);
      return;
    }
    apply(params.has("fullscreen") || stored === "full");
    layoutButton.addEventListener("click", function () {
      const full = document.body.dataset.layout !== "full";
      apply(full);
      try {
        localStorage.setItem("matterm.layout", full ? "full" : "framed");
      } catch (e) {}
      layoutButton.classList.remove("near");
      canvas.focus(); // Keys belong to the terminal, not to the button.
    });
    // The button appears when a mouse is near the top edge of the window.
    document.addEventListener("pointermove", function (event) {
      if (event.pointerType !== "mouse") {
        return;
      }
      const box = terminalWindow.getBoundingClientRect();
      const near = event.clientY < box.top + 48 && event.clientY >= box.top - 24 &&
        event.clientX > box.right - 160 && event.clientX <= box.right + 24;
      layoutButton.classList.toggle("near", near);
    });
    document.addEventListener("pointerleave", function () {
      layoutButton.classList.remove("near");
    });
  }

  // Framed layout: size the window to a whole number of cells (plus the scrollbar
  // strip) so no partial cell is left at the right or bottom. The cell size comes
  // from the terminal (window.mattermCell, device pixels). Edge-to-edge layouts
  // (no border) keep the full viewport; the leftover there is terminal background.
  let cell = null;
  function snapWindow() {
    terminalWindow.style.width = "";
    terminalWindow.style.height = "";
    const style = getComputedStyle(terminalWindow);
    const border = parseFloat(style.borderTopWidth) || 0;
    if (!cell || border === 0 || terminalWindow.hidden) {
      return;
    }
    const ratio = window.devicePixelRatio || 1;
    const rect = terminalWindow.getBoundingClientRect();
    const cols = Math.max(1, Math.floor(((rect.width - 2 * border) * ratio - cell.strip) / cell.width));
    const rows = Math.max(1, Math.floor((rect.height - 2 * border) * ratio / cell.height));
    terminalWindow.style.width = (cols * cell.width + cell.strip) / ratio + 2 * border + "px";
    terminalWindow.style.height = rows * cell.height / ratio + 2 * border + "px";
  }
  window.mattermCell = function (width, height, strip) {
    cell = { width: width, height: height, strip: strip };
    snapWindow();
  };
  window.addEventListener("resize", snapWindow);

  // The canvas fills the terminal window, so its CSS size is the container's
  // and the backing store is the container in device pixels: text stays sharp
  // on HiDPI screens and the grid matches what is visible. A ResizeObserver
  // sees every cause (window resize, rotation, layout toggle, keyboard bar).
  let fitted = [0, 0];
  function setBackingSize(width, height) {
    width = Math.max(1, width);
    height = Math.max(1, height);
    if (width === fitted[0] && height === fitted[1]) {
      return;
    }
    fitted = [width, height];
    canvas.width = width;
    canvas.height = height;
    // SDL learns the new window size from a resize event; the layout can
    // change without the browser sending one.
    window.dispatchEvent(new Event("resize"));
  }
  let observed = false; // Once the observer reports sizes it is the only source.
  function fitCanvas() {
    if (observed) {
      return;
    }
    const ratio = window.devicePixelRatio || 1;
    setBackingSize(Math.round(canvas.clientWidth * ratio),
      Math.round(canvas.clientHeight * ratio));
  }
  if (window.ResizeObserver) {
    new ResizeObserver(function (entries) {
      observed = true;
      const size = entries[0].devicePixelContentBoxSize;
      if (size && size.length) {
        setBackingSize(size[0].inlineSize, size[0].blockSize); // Exact device pixels.
      } else {
        fitCanvas();
      }
    }).observe(canvas, { box: "device-pixel-content-box" });
  }
  if (!window.ResizeObserver) {
    window.addEventListener("resize", fitCanvas);
  }

  // A page that is out of view (scrolled away, a hidden tab, an iframe below the
  // fold) does not need frames: the terminal checks window.mattermPaused.
  function setupPause() {
    window.mattermPaused = document.hidden;
    document.addEventListener("visibilitychange", function () {
      window.mattermPaused = document.hidden || window.mattermOffscreen === true;
    });
    if (window.IntersectionObserver) {
      new IntersectionObserver(function (entries) {
        window.mattermOffscreen = !entries[entries.length - 1].isIntersecting;
        window.mattermPaused = document.hidden || window.mattermOffscreen;
      }).observe(terminalWindow);
    }
  }

  // ?demo skips the login and runs the in-browser pretend system (no server
  // needed). demo.html (generated from shell.html, see web/CMakeLists.txt) is the
  // same page with a marker, so a static host can serve it without a query.
  if (new URLSearchParams(location.search).has("demo") ||
      document.querySelector('meta[name="matterm-static-demo"]')) {
    start(true);
    return;
  }

  form.addEventListener("submit", async function (event) {
    event.preventDefault();
    message.textContent = "";
    let response;
    try {
      response = await fetch("/login", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ password: password.value }),
        credentials: "same-origin",
      });
    } catch (error) {
      message.textContent = "Could not reach the server.";
      return;
    }
    password.value = "";
    if (response.ok) {
      start();
    } else if (response.status === 429) {
      const wait = response.headers.get("Retry-After");
      message.textContent = "Too many attempts. Try again in " + wait + " s.";
    } else {
      message.textContent = "Login failed.";
    }
  });
})();
