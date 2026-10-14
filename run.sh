#!/bin/sh
# Runs the game from the build with your disc image (CTTR_DISC or local.json's path), passing the options on.
DISC="${CTTR_DISC:-$(python3 -c 'import json;print(json.load(open("local.json"))["disc"])' 2>/dev/null)}"
APP="$(dirname "$0")/build/crash-cttr.app/Contents/MacOS/crash-cttr"
[ -x "$APP" ] || APP="$(dirname "$0")/build/crash-cttr"
exec "$APP" --disc "$DISC" "$@"
