# Task: server configuration file

Assignment for an independent agent in its own worktree and branch. Read first:
Agents.md, docs/PARALLEL_WORK.md, docs/WEB_TERMINAL.md section 5.6 (design of
the configuration), docs/SERVER.md, `src/server/server.h` (`ServerConfig` and
the structs inside it) and `src/server/main.cpp`. The JSON library is the
submodule `lib/fast-json` (C++20, header-only, see its README and `test/`).

## Goal

`matterm-server` reads its settings from a JSON file, with command-line flags
overriding it and built-in defaults below both, as specified in 5.6.

## Scope

1. **Loader** in new files `src/server/config.h/.cpp`: parse a file into
   `ServerConfig` (including `RateLimitConfig`, `AuthConfig`, `ProcessConfig`
   and the connection limits). Members whose name starts with `_` are ignored
   at any depth. Unknown keys, wrong types, out-of-range values and duplicate
   keys are errors that name the key path (never the value of secrets). Use
   snake_case keys that match WEB_TERMINAL.md (`max_failures`, `lockout_initial`,
   `global_attempts_per_hour`, `exempt_loopback`, ...). Durations are plain
   numbers with the unit in the key name (`lockout_initial_seconds`).
2. **Paths.** Default config `$XDG_CONFIG_HOME/matterm/server.json`
   (fallback `~/.config/matterm/server.json`); a missing default file is not an
   error, a missing `--config PATH` file is. Credentials default to
   `<config dir>/credentials`. Refuse a config file that is a symlink, not owned
   by the user, or writable by group/other.
3. **CLI.** Add `--config PATH` and `--print-config` (effective values as JSON,
   with the password hash replaced by a placeholder; round-trips through the
   loader). Existing flags keep working and override the file. Keep
   `--set-password` behavior.
4. **Reference.** `docs/SERVER_CONFIG.md`: every key, its type, default, range
   and meaning, plus one lenient (laptop) and one strict (public) example file.
5. **Build.** The loader needs C++20 and fast-json; add it to
   `matterm-server-core` or a sibling target without affecting the native or web
   builds. Initialise the submodule as in the environment notes if needed; do not
   advance its pointer.

## Out of scope

Writing the config back, a state directory and persisted lockouts, TLS,
`trusted_proxy` behavior (accept and validate the keys only if the server code
already supports them), the rate limiter/auth internals, the browser client.

## Owned and shared paths

Owned: `src/server/config.*`, `test/serverconfig_test.cpp`,
`docs/SERVER_CONFIG.md`. Shared, edit minimally and report:
`src/server/main.cpp`, `src/server/CMakeLists.txt`, `test/CMakeLists.txt`,
`src/server/server.h` (only to add fields that a setting needs, with defaults
equal to today's behavior). Another agent works in `src/server/ratelimiter.*`,
`auth.*`, and `server.cpp`: do not touch those.

## Acceptance

- Tests (`test/serverconfig_test.cpp`): defaults equal today's `ServerConfig`;
  every section loads; `_` comments; each error class names its path; permission
  checks (use a temp dir); precedence file < CLI; `--print-config` round trip.
- Full suite passes in the default and server-only configurations.
- clang-format applied; no build products committed.

## Handoff

As in docs/PARALLEL_WORK.md: full commit SHAs in dependency order, changed
files, commands run and results, limitations, deferred items. Do not push.

## Status (branch agent2-config, based on main 44933e5)

Done: loader, paths, CLI (`--config`, `--print-config`), `docs/SERVER_CONFIG.md`
with two tested example files in `docs/examples/`, build integration. It
exposes the settings added by the hardening task (`global_counts_failures_only`,
`source_idle_expiry_seconds`, `timeouts`, `session`) and `static_dir`.
`password_hash` is output-only: `--print-config` writes a placeholder and the
loader accepts only that placeholder, so the hash stays in the credentials file.

fast-json findings, handled in `config.cpp` (the submodule is untouched): its
tokenizer loops forever on a negative number, accepts truncated documents,
missing commas and trailing text, reads `1.5` as `1`, cannot decode `\u`
escapes and does not detect duplicate keys. The loader therefore runs its own
strict syntax check (with line and column) before handing the text to fast-json,
rejects negative numbers, decodes strings itself and checks duplicates itself.
`--print-config` uses a small writer instead of `JsonOut`, which cannot write an
empty array. Not done: writing the config back, `trusted_proxy`, TLS.
