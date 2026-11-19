#!/bin/sh
# Creates the matterm-server credentials file from a password you type.
#   tools/set-password.sh [path/to/matterm-server] [credentials-file]
# The file holds only an Argon2id hash (never the password) and gets mode 0600.
# Defaults: build/server/src/server/matterm-server and
# ${XDG_CONFIG_HOME:-$HOME/.config}/matterm/credentials.
set -eu
server=${1:-build/server/src/server/matterm-server}
file=${2:-${XDG_CONFIG_HOME:-$HOME/.config}/matterm/credentials}
[ -x "$server" ] || { echo "server binary not found: $server" >&2; exit 1; }
umask 077
mkdir -p "$(dirname "$file")"
printf 'Password (24-256 characters, a long passphrase is best): ' >&2
stty -echo 2>/dev/null || true
IFS= read -r first
stty echo 2>/dev/null || true; echo >&2
printf 'Again: ' >&2
stty -echo 2>/dev/null || true
IFS= read -r second
stty echo 2>/dev/null || true; echo >&2
[ "$first" = "$second" ] || { echo "The passwords differ." >&2; exit 1; }
rm -f "$file.new"
printf '%s\n' "$first" | "$server" --set-password --credentials "$file"
echo "Wrote $file" >&2
