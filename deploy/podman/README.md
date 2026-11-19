# Podman deployment (provisional)

A locked-down container for testing `matterm-server` on the laptop and later on
the public server, using the same files. The server and the flags used here,
including `--trusted-proxy` / `--trusted-proxy-hops`, exist; no `--health-check`
mode exists. `test/serversecurity_test.cpp` parses these command lists with the
real flag parser.
None of this has been run. The files parse with podman-compose 1.0.3 (Debian 12),
which needs `-p matterm` (it ignores the `name:` key) and does not support nested
`${A:-${B}}` defaults, so the host paths are fixed to `~/.config/matterm` and
`~/.local/state/matterm`. Containers cannot be started inside the agent's
sandbox, so everything past parsing is untested.

## Layout

```
browser --> 127.0.0.1:8080 (published, loopback only) --> matterm (server + shell)
                                                          internal network, no egress
```

One container, one port. It holds the server and the shell the browser user
gets. It sits on an `internal` network: no gateway, no route to the internet or
the LAN, so the shell cannot download tools or reach other machines.

- The root filesystem is read-only, all capabilities are dropped,
  `no-new-privileges` is set, and pids/memory/CPU are limited. The shell's home
  is a 64 MiB tmpfs that disappears with the container.
- Host `~/.config/matterm` (read-only while running) and `~/.local/state/matterm`
  (read-write) are mounted in, so the container uses the same layout as a native
  install. See "Open" in docs/WEB_TERMINAL.md about separating them.
  Note the shell can read the config directory, including the password hash.
- The laptop setup disables proxy trust (`none`); gateway-address accounting is
  sufficient for local testing. The public override enables `--trusted-proxy any`
  with zero skipped hops: nginx overwrites `X-Forwarded-For` with its client's
  IP, which the server uses for lockout/global-budget classification. An exact
  gateway/proxy IP is preferable when stable. `any` is safe ONLY if network
  isolation and host-loopback publishing make the proxy the only caller; it is
  not safe merely because TLS or an Origin check exists. Never publish this
  backend publicly or attach unrelated containers to its network. nginx's
  per-client `/login` limit remains defence in depth.

### Network isolation (unverified)

Podman should accept a published port on an `internal` network, but this has not
been tried. If it does not work, run the container on a normal network and block
its outbound traffic on the host instead, for example with an nftables rule
dropping forwarded packets from the container subnet to anywhere but the host.
Check that the shell really has no network: `curl https://example.org` must fail.

## Laptop

```sh
mkdir -p ~/.config/matterm ~/.local/state/matterm
export MATTERM_UID=$(id -u) MATTERM_GID=$(id -g)
podman compose -p matterm -f deploy/podman/compose.yaml build
podman compose -p matterm -f deploy/podman/compose.yaml --profile setup run --rm setup
podman compose -p matterm -f deploy/podman/compose.yaml up
# open http://127.0.0.1:8080
```

## Public server

The host's nginx terminates TLS; copy `nginx-host.conf.example` into its
configuration and adjust names and certificates. It forwards to
`127.0.0.1:8081`, the loopback-published port of the container.

```sh
export MATTERM_DOMAIN=term.example.org MATTERM_PORT=8081
podman compose -f deploy/podman/compose.yaml -f deploy/podman/compose.server.yaml up -d
```

Only do this after the hardening phases and with the rate limiting settings
reviewed. The server container still has no outbound network access.
