#!/usr/bin/env bash
# PROTOTYPE build: compile studio-host against the CI-packaged bundle's libobs + Qt frameworks
# (no Xcode on this machine, so no local libobs build). Throwaway; see streammate-pivot #11.
set -euo pipefail
SRC="$(cd "$(dirname "$0")" && pwd)"
OBS="$HOME/Projects/streammate-studio-host/external/obs-studio"
BUNDLE_SRC=/tmp/sm-host/StreamMateStudioHost.app
FW="$BUNDLE_SRC/Contents/Frameworks"
OUT=/tmp/sm-host-build
PROTO_APP=/tmp/sm-host-proto/StreamMateStudioHost.app
mkdir -p "$OUT"
time clang++ -std=c++20 -O2 -g0 -Wno-deprecated-declarations \
  -DSTREAMMATE_HAS_LIBOBS=1 -DSTREAMMATE_HAS_CEF_QT_LOOP=1 -DQT_NO_KEYWORDS \
  -DSTREAMMATE_STUDIO_HOST_VERSION='"0.0.0-proto-preview"' \
  -I "$OBS/libobs" -I /tmp/simde -I /tmp/sm-obs-gen -I "$SRC/src" \
  -I "$FW/QtCore.framework/Headers" -I "$FW/QtGui.framework/Headers" -I "$FW/QtWidgets.framework/Headers" \
  -I /opt/homebrew/opt/jpeg-turbo/include \
  -F "$FW" -framework libobs -framework QtCore -framework QtGui -framework QtWidgets \
  -framework CoreGraphics -framework CoreFoundation \
  /opt/homebrew/opt/jpeg-turbo/lib/libturbojpeg.a \
  "$SRC/src/studio_host.cpp" "$SRC/src/native_overlay_renderer.cpp" -o "$OUT/studio-host"
if [[ ! -d "$PROTO_APP" ]]; then
  mkdir -p "$(dirname "$PROTO_APP")"
  cp -R "$BUNDLE_SRC" "$PROTO_APP"
fi
cp "$OUT/studio-host" "$PROTO_APP/Contents/MacOS/studio-host"
install_name_tool -add_rpath @executable_path/../Frameworks "$PROTO_APP/Contents/MacOS/studio-host" 2>/dev/null || true
codesign -s - -f --identifier com.streammate.studio-host "$PROTO_APP/Contents/MacOS/studio-host" 2>&1 | tail -1
echo "built: $PROTO_APP/Contents/MacOS/studio-host"
