# chatd in a container

The Mattrix chat server (docs/CHAT.md) as an image with one file in it. Nothing
here has been run in the development sandbox (Podman cannot start there); the
static `chatd` it contains was built and exercised there. Check the files before
you rely on them.

| File | What |
|---|---|
| `Containerfile` | Builds a fully static `chatd`, then an empty (`FROM scratch`) image with only that binary, run as user 65532. |
| `compose.yaml` | Read-only root, no capabilities, no new privileges, memory/process/CPU limits, **no volumes**, port published on **loopback only**, the operator console on `podman attach`. |
| `nginx.conf.example`, `Caddyfile.example` | TLS in front: the browser speaks `wss://`, the proxy speaks plain WebSocket to chatd on loopback. |

```sh
CHAT_ORIGIN=https://you.github.io CHAT_PROXY_IP=<proxy address as the container sees it> \
    podman-compose -f deploy/chatd/compose.yaml up -d --build
podman attach --sig-proxy=false chatd  # (Ctrl-C would otherwise stop chatd)
                           # operator commands: help, list, stats, kick, ban, mute, lock ...
                           # detach with the Podman detach keys (default Ctrl-P, Ctrl-Q)
```

Things to know:

* `--origin` is an exact `Origin` (scheme, host, port); repeat it for each site that
  uses the chat. A page on another origin cannot open the socket from a browser.
  (It is not authentication: other programs can send any header.)
* `CHAT_PROXY_IP` is **required** by compose (no default): without `--trust-proxy` every
  visitor shares the proxy's address and one person's ban or the 3-connection cap
  hits everybody. chatd also prints a warning once if a request carries
  `X-Forwarded-For` while `--trust-proxy` is unset.
* `--trust-proxy` takes the one address nginx/Caddy connects from; the container
  sees the host side of its network (for example `10.89.0.1`), not `127.0.0.1`.
  Without it, every visitor looks like the proxy and the per-address limits count
  them together.
* Which address chatd sees from the proxy depends on the Podman version and its
  network backend (rootless is the doubtful case). Check it: connect once and
  look at the source the console's `list` shows, and use that for
  `CHAT_PROXY_IP`; chatd's warning (above) tells you when it is wrong.
* The proxies' error logs (nginx error log, Caddy's stderr) can still name client
  addresses even with the access log off; see the comments in the examples.
* Nothing is stored: no volume, no logs of messages. The room is gone at restart.
  Keep the proxy's access log off or short-lived, since visitor IPs live there.
* The admin console has no login: whoever can `podman attach` is the operator.
* The image has no shell and no health check; watch the process from outside.
