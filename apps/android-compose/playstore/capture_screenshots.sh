#!/usr/bin/env bash
# Play Store screenshots from a running emulator or device, written straight
# into fastlane's metadata folders at Play-legal sizes (exact 9:16, 24-bit,
# no alpha). Adapted from the eqm project's capture script.
#
#   ./capture_screenshots.sh size phone        # set the display, clean status bar
#   ./capture_screenshots.sh shot phone 01_notes
#   ./capture_screenshots.sh reset             # restore the display
#
# Profiles (Play enforces EXACT 9:16 or 16:9):
#   phone    1080x1920 @420  -> phoneScreenshots      (each side 320-3840)
#   tablet7  1152x2048 @280  -> sevenInchScreenshots  (each side 320-3840)
#   tablet10 1440x2560 @320  -> tenInchScreenshots    (each side 1080-7680)
#
# `wm size` makes the app lay itself out at the target size, so a capture needs
# no cropping. With several devices attached, set ANDROID_SERIAL.
set -euo pipefail

SDK="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
ADB="$SDK/platform-tools/adb"
IMAGES="$(cd "$(dirname "$0")" && pwd)/metadata/android/en-US/images"

profile() {
  case "$1" in
    phone)    W=1080; H=1920; DENSITY=420; DIR=phoneScreenshots ;;
    tablet7)  W=1152; H=2048; DENSITY=280; DIR=sevenInchScreenshots ;;
    tablet10) W=1440; H=2560; DENSITY=320; DIR=tenInchScreenshots ;;
    *) echo "profile must be phone, tablet7 or tablet10" >&2; exit 1 ;;
  esac
}

demo() {  # Android demo mode: full battery and signal, clock 09:41, no notifications
  "$ADB" shell settings put global sysui_demo_allowed 1
  for args in "enter" "clock -e hhmm 0941" "battery -e level 100 -e plugged false" \
              "network -e wifi show -e level 4 -e mobile show -e level 4" "notifications -e visible false"; do
    # shellcheck disable=SC2086
    "$ADB" shell am broadcast -a com.android.systemui.demo -e command $args >/dev/null
  done
}

case "${1:?size|shot|reset}" in
  size)
    profile "${2:?profile}"
    "$ADB" shell wm size "${W}x${H}"
    "$ADB" shell wm density "$DENSITY"
    sleep 2
    demo
    ;;
  shot)
    profile "${2:?profile}"
    NAME="${3:?name}"
    mkdir -p "$IMAGES/$DIR"
    RAW="$(mktemp -t shot).png"
    "$ADB" exec-out screencap -p > "$RAW"
    python3 - "$RAW" "$IMAGES/$DIR/$NAME.png" "$W" "$H" <<'PY'
import sys
from PIL import Image
src, dst, W, H = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
im = Image.open(src).convert("RGB")                  # Play rejects alpha
w, h = im.size
tar, cur = W / H, w / h
if abs(cur - tar) > 0.001:                            # centre-crop to 9:16 if needed
    if cur > tar:
        nw = int(h * tar); im = im.crop(((w - nw) // 2, 0, (w - nw) // 2 + nw, h))
    else:
        nh = int(w / tar); im = im.crop((0, (h - nh) // 2, w, (h - nh) // 2 + nh))
im.resize((W, H), Image.LANCZOS).save(dst, "PNG", optimize=True)
print(f"wrote {dst} {W}x{H}")
PY
    rm -f "$RAW"
    ;;
  reset)
    "$ADB" shell am broadcast -a com.android.systemui.demo -e command exit >/dev/null
    "$ADB" shell wm size reset
    "$ADB" shell wm density reset
    ;;
  *) echo "usage: $0 size <profile> | shot <profile> <name> | reset" >&2; exit 1 ;;
esac
