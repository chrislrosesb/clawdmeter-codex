#!/bin/bash
# Install the Phase 2 Claude/Codex/Pluribus state relay on the Mac mini.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SERVICE_LABEL="com.user.clawdmeter-relay"
PLIST_SRC="$SCRIPT_DIR/daemon/$SERVICE_LABEL.plist"
PLIST_DST="$HOME/Library/LaunchAgents/$SERVICE_LABEL.plist"
VENV_DIR="$SCRIPT_DIR/daemon/.venv"
RELAY_PY="$SCRIPT_DIR/daemon/relay_server.py"
CONFIG_DIR="$HOME/.config/claude-usage-monitor"
CONFIG_FILE="$CONFIG_DIR/relay-server.json"
LOG_DIR="$HOME/Library/Logs"
LOG_OUT="$LOG_DIR/clawdmeter-relay.out.log"
LOG_ERR="$LOG_DIR/clawdmeter-relay.err.log"
PORT="${CLAWDMETER_RELAY_PORT:-8765}"

py_ge_310() { "$1" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)' >/dev/null 2>&1; }

PYTHON3=""
for cand in \
    "$(command -v python3.13 || true)" "$(command -v python3.12 || true)" \
    "$(command -v python3.11 || true)" "$(command -v python3.10 || true)" \
    /opt/homebrew/bin/python3 /usr/local/bin/python3 "$(command -v python3 || true)"; do
    [ -n "$cand" ] && [ -x "$cand" ] || continue
    if py_ge_310 "$cand"; then PYTHON3="$cand"; break; fi
done
[ -n "$PYTHON3" ] || { echo "Error: Python 3.10+ is required (brew install python)."; exit 1; }

TAILSCALE="$(command -v tailscale || true)"
if [ -z "$TAILSCALE" ] && [ -x /Applications/Tailscale.app/Contents/MacOS/Tailscale ]; then
    TAILSCALE=/Applications/Tailscale.app/Contents/MacOS/Tailscale
fi
[ -n "$TAILSCALE" ] || { echo "Error: install and sign into Tailscale first."; exit 1; }
TAIL_IP="$($TAILSCALE ip -4 2>/dev/null | head -1)"
[ -n "$TAIL_IP" ] || { echo "Error: Tailscale has no active IPv4 address."; exit 1; }
[[ "$PORT" =~ ^[0-9]+$ ]] && [ "$PORT" -ge 1 ] && [ "$PORT" -le 65535 ] || {
    echo "Error: CLAWDMETER_RELAY_PORT must be 1..65535."; exit 1;
}

echo "=== Clawdmeter Mac-mini relay install ==="
echo "Using Tailscale address $TAIL_IP and port $PORT"

if [ -d "$VENV_DIR" ] && ! py_ge_310 "$VENV_DIR/bin/python"; then
    echo "Existing daemon venv is older than Python 3.10; recreating it."
    rm -rf "$VENV_DIR"
fi
if [ ! -d "$VENV_DIR" ]; then
    "$PYTHON3" -m venv "$VENV_DIR"
fi
"$VENV_DIR/bin/pip" install --quiet --upgrade pip
"$VENV_DIR/bin/pip" install --quiet "bleak>=0.22" "httpx>=0.27" "Pillow>=10.0"
PYTHON_BIN="$VENV_DIR/bin/python"

mkdir -p "$CONFIG_DIR" "$HOME/Library/LaunchAgents" "$LOG_DIR"
TOKEN=""
if [ -f "$CONFIG_FILE" ]; then
    TOKEN="$($PYTHON_BIN - "$CONFIG_FILE" <<'PY'
import json, sys
try:
    value = json.load(open(sys.argv[1], encoding="utf-8")).get("token", "")
    print(value if isinstance(value, str) else "")
except (OSError, ValueError):
    print("")
PY
)"
fi
[ ${#TOKEN} -ge 32 ] || TOKEN="$($PYTHON_BIN -c 'import secrets; print(secrets.token_hex(32))')"

"$PYTHON_BIN" - "$CONFIG_FILE" "$TAIL_IP" "$PORT" "$TOKEN" <<'PY'
import json, os, sys
path, host, port, token = sys.argv[1:]
tmp = path + ".tmp"
with open(tmp, "w", encoding="utf-8") as handle:
    json.dump({"bind_host": host, "port": int(port), "token": token}, handle, indent=2)
    handle.write("\n")
os.chmod(tmp, 0o600)
os.replace(tmp, path)
PY

sed \
    -e "s|__PYTHON_BIN__|${PYTHON_BIN}|g" \
    -e "s|__RELAY_PATH__|${RELAY_PY}|g" \
    -e "s|__CONFIG_PATH__|${CONFIG_FILE}|g" \
    -e "s|__REPO_DIR__|${SCRIPT_DIR}|g" \
    -e "s|__LOG_OUT__|${LOG_OUT}|g" \
    -e "s|__LOG_ERR__|${LOG_ERR}|g" \
    -e "s|__HOME__|${HOME}|g" \
    "$PLIST_SRC" > "$PLIST_DST"

launchctl unload "$PLIST_DST" 2>/dev/null || true
launchctl load -w "$PLIST_DST"
sleep 2

STATE_URL="http://${TAIL_IP}:${PORT}/v1/state"
if ! curl --fail --silent --show-error \
    -H "Authorization: Bearer $TOKEN" "$STATE_URL" >/dev/null; then
    echo "Relay did not answer yet. Inspect: $LOG_ERR"
    exit 1
fi

echo ""
echo "Relay is running. Keep this token private; it is not stored in Git."
echo "Work-Mac relay URL:   $STATE_URL"
echo "Work-Mac relay token: $TOKEN"
echo "Logs: $LOG_OUT"
