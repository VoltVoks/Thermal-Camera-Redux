# Mini640 / 2bdf:0102 Notes

## Device Summary

- USB ID: `2bdf:0102`
- Linux name: `USB Camera: UVC Camera`
- Main thermal stream used here: `/dev/video2`
- Stable camera path: `/dev/v4l/by-id/usb-Camera_USB_Camera_EA5243009-video-index0`

## What Was Wrong

- The original `TC001`-style viewer assumed a different frame format and decoded the camera as if it exposed direct Kelvin-style thermal words.
- That produced obviously wrong temperatures like `245 C` and `474 F`.
- The camera does not behave like the TC001 family on Linux through plain V4L2/UVC.

## What We Found

- `256x196` is the most useful mode for this camera.
- In that mode, the frame is:
  - `256x192` rows of thermal image payload
  - `4` extra rows of metadata/padding
- Metadata rows include stable markers like `0xccdd 0xaabb`.
- The previous metadata-range mapping looked plausible visually but produced bad temperatures in practice.

## Current Viewer

- Main file: `src/mini640_viewer.cpp`
- Unified launcher: `run-thermal-camera.sh`
- Mini640 launcher: `run-mini640.sh`
- TC001 fallback launcher: `run-tc001.sh`

Run:

```bash
/home/iliam/projects/active/tc001-redux-patched/run-thermal-camera.sh --camera-profile mini640
```

## Current Temperature Strategy

- The default Mini640 model is now a persisted affine raw-count model:

```text
temperature_celsius = (raw_pixel_value * slope) + offset_celsius
```

- Without calibration references, the default starts at the old rough fit:

```text
slope = 1 / 160.0
offset_celsius = 0.0
```

- Calibration is saved per device under:

```text
~/.config/thermal-camera-redux/EA5243009-calibration.json
```

- Add a thermometer-backed reference while aiming the center crosshair/ROI at the measured target:

```bash
/home/iliam/projects/active/tc001-redux-patched/run-thermal-camera.sh \
  --camera-profile mini640 \
  --capture-reference 23.4
```

- One reference adjusts offset. Two or more references fit slope and offset. This is still more practical than:
  - the old TC001 decode
  - the metadata min/max interpolation path

## Current Controls

- `q`: quit
- `j` / `m`: next / previous palette
- `t`: toggle Celsius / Fahrenheit
- `r`: cycle temperature source
  - `raw affine C`
  - `meta/10 F`
  - `meta/10 C`
  - `raw counts`
- `8` / `9`: rotate clockwise / counterclockwise
- `x` / `y`: flip horizontal / vertical
- `-` / `=`: calibration offset by `0.5 C`
- `0`: reset offset
- `[` / `]`: zoom out / in
- `h`: HUD
- `d`: metadata debug
- `space`: freeze
- `f`: fullscreen

## Current Status

- Display is now intentionally rotated `90` degrees clockwise by default.
- Mouse/crosshair mapping follows rotation and flips correctly.
- The viewer accepts `256x196`, `256x392`, and `256x192`-style raw frames as long as the payload is `256` columns of `YUYV`/`CV_8UC2`.
- For `256x392`, it scans candidate frame blocks and prefers the block with the `0xccdd 0xaabb` metadata marker.
- Temperature now uses a persisted calibration model and can be improved with thermometer references.
- Metadata-derived temperatures remain visible as experimental modes, not the trusted default.
- Build output now goes under `build/` instead of `src/`.

## Remaining Open Question

- Full vendor-side USB control and exact radiometric decoding are still unresolved.
- Read-only UVC Extension Unit probing found unit `10`, controls `1..23`, with selector `4` returning `2.0`-like data and selectors `7..23` returning 7-byte payloads. Do not write `UVC_SET_CUR` commands until selector meanings are identified.
