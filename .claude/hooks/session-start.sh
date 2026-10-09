#!/bin/bash
# Cloud sessions start from a bare Ubuntu image: install the toolchain from
# the README's "Building" section, then configure and build so tests can run
# straight away. Local machines are left alone.
set -euo pipefail

if [ "${CLAUDE_CODE_REMOTE:-}" != "true" ]; then
  exit 0
fi

PKGS="build-essential cmake qt6-base-dev qt6-base-dev-tools libqt6sql6-sqlite
      qt6-serialport-dev zlib1g-dev libgl1-mesa-dev libboost-dev"

missing=""
for p in $PKGS; do
  dpkg -s "$p" >/dev/null 2>&1 || missing="$missing $p"
done
if [ -n "$missing" ]; then
  SUDO=""
  [ "$(id -u)" -ne 0 ] && SUDO="sudo"
  $SUDO apt-get update -qq
  $SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq $missing
fi

# No display in the container; the GUI tests and --shot render offscreen.
if [ -n "${CLAUDE_ENV_FILE:-}" ]; then
  echo 'export QT_QPA_PLATFORM=offscreen' >> "$CLAUDE_ENV_FILE"
fi

cd "$CLAUDE_PROJECT_DIR"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j"$(nproc)" >/dev/null
