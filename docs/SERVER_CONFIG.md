# matterm-server configuration

Settings come from three layers, each overriding the one before:

1. built-in defaults (the values below),
2. the JSON config file,
3. command-line flags.

`matterm-server --print-config` prints the effective values as JSON that the
loader accepts again. The design is in [WEB_TERMINAL.md](WEB_TERMINAL.md)
section 5.6; usage is in [SERVER.md](SERVER.md).

## Files and paths

- Config file: `$XDG_CONFIG_HOME/matterm/server.json`, else
  `~/.config/matterm/server.json` (only absolute values of those variables
  count). A missing default file is not an error. `--config PATH` selects
  another file, which must exist.
- Credentials (the Argon2id hash written by `--set-password`): the
  `credentials` setting or `--credentials`, else `credentials` in the directory
  of the config file. A missing *default* credentials file only logs a warning
  (every login then fails); a missing explicit one is an error.
  `--set-password` creates the default directory with mode 0700.
- The config file is refused when it is a symbolic link, not a regular file,
  not owned by the user running the server, writable by group or others, or
  larger than 64 KiB. The password hash is never read from it (see
  `password_hash`).

## Format

Strict JSON with these rules:

- The document is one object. Syntax errors (a truncated file, a missing or
  trailing comma, text after the document) are errors with line and column.
- Members whose name starts with `_` are ignored at any depth; use them for
  comments (`"_note": "..."`). They still have to be valid JSON.
- Unknown keys, wrong types, out-of-range values and duplicate keys are errors
  that name the key path, for example
  `server.json:7:5: rate_limit.max_failurs: unknown key`. Messages never contain
  values.
- Numbers are non-negative integers (everywhere, comments included): no
  fractions, exponents or signs. Durations carry their unit in the key name.
- Strings may use every JSON escape including `\uXXXX`; they must not contain
  NUL.

A file that is rejected changes nothing.

## Keys

Top level:

| Key | Type | Default | Range / rule | Meaning |
| --- | --- | --- | --- | --- |
| `listen_address` | string | `127.0.0.1` | IPv4 or IPv6 address | Address to bind. Anything but `127.0.0.1` also needs `allow_plain_http`. |
| `port` | integer | 8080 | 0..65535 | TCP port; 0 picks a free one. |
| `allowed_origin` | string | `http://localhost:8080` | `http(s)://host[:port]`, no path | The exact `Origin` browsers must send; its authority must equal `Host`. |
| `allow_plain_http` | boolean | false | | Permit plain HTTP on a non-loopback address (prints a warning). |
| `credentials` | string | next to the config file | non-empty path | Credentials file. |
| `password_hash` | string | | `"<redacted>"` or `"<none>"` only | Output of `--print-config`. Accepted on input only as those placeholders, so a config file can never carry the hash. |
| `static_dir` | string | none | non-empty path | Directory of the browser client; empty: placeholder page only. |
| `max_connections` | integer | 32 | 1..65535 | Simultaneous connections. |
| `max_unauthenticated` | integer | 16 | 1..65535 | Simultaneous connections that have not authenticated. |

`rate_limit` (see [SERVER.md](SERVER.md#rate-limiting)):

| Key | Type | Default | Range | Meaning |
| --- | --- | --- | --- | --- |
| `max_failures` | integer | 5 | 1..1000 | Consecutive failures before a source is locked out. |
| `lockout_initial_seconds` | integer | 30 | 1..86400 | First lockout; doubles on further failures. |
| `lockout_max_seconds` | integer | 900 | 1..2592000, not below `lockout_initial_seconds` | Longest lockout. |
| `global_attempts_per_hour` | integer | 30 | 0..1000000 | Password attempts per hour across all sources; 0 disables. |
| `global_counts_failures_only` | boolean | true | | true: only failures spend the global budget; false: every attempt does. |
| `exempt_loopback` | boolean | true | | Loopback sources are exempt from the global budget. Set false behind a reverse proxy on the same host, where every client looks like loopback. |
| `max_sources` | integer | 4096 | 1..1000000 | Sources remembered at once. |
| `source_idle_expiry_seconds` | integer | 3600 | 1..2592000 | An unlocked source idle this long is forgotten. |

`auth`:

| Key | Type | Default | Range | Meaning |
| --- | --- | --- | --- | --- |
| `idle_timeout_seconds` | integer | 43200 | 1..31536000 | Session expires after this long without activity. |
| `absolute_timeout_seconds` | integer | 604800 | 1..31536000 | Session expires this long after login. |

`process` (the program each session runs; never chosen by a client):

| Key | Type | Default | Range | Meaning |
| --- | --- | --- | --- | --- |
| `arguments` | array of strings | `["/bin/sh"]` | 1..64 entries, each 1..4096 bytes | Program and arguments. `--` on the command line replaces it. |
| `environment_allow_list` | array of strings | `HOME USER LOGNAME PATH LANG LC_ALL LC_CTYPE` | 0..256 environment variable names | Variables the child inherits (plus `TERM`, `COLORTERM`). `[]` passes none. |
| `termination_grace_milliseconds` | integer | 500 | 0..60000 | Time between SIGHUP and SIGKILL to the process group. |
| `termination_drain_timeout_milliseconds` | integer | 1000 | 0..60000 | How long to wait for output to drain after the client asked to close. |

`timeouts` (see [SERVER.md](SERVER.md#deadlines-and-shutdown)):

| Key | Type | Default | Range | Meaning |
| --- | --- | --- | --- | --- |
| `request_milliseconds` | integer | 5000 | 100..600000 | Total time for an unauthenticated request and its response. |
| `websocket_handshake_milliseconds` | integer | 5000 | 100..600000 | WebSocket handshake and closing handshake. |
| `websocket_idle_milliseconds` | integer | 60000 | 1000..86400000 | No message received on an open WebSocket. |

`session` (protocol limits, see [PROTOCOL.md](PROTOCOL.md)):

| Key | Type | Default | Range | Meaning |
| --- | --- | --- | --- | --- |
| `hello_timeout_milliseconds` | integer | 5000 | 100..600000 | Time to send `Hello` after the upgrade. |
| `idle_timeout_milliseconds` | integer | 60000 | 1000..86400000 | No protocol message from the client. |
| `ack_stall_timeout_milliseconds` | integer | 30000 | 1000..86400000 | Output pending without any `Ack`. |
| `output_window_bytes` | integer | 262144 | 32768..67108864 | Unacknowledged output allowed in flight. |
| `resize_interval_milliseconds` | integer | 100 | 0..60000 | Minimum time between applied resizes. |
| `resize_hard_limit` | integer | 100 | 1..1000000 | Resizes per second beyond which the client is rate limited. |
| `rate_window_milliseconds` | integer | 5000 | 100..600000 | Window of the message rate limit. |
| `max_messages_per_window` | integer | 5000 | 1..10000000 | Client messages allowed per window. |

Not yet supported, and therefore rejected as unknown keys: `trusted_proxy`,
TLS files and scrollback sizes.

## Command-line flags

| Flag | Overrides |
| --- | --- |
| `--config PATH` | selects the config file (required to exist) |
| `--print-config` | prints the effective settings and exits |
| `--listen ADDR`, `--port N`, `--origin URL`, `--static-dir DIR` | `listen_address`, `port`, `allowed_origin`, `static_dir` |
| `--credentials PATH` | `credentials` |
| `--allow-plain-http` | sets `allow_plain_http` to true (it cannot unset it) |
| `-- PROGRAM ARG...` | `process.arguments` |
| `--set-password` | reads a password from stdin and writes the credentials file; honors `--config` and `--credentials` |

## Examples

[`examples/server-laptop.json`](examples/server-laptop.json): loopback only,
forgiving limits (ten failures, short lockouts, no global budget).

[`examples/server-public.json`](examples/server-public.json): behind a
TLS-terminating reverse proxy on the same machine. Three failures, lockouts
from one minute to an hour, ten attempts per hour globally, loopback *not*
exempt (the proxy connects from loopback), short sessions and tighter
transport timeouts. Both files are loaded by the test suite.
