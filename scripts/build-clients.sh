#!/usr/bin/env sh
# Build mailboxd-telnet, mailboxd-terminal, and mailboxd-ssh (when libssh is available)
# without the mailboxd server or plugins.
set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/build-clients}"

shift 2>/dev/null || true

cmake -B "$BUILD" -DMAILBOXD_CLIENTS_ONLY=ON "$@"
cmake --build "$BUILD"

echo "Clients:"
echo "  $BUILD/src/clients/mailboxd-telnet"
echo "  $BUILD/src/clients/mailboxd-terminal"
if [ -x "$BUILD/src/clients/mailboxd-ssh" ]; then
    echo "  $BUILD/src/clients/mailboxd-ssh"
fi
