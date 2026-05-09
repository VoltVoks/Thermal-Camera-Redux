#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
echo "Launching TC001 Redux viewer"
echo "Keys: j/m colormap, o rulers/crosshair, h HUD, t C/F, 6 fullscreen, q quit"
echo "Cal: 7/9 emissivity, [ ] reflected temp, -/= offset, 0 reset"
echo "If key input does not go to the window, click the viewer once and try again."
exec "${SCRIPT_DIR}/run-thermal-camera.sh" --camera-profile tc001 -d 2 -scale 6 -cmap 4 "$@"
