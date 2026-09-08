#!/bin/bash
# Configure this checkout as the Phase 2 work-Mac BLE receiver.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CONFIG_FILE="$HOME/.config/claude-usage-monitor/config"
RELAY_URL="${1:-}"

if [ -z "$RELAY_URL" ]; then
    read -r -p "Mac-mini relay URL (http://TAILSCALE_IP:8765/v1/state): " RELAY_URL
fi
case "$RELAY_URL" in
    http://*|https://*) ;;
    *) echo "Error: relay URL must start with http:// or https://"; exit 1 ;;
esac
[[ "$RELAY_URL" =~ [[:space:]] ]] && { echo "Error: relay URL cannot contain spaces."; exit 1; }

TOKEN="${CLAWDMETER_RELAY_TOKEN:-}"
if [ -z "$TOKEN" ]; then
    read -r -s -p "Mac-mini relay token: " TOKEN
    echo ""
fi
[ ${#TOKEN} -ge 32 ] || { echo "Error: relay token is too short."; exit 1; }
[[ "$TOKEN" =~ [[:space:]] ]] && { echo "Error: relay token cannot contain spaces."; exit 1; }

echo "Checking the authenticated Tailscale state feed..."
curl --fail --silent --show-error \
    -H "Authorization: Bearer $TOKEN" "${RELAY_URL%/}" >/dev/null
echo "Relay reachable. Saving receiver settings..."

mkdir -p "$(dirname "$CONFIG_FILE")"
touch "$CONFIG_FILE"
upsert() {
    local key="$1" value="$2"
    grep -vE "^[[:space:]]*$key[[:space:]]*=" "$CONFIG_FILE" > "$CONFIG_FILE.tmp" 2>/dev/null || true
    mv "$CONFIG_FILE.tmp" "$CONFIG_FILE"
    echo "$key = $value" >> "$CONFIG_FILE"
}
upsert relay_url "${RELAY_URL%/}"
upsert relay_token "$TOKEN"
upsert now_playing on
chmod 600 "$CONFIG_FILE"

echo "Installing the local BLE + Apple Music daemon..."
echo ""

"$SCRIPT_DIR/install-mac.sh"

echo ""
echo "=== Work-Mac receiver configured ==="
echo "Claude, Codex, and Pluribus will come from the Mac mini."
echo "Apple Music will be read only from this Mac."
echo "Do not pair the Clawdmeter until the Mac-mini BLE daemon has been stopped."
