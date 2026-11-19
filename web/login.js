// Logs in with POST /login (the session cookie is HttpOnly, so this script
// never sees it), then starts the terminal, which opens the WebSocket itself.
(function () {
  const form = document.getElementById("login");
  const password = document.getElementById("password");
  const message = document.getElementById("message");
  const canvas = document.getElementById("canvas");

  function start(demo) {
    form.hidden = true;
    canvas.hidden = false;
    fitCanvas();
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
    window.addEventListener("resize", fitCanvas);
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

  // The canvas's CSS size is the window; its backing store is in device pixels
  // so text stays sharp on HiDPI screens (the terminal scales its font too).
  function fitCanvas() {
    const ratio = window.devicePixelRatio || 1;
    canvas.width = Math.round(canvas.clientWidth * ratio);
    canvas.height = Math.round(canvas.clientHeight * ratio);
  }

  // ?demo skips the login and runs the in-browser simulator (no server needed).
  if (new URLSearchParams(location.search).has("demo")) {
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
