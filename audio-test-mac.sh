#!/bin/bash
set -euo pipefail
project_dir="$(cd "$(dirname "$0")" && pwd)"
for candidate in "$project_dir/daemon/.venv/bin/python" "$HOME/.platformio/penv/bin/python" python3; do
    if "$candidate" -c 'import serial' >/dev/null 2>&1; then
        exec "$candidate" "$project_dir/tools/capture_audio.py" "$@"
    fi
done
echo "Python with pyserial is required. Use this project's existing daemon or PlatformIO environment." >&2
exit 1
