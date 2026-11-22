# Dependencies

What is installed on the development machine (Debian 12 "bookworm"), and what each part of the project
needs. Package names are Debian/Ubuntu apt packages; versions are what is installed now.

## Build the native app, server and daemon

```
sudo apt install build-essential cmake git \
    libsdl2-dev libsdl2-ttf-dev libfontconfig-dev libfreetype-dev libharfbuzz-dev \
    libboost-dev libsodium-dev libc6-dev python3
git submodule update --init            # lib/fast-json, test/mls-unit-test, matperf
cmake -B build -DMATTERM_BUILD_SERVER=ON && cmake --build build -j8
```

`MATTERM_BUILD_SERVER` is OFF by default. `matterm-server` and `matterm-daemon` are only built with it ON, and without the
daemon the native app falls back to running programs in its own process (with a note). Keep it ON in a dev build.

| Need | Package | Installed | Used for |
|---|---|---|---|
| Compiler | `g++` (build-essential), C++20 | 12.2 | everything |
| CMake >= 3.25 | `cmake` | 3.25.1 | build |
| SDL2 >= 2.0.18, SDL2_ttf >= 2.0.18 | `libsdl2-dev`, `libsdl2-ttf-dev` | 2.26.5 / 2.20.1 | native window |
| Fontconfig, FreeType, HarfBuzz | `libfontconfig-dev`, `libfreetype-dev`, `libharfbuzz-dev` | 2.14 / 2.12 / 6.0 | fonts and text |
| Boost >= 1.74 (headers) | `libboost-dev` | 1.74 | Beast/Asio in server, daemon, chatd |
| libsodium | `libsodium-dev` | 1.0.18 | server/daemon auth and keys |
| libutil (`forkpty`) | in `libc6-dev` | glibc 2.36 | PTYs |
| Static daemon | `libc6-dev` (`libc.a`) and `libsodium-dev` (`libsodium.a`) | | `matterm-daemon` linked fully static (`MATTERM_STATIC_DAEMON`, ON by default; without them the build warns and links dynamically) |
| Python 3 | `python3` | 3.11 | a few tests (fake ssh in `test/fakessh.py`, helpers) |
| Submodules | `git submodule update --init` | | `lib/fast-json` (server config; the build stops without it), `test/mls-unit-test`, `matperf` |

The container build for the chat server uses `-DBUILD_TESTING=OFF` (no test submodule in a `git archive`).

## Build the browser client (WebAssembly)

- **Emscripten SDK** (not an apt package): `git clone https://github.com/emscripten-core/emsdk`, then
  `emsdk install <version>` and `emsdk activate <version>`. Installed here: emsdk in `~/emsdk`,
  emscripten **7.0.0** (the Pages workflow installs `latest`, see `.github/workflows/pages.yml`).
- Configure and build: `emcmake cmake -B build-web && cmake --build build-web -j8`
  (toolchain file `$EMSDK/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake`). Output: `build-web/site`.

## Browser end-to-end tests

```
sudo apt install nodejs        # or any recent Node; installed: v24.14.0 in /usr/local/bin
mkdir -p ~/pw && cd ~/pw && npm install playwright-core      # installed: 1.65.1
npx playwright-core install chromium                          # browser in ~/.cache/ms-playwright
NODE_PATH=$HOME/pw/node_modules MATTERM_DAEMON=$PWD/build/matterm-daemon \
  sh web/tools/run-e2e.sh $PWD/build/src/server/matterm-server build-web/site /tmp/e2eout
```

Use `build/src/server/matterm-server`, not `build/server` (stale). `e2e-local.js` is skipped without
`MATTERM_DAEMON`.

## Optional tools

| Tool | Package | Installed | Used for |
|---|---|---|---|
| clang-tidy | `clang-tidy` | 14 | static checks (weekly CI workflow, `.github/workflows/clang-tidy.yml`; also needs `ninja-build`) |
| clang-format | `clang-format` | 14 | formatting |
| ssh client | `openssh-client` | | `matterm --daemon-host` (and a real `sshd` on the other computer to test it) |
| Ninja | `ninja-build` | | the clang-tidy workflow builds with `-G Ninja` |
| gdb / perf | `gdb`, `linux-perf` | | profiling (`bench/cpu-measure.sh`, `docs/CPU.md`) |

## Running on another computer

- `matterm-daemon` linked static needs nothing there (`matterm --daemon-host` copies it, see `docs/REMOTE_DAEMON.md`).
  A dynamic one needs `libsodium23`.
- Native app at runtime: `libsdl2-2.0-0 libsdl2-ttf-2.0-0 libfontconfig1 libfreetype6 libharfbuzz0b`.
- A display is needed for the native app; agent sandboxes have none (only `SDL_VIDEODRIVER=dummy` runs).
