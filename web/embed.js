// Mounts the matterm demo into a page as an <iframe>:
//
//   <div data-matterm-demo data-src="https://example.org/matterm/demo.html"
//        data-height="420" data-nointro data-cmd="neofetch" data-theme="dark"></div>
//   <script src="https://example.org/matterm/embed.js" defer></script>
//
// data-src      the demo page (static, hosted apart from any matterm-server)
// data-height   CSS height in pixels (default 420); the width is the container's
// data-nointro, data-noreveal   flags: skip the intro, print output at once
// data-cmd      a command typed at the first prompt
// data-theme    light or dark: the page colors around the terminal
// data-smooth   cursor glide in ms (0 turns it off)
//
// The iframe is sandboxed to what the demo needs (scripts, its own storage) and
// loads lazily; it pauses itself while scrolled out of view. Click it to give it
// the keyboard.
(function () {
  "use strict";
  function mount(host) {
    const src = host.getAttribute("data-src");
    if (!src) {
      return;
    }
    const url = new URL(src, location.href);
    url.searchParams.set("embed", "");
    ["nointro", "noreveal"].forEach(function (flag) {
      if (host.hasAttribute("data-" + flag)) {
        url.searchParams.set(flag, "");
      }
    });
    ["cmd", "theme", "smooth"].forEach(function (name) {
      const value = host.getAttribute("data-" + name);
      if (value !== null) {
        url.searchParams.set(name, value);
      }
    });
    const frame = document.createElement("iframe");
    frame.src = url.toString();
    frame.title = host.getAttribute("data-title") || "matterm demo";
    frame.loading = "lazy";
    frame.setAttribute("sandbox", "allow-scripts allow-same-origin");
    frame.setAttribute("allow", "clipboard-write");
    frame.setAttribute("referrerpolicy", "no-referrer");
    frame.tabIndex = 0;
    frame.style.cssText =
      "display:block;width:100%;border:0;border-radius:10px;background:#1e1e1e;height:" +
      (parseInt(host.getAttribute("data-height"), 10) || 420) + "px";
    host.replaceChildren(frame);
  }
  function run() {
    document.querySelectorAll("[data-matterm-demo]").forEach(mount);
  }
  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", run);
  } else {
    run();
  }
})();
