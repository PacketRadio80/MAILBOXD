#!/usr/bin/env bash
# Local test: mock ARDOPC + MailboxD ardop plugin (no Main circuit required).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${ROOT}/build"
MAILBOXD="${BUILD}/src/mailboxd"
MOCK_PORT=18515
TMPDIR="${ROOT}/local/test-ardop-$$"
mkdir -p "${TMPDIR}"

cleanup() {
    kill "${MOCK_PID:-}" 2>/dev/null || true
    kill "${CIRCUIT_PID:-}" 2>/dev/null || true
    kill "${MAILBOXD_PID:-}" 2>/dev/null || true
    rm -rf "${TMPDIR}"
}
trap cleanup EXIT

if [[ ! -x "${MAILBOXD}" ]]; then
    echo "Build mailboxd first: cmake --build build" >&2
    exit 1
fi

cat > "${TMPDIR}/mailboxd.ini" <<EOF
[service]
name = test-ardop

[storage]
path = ${TMPDIR}/data

[auth]
auto_login = no

[networks]
ax25 = no
ardop = yes
circuit = no

[transport.telnet]
enabled = no

[transport.ardop]
enabled = yes
ardop_host = 127.0.0.1
ardop_port = ${MOCK_PORT}
mycall = TEST-0
radio_profile = cb
arq_bandwidth = 500MAX
listen = yes
circuit_host = 127.0.0.1
circuit_port = 17323
link_id = test-ardop
EOF

python3 "${ROOT}/scripts/mock-ardopc.py" "${MOCK_PORT}" &
MOCK_PID=$!
sleep 0.3

# Minimal TCP acceptor so circuit connect does not block startup (no HBX handshake).
nc -l 127.0.0.1 17323 >/dev/null 2>&1 &
CIRCUIT_PID=$!
sleep 0.2

LOG="${TMPDIR}/mailboxd.log"
stdbuf -oL timeout 5 "${MAILBOXD}" -c "${TMPDIR}/mailboxd.ini" >"${LOG}" 2>&1 &
MAILBOXD_PID=$!
sleep 3
kill "${MAILBOXD_PID}" 2>/dev/null || true
wait "${MAILBOXD_PID}" 2>/dev/null || true
kill "${CIRCUIT_PID}" 2>/dev/null || true

grep -q "connected to external ARDOPC" "${LOG}" || {
    echo "FAIL: no ARDOPC connect log" >&2
    cat "${LOG}" >&2
    exit 1
}
grep -q "host init sent" "${LOG}" || {
    echo "FAIL: no host init log" >&2
    cat "${LOG}" >&2
    exit 1
}
grep -q "CB half-duplex profile" "${LOG}" || {
    echo "FAIL: no CB profile log" >&2
    cat "${LOG}" >&2
    exit 1
}

echo "OK: ardop plugin local test passed"
cat "${LOG}" | grep '\[ardop\]'
