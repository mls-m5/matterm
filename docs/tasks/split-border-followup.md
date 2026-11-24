# Split border follow-up: owner visual feedback

Owner reports that the terminal's background no longer reaches the right edge;
a black rectangle now looks like a scrollbar background. Investigate as a
regression against the rendering before themed dividers (cb177e5), with real
SDL pixel tests. Restore background coverage without covering scrollbar thumbs,
wrap marks, or terminal text. Determine the cause before assuming a fix.

Owner explicitly decided: **no extra outer border with the system window frame**;
the window manager supplies it. Do not reserve an extra pixel/inset there. With
our own frame, put the desired darker one-pixel boundary within the existing
frame, outside the text viewport; do not add a line where a scrollbar already
forms the edge. No additional outer edge against screen edges when maximized or
fullscreen. At a vertical split's top junction the themed middle should connect
continuously into the top themed strip, without a dark seam across it.

No pixel may overlay text or reduce terminal geometry merely to add a border.
Preserve scrollbar/thumbs, wrap marks, drag hitboxes and pane layout. Test own
and system frames, themes, scrollbar visibility, fullscreen/maximized and top
junction with software SDL pixel readback; a physical-display check remains
needed. Keep this separate from settings, systemd and Attach Here.
