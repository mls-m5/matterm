# Bundled JetBrains Mono

JetBrains Mono is the font from JetBrains, the company behind IntelliJ IDEA.
The pinned v2.304 files are licensed under SIL Open Font License 1.1. That license
permits bundling/embedding and redistribution with software, including commercial
software, provided the copyright notice and license accompany the font. The font
must not be sold by itself or relicensed. Font license obligations do not require
licensing the application under OFL. We distribute the font unmodified.

Upstream copyright: Copyright 2020 The JetBrains Mono Project Authors.
License: https://github.com/JetBrains/JetBrainsMono/blob/v2.304/OFL.txt

## Build and Layout

With applications enabled, MATTERM_BUNDLE_FONT defaults ON. During CMake configure,
cmake/BundledFont.cmake fetches Regular TTF and OFL.txt from immutable revision
cd5227bd1f61dff3bbd6c814ceaf7ffd95e947d9 (v2.304), verifies SHA-256 for both files,
and caches them under build/_deps/jetbrains-mono-2.304. Reconfiguring uses valid
cached files without another download. Network, TLS, or checksum failures are
explicit errors, not silent fallback to unverified content.

The build copies both files to assets/fonts/ next to each executable. Installation
places executables under bin/ and the same relative assets under bin/assets/fonts/.
This deliberately favors a relocatable bundle rather than system-wide font
installation. Copy the entire directory, not only the executable. The font is not
registered in the operating system. SDL_GetBasePath finds assets independently of
the current working directory; each program still accepts an explicit first font
argument to override the bundled default.

```sh
cmake -S . -B build
cmake --build build
./build/matterm
./build/matterm-render-demo
./build/matterm-lib-demo
cmake --install build --prefix /path/to/package
```

For offline builds, provide the pinned files named JetBrainsMono-Regular.ttf and
OFL.txt in one directory; hashes are still verified:

```sh
cmake -S . -B build -DMATTERM_FONT_SOURCE_DIR=/path/to/pinned-font-assets
```

MATTERM_FONT_CACHE_DIR can select a reusable download cache. To opt out entirely,
configure with -DMATTERM_BUNDLE_FONT=OFF and pass a font path at runtime. Library-only
builds (both application/demo options OFF) do not download font assets. Tests use
the pinned downloaded font when TEST_FONT_FILE is empty and applications bundle it.

## Font Size and Desktop Scaling

`matterm --font-size 18` sets the nominal font size in renderer pixels (18 is
also the default). SDL_ttf opens the font at a fixed 72 DPI, so the numeric
argument maps directly to pixels in the render target. The actual height of
letters and rows depends on font metrics. The app does not currently adjust font
size for desktop text scaling or the window's HiDPI backing scale.

GNOME Terminal specifies font sizes in points through GTK/Pango. At a typical
96 DPI, 12 points corresponds to 16 logical pixels before any additional
scaling. The desktop scale and font choice can affect how big the text looks;
there is no universal 2:1 conversion. For benchmark comparisons, match the
terminal grid dimensions and record font sizes and display scaling separately.

## System Font Fallback

On Linux, a build with Fontconfig looks up missing code points among installed
fonts, preferring monospace faces. For example, the bundled JetBrains Mono lacks
Braille Patterns (U+2800–U+28FF), but DejaVu Sans supplies them. The primary
font still determines the grid. If there is no usable fallback, matterm draws
its usual replacement glyph. This is per-code-point fallback, not text shaping,
emoji sequence handling, or wide-cell support.

Install Fontconfig development files and a suitable system font (Debian/Ubuntu:
`libfontconfig-dev` and `fonts-dejavu-core`), then reconfigure and rebuild. Builds
without Fontconfig, or with `-DMATTERM_SYSTEM_FONT_FALLBACK=OFF`, use the primary
font and its replacement glyph only. Install the fonts where matterm runs,
not inside a container that only runs a terminal application. Use
`fc-list ':charset=280b' family` to check which local fonts cover the `⠋` glyph.
To run the Braille regression test with
JetBrains Mono as the test font and a covering system font installed:

```sh
GLYPH_TEST_EXPECT_BRAILLE_FALLBACK=1 ctest --test-dir build -R '^glyph_atlas$' --output-on-failure
```

Current bundled assets contain only Regular. Bold/italic are SDL_ttf synthetic
variants, not separately bundled face files. Browser asset preloading, native
platform app bundles, fallback fonts, and distro-specific shared-data installation
may need separate packaging rules later. This does not install an application icon
or desktop entry.
