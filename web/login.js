// Logs in with POST /login (the session cookie is HttpOnly, so this script
// never sees it), then starts the terminal, which opens the WebSocket itself.
(function () {
  const form = document.getElementById("login");
  const password = document.getElementById("password");
  const message = document.getElementById("message");
  const canvas = document.getElementById("canvas");

  function start() {
    form.hidden = true;
    canvas.hidden = false;
    canvas.width = window.innerWidth;
    canvas.height = window.innerHeight;
    window.mattermWs =
      (location.protocol === "https:" ? "wss://" : "ws://") + location.host + "/ws";
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
    const script = document.createElement("script");
    script.src = "index.js";
    document.body.appendChild(script);
    window.addEventListener("resize", function () {
      canvas.width = window.innerWidth;
      canvas.height = window.innerHeight;
    });
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
