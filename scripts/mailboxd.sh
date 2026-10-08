#!/bin/sh
# Start mailboxd only. Build and install with CMake (see README).
#
# Default install (source tree):  ./bin/mailboxd-start
# Prefix install:                 /usr/local/mailboxd/mailboxd-start  (or $HOME/mailboxd/…)
# Dev (no install):               ./scripts/mailboxd.sh
#
# Detached:   mailboxd-start --screen | --tmux [--attach]
set -e

self="${0#./}"
case "$self" in
    /*) script_path="$self" ;;
    *)  script_path="$(pwd)/$self" ;;
esac
script_dir="$(cd "$(dirname "$script_path")" && pwd)"
script_name="$(basename "$script_path")"

# Installed layout: <tree>/mailboxd-start + <tree>/mailboxd  (./bin or <prefix>/mailboxd)
if [ "$script_name" = "mailboxd-start" ] && [ -x "$script_dir/mailboxd" ]; then
    prefix="$script_dir"
    config="${MAILBOXD_CONFIG:-$prefix/mailboxd.ini}"
    binary="$script_dir/mailboxd"
    export MAILBOXD_ROOT="${MAILBOXD_ROOT:-$prefix}"
    cd "$prefix"
    if [ ! -f "$config" ]; then
        echo "Config not found: $config" >&2
        exit 1
    fi
    if [ "$#" -eq 0 ]; then
        exec "$binary" -c "$config"
    fi
    exec "$binary" -c "$config" "$@"
fi

# Explicit or known install roots
root="$(cd "$script_dir/.." && pwd)"
if [ -n "$MAILBOXD_ROOT" ]; then
    prefix="$MAILBOXD_ROOT"
elif [ -x "$root/bin/mailboxd" ]; then
    prefix="$root/bin"
elif [ -x "/usr/local/mailboxd/mailboxd" ]; then
    prefix="/usr/local/mailboxd"
elif [ -n "$HOME" ] && [ -x "$HOME/mailboxd/mailboxd" ]; then
    prefix="$HOME/mailboxd"
else
    prefix=""
fi

if [ -n "$prefix" ]; then
    config="${MAILBOXD_CONFIG:-$prefix/mailboxd.ini}"
    binary="$prefix/mailboxd"
    if [ -x "$binary" ] && [ -f "$config" ]; then
        export MAILBOXD_ROOT="${MAILBOXD_ROOT:-$prefix}"
        cd "$prefix"
        if [ "$#" -eq 0 ]; then
            exec "$binary" -c "$config"
        fi
        exec "$binary" -c "$config" "$@"
    fi
fi

# Development tree (repo checkout, no cmake --install yet)
config="${MAILBOXD_CONFIG:-$root/local/mailboxd.ini}"
build="$root/build/src/mailboxd"

if [ -f "$config" ] && [ -x "$build" ]; then
    export MAILBOXD_ROOT="${MAILBOXD_ROOT:-$root}"
    cd "$root"
    if [ "$#" -eq 0 ]; then
        exec "$build" -c "$config"
    fi
    exec "$build" -c "$config" "$@"
fi

echo "MailboxD not found." >&2
echo "Build and install (default → ./bin):" >&2
echo "  cmake -B build -DCMAKE_BUILD_TYPE=Release" >&2
echo "  cmake --build build" >&2
echo "  cmake --install build" >&2
echo "  ./bin/mailboxd-start" >&2
echo "System prefix:" >&2
echo "  cmake -B build -DCMAKE_INSTALL_PREFIX=/usr/local" >&2
echo "  cmake --build build && sudo cmake --install build" >&2
echo "  /usr/local/mailboxd/mailboxd-start" >&2
exit 1
