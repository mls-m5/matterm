# "Attach tab..." (renamed) and the detach-tab check

Written by agent3, 2026-11-23.

The palette entry that opens a detached tab again is now **Attach tab...** (it matches **Detach tab**);
the list is titled "Attach tab", the empty list says "No detached tabs", the notice after detaching says
"Attach tab... opens it again". Internal ids (`layout.attach`, `layout.attach.go`), the layout files and the word
"layout" in the code are unchanged. docs/TABS_PLAN.md section 14 says so.

## The reported bug: "Attach layout..." missing after Detach tab with --daemon-host

**Not reproduced.** `matterm --pty-layout-smoke-test` (hidden; needs a daemon, runs in the hidden SDL
window) does, through the real window code and the same palette entries the palette builds:
open a second tab, "Detach tab", the tab closes, the layout is saved, the palette offers
"Attach tab..."; then as the only tab (the window ends), and a later window (a restart) that must offer it
too. `test/layout_smoke.py` runs these against a local daemon (`layout_smoke_local`) and against
`--daemon-host` through the fake ssh (`layout_smoke_remote`, test/fakessh.py): all pass.

Where layouts live: on the computer of the window (`$XDG_CONFIG_HOME/matterm/layouts`), whichever daemon
the terminals run in; so `--daemon-host` is not the cause. What would make the entry vanish:
`$XDG_CONFIG_HOME`/`$HOME` differing between the two runs (a launcher versus a shell), the layout
folder not owned by the user or writable by others (the list ignores it), or the save failing
("layout not saved: ..." is shown as a notice). Please check with the owner what the notice said and
`ls ~/.config/matterm/layouts` after detaching.

## Not verified

A real ssh (fake ssh only), a real display (hidden dummy-driver window).
