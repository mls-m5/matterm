# Podman deployment (provisional)

Locked-down containers for testing `matterm-server` on the laptop and later on
the public server, using the same files. The server does not exist yet
(docs/WEB_TERMINAL.md, phase 2): the `Containerfile` targets, CMake options and
the server flags used here (`--listen`, `--trusted-proxy`, `--allowed-origin`,
`--set-password`, `--health-check`) are the planned interface and may change.
None of this has been built or run; Podman was not available when it was written.

## Layout

```
browser --> proxy (Caddy, 127.0.0.1:8080) --> matterm (server + shell)
                 |                                 |
            frontend + backend networks       backend only (internal, no egress)
```

- `matterm` holds the server and the shell the browser user gets. It sits on an
  `internal` network: no gateway, no route to the internet or the LAN. The shell
  can therefore not download tools or reach other machines.
- `proxy` is the only container with a published port (loopback only on the
  laptop) and the only one attached to a routable network. It terminates TLS on
  the public server.
- Both containers run with a read-only root filesystem, all capabilities
  dropped, `no-new-privileges`, and pid/memory/CPU limits. The shell's home is a
  64 MiB tmpfs that disappears with the container.
- Host `~/.config/matterm` (read-only while running) and `~/.local/state/matterm`
  (read-write) are mounted in, so the container uses the same layout as a native
  install. See "Open questions" in docs/WEB_TERMINAL.md about separating them.
  Note the shell can read the config directory, including the password hash.

## Laptop

```sh
mkdir -p ~/.config/matterm ~/.local/state/matterm
export MATTERM_UID=$(id -u) MATTERM_GID=$(id -g)
podman compose -f deploy/podman/compose.yaml build
podman compose -f deploy/podman/compose.yaml --profile setup run --rm setup
podman compose -f deploy/podman/compose.yaml up
# open http://127.0.0.1:8080
```

## Public server

```sh
export MATTERM_DOMAIN=term.example.org MATTERM_CADDYFILE=Caddyfile.server
podman compose -f deploy/podman/compose.yaml -f deploy/podman/compose.server.yaml up -d
```

Only do this after the hardening phases and with the rate limiting settings
reviewed. The server container still has no outbound network access.
