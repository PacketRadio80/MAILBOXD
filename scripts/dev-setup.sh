#!/bin/sh
# One-shot dev environment: configure, build, IDE compile_commands symlink.
set -e

root="$(cd "$(dirname "$0")/.." && pwd)"

cmake -B "$root/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

cmake --build "$root/build"

if [ -f "$root/build/compile_commands.json" ]; then
    ln -sf build/compile_commands.json "$root/compile_commands.json"
    echo "Linked compile_commands.json -> build/compile_commands.json"
fi

echo "Ready. Install (default → ./bin): cmake --install $root/build"
echo "Start: $root/bin/mailboxd-start  or  $root/scripts/mailboxd.sh"
