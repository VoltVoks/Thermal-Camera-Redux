#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$ROOT/build"
MINI_SRC="$ROOT/src/mini640_viewer.cpp"
MINI_BIN="$BUILD_DIR/mini640-viewer"
REDUX_BIN="$BUILD_DIR/redux"
DEFAULT_MINI_DEVICE="/dev/v4l/by-id/usb-Camera_USB_Camera_EA5243009-video-index0"

profile="auto"
declare -a passthrough=()

while (($#)); do
  case "$1" in
    --camera-profile|--profile)
      if (($# < 2)); then
        echo "$1 needs a value" >&2
        exit 2
      fi
      profile="$2"
      shift 2
      ;;
    --camera-profile=*|--profile=*)
      profile="${1#*=}"
      shift
      ;;
    *)
      passthrough+=("$1")
      shift
      ;;
  esac
done

if [[ "$profile" == "auto" ]]; then
  if [[ -e "$DEFAULT_MINI_DEVICE" ]] || lsusb -d 2bdf:0102 >/dev/null 2>&1; then
    profile="mini640"
  else
    profile="tc001"
  fi
fi

mkdir -p "$BUILD_DIR"

needs_rebuild() {
  local bin="$1"
  shift
  [[ ! -x "$bin" ]] && return 0
  local src
  for src in "$@"; do
    [[ "$src" -nt "$bin" ]] && return 0
  done
  return 1
}

build_mini640() {
  if needs_rebuild "$MINI_BIN" "$MINI_SRC"; then
    g++ -std=c++17 -O2 -Wall -Wextra -o "$MINI_BIN" "$MINI_SRC" \
      $(pkg-config --cflags opencv4) \
      -lopencv_core -lopencv_imgproc -lopencv_highgui -lopencv_videoio
  fi
}

build_redux() {
  if needs_rebuild "$REDUX_BIN" \
      "$ROOT/src/tc001.cpp" "$ROOT/src/thread.cpp" "$ROOT/src/thread.h" \
      "$ROOT/src/image_0.cpp" "$ROOT/src/image_1.cpp" "$ROOT/src/image_2.cpp" "$ROOT/src/image_3.cpp" \
      "$ROOT/src/therm_0.cpp" "$ROOT/src/therm_1.cpp" "$ROOT/src/therm_2.cpp"; then
    g++ -Wall -Wextra -O3 -ffast-math \
      -DBORDER_LAYOUT=0 -DDEFAULT_FONT=0 -DDEFAULT_COLORMAP=4 -DROTATION=0 \
      -DDISPLAY_WIDTH=2560 -DDISPLAY_HEIGHT=1600 -DUSE_CELSIUS=0 -DHUD_ALPHA=0.4 \
      -DUSE_ASSERT=0 -march=native -mtune=native -pipe -fomit-frame-pointer \
      $(pkg-config --cflags opencv4) \
      "$ROOT/src/tc001.cpp" "$ROOT/src/thread.cpp" \
      -o "$REDUX_BIN" -lpthread \
      -lopencv_core -lopencv_imgproc -lopencv_highgui -lopencv_videoio -lopencv_imgcodecs
  fi
}

case "$profile" in
  mini640)
    build_mini640
    exec "$MINI_BIN" --camera-profile mini640 "${passthrough[@]}"
    ;;
  tc001)
    build_redux
    exec "$REDUX_BIN" --camera-profile tc001 "${passthrough[@]}"
    ;;
  *)
    echo "Unknown camera profile '$profile' (expected auto, mini640, tc001)" >&2
    exit 2
    ;;
esac
