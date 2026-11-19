# Ctrl+click links

In the native PTY terminal and browser client, hold **Ctrl** over a detected URL
to underline it and show its complete target in the window/document title.
**Ctrl+left click** opens the URL without changing the selection. Releasing Ctrl,
leaving the text/window, losing focus or moving off the URL clears the preview
and restores the normal title. Existing ANSI underline attributes are unchanged.

A plain click keeps its existing selection or application-mouse behavior.
When an application has mouse reporting enabled, Ctrl+click still goes to the
application; use **Ctrl+Shift+left click** for local link opening. An already
captured application mouse press keeps ownership until release, even with Shift.
The same rule controls hover feedback, so a preview does not promise an action
that the current mouse mode will intercept.

Native opening uses `SDL_OpenURL`; failure is shown in the window title.
Browser opening is called in the SDL click handling path using
`window.open(url, "_blank", "noopener,noreferrer")`, with the URL supplied as
string data, not JavaScript source. Browser popup/activation policy still applies.
No shell, command execution, automatic opening or OSC 8 actions are introduced.

## Detection and security policy

`src/frontend/urllink.h` is CPU-only and independent of SDL and the operating
system. `urlAt(model, cell)` returns validated URL text plus an inclusive,
absolute cell range, or nothing. It reads displayed/retained text, including
soft wraps, but does not cross explicit newlines or recover evicted text.
Cells on removed punctuation or outside the model do not match.

The policy deliberately accepts a conservative subset of HTTP(S) URIs:

- Only `http://` and `https://`, case-insensitive. A nonempty valid authority is
  required. DNS/localhost names, dotted-decimal IPv4 and hexadecimal IPv6
  literals are supported, with optional decimal ports 1..65535. Credentials,
  ambiguous numeric host shorthands, IPv6 zone IDs and IPv4-mapped IPv6 notation
  are not accepted.
- At most **2048 bytes** for the returned URL. Work is also bounded to a
  4096-cell surrounding token, so very long output cannot cause unbounded
  hover scans. Detection never returns a truncated prefix of an oversized URL.
- ASCII URI spelling only: use punycode for international hostnames and percent
  encoding for non-ASCII path/query text. Raw non-ASCII and control characters,
  quotes, backslashes and characters outside the URI allow-list are rejected.
  Percent escapes must have two hexadecimal digits; escapes for C0/DEL controls,
  quotes and backslashes are also rejected. Spaces may be encoded as `%20`.
- Tokens must begin with the scheme, optionally preceded by prose opening
  brackets/quotes. Arbitrary prefixes such as `javascript:https://...` do not
  expose an embedded HTTP URL. Whitespace separates tokens; adjacent URLs
  without whitespace and Markdown link markup are not specially parsed.
- Trailing `.,;:!?`, `>`, and prose quotes are removed, as are unmatched closing
  `)`, `]`, `}`. Balanced parentheses/brackets inside a URL are retained.
  Leading prose wrappers and trailing quotes are outside the returned range;
  an **embedded** quote/backslash invalidates the token, rather than opening
  a potentially misleading prefix.

This is not a general-purpose URL parser, IDN implementation or trust check.
The destination remains untrusted even when its syntax is accepted. No URL is
opened until an explicit user gesture.

## Agent handoff and verification

Branch `agent3-url`, base `52b4a0647aec2b9c25aa4158cb09e01c2d8ea5d4`.

Shared-path changes:

- `app/ptyfrontend.cpp`: local click opening, failure title, per-frame hover.
- `web/main.cpp`: local click opening, per-frame hover, base-title preservation.
- `src/frontend/sdlinput.h`: shared modifier/focus/pointer hover adapter.
- `src/core/renderframe.h`: appended `underlineBegin`/`underlineEnd`, a half-open
  row-major overlay interval. This avoids copying cells or changing the model.
- `src/render/sdlbackend.cpp`: draw the overlay using resolved foreground colors,
  clipped/fractionally positioned like text. It coexists with selection and
  remains visible over blinking text. No glyph-atlas changes.
- `test/CMakeLists.txt`: register the new CPU-only URL test executable.

There are 12 URL unit cases for basic/query/fragment URLs, wrappers, every cell
boundary, wraps and history, schemes and malformed authorities, quote/backslash/
control rejection, exact/over-length URLs, bounded scanning, modifier precedence
and overlay range clipping. Two SDL framebuffer cases cover overlay appearance,
selection/blink interaction, clearing, fractional positioning and clipping.

Offline build with system DejaVu Sans Mono and font bundling disabled:

```sh
cmake -S . -B build -DMATTERM_BUNDLE_FONT=OFF \
  -DTEST_FONT_FILE=/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf
cmake --build build -j4
ctest --test-dir build --output-on-failure

cmake -S . -B build/selection-cpu -DMATTERM_BUILD_APP=OFF \
  -DMATTERM_BUILD_DEMO=OFF -DMATTERM_BUILD_RENDERER=OFF
cmake --build build/selection-cpu -j4
ctest --test-dir build/selection-cpu --output-on-failure
```

Results: **34/34** native tests and **21/21** graphics-disabled tests passed.
The URL test executable also passed with AddressSanitizer and UndefinedBehaviorSanitizer.
`emcmake` is unavailable: browser compilation, trusted-click popup activation,
focus/hover behavior and real desktop opening/failure reporting are not verified
here. No external browser was launched by the automated tests. The integrator
should check these gestures, including Ctrl+Shift under application mouse
reporting and hover clearing after key release/pointer leave, in the browser.
The pre-existing `lib/fast-json` gitlink mismatch is untouched and not committed.
No server/protocol/submodule changes or push were made.
