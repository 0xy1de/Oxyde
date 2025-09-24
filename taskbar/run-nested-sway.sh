#!/usr/bin/env bash
set -euo pipefail
# Start nested sway on X11 or Wayland
if [ "${XDG_SESSION_TYPE:-}" = "wayland" ]; then
  WLR_BACKENDS=wayland WLR_RENDERER=pixman sway -d -D noscanout >/tmp/sway-nested.log 2>&1 &
else
  WLR_BACKENDS=x11     WLR_RENDERER=pixman sway -d -D noscanout >/tmp/sway-nested.log 2>&1 &
fi
sleep 1
sock_before=$(ls "$XDG_RUNTIME_DIR"/wayland-* 2>/dev/null || true)
# Wait for a new socket
for i in {1..30}; do
  sock_after=$(ls "$XDG_RUNTIME_DIR"/wayland-* 2>/dev/null || true)
  new=$(comm -13 <(echo "$sock_before"|sort) <(echo "$sock_after"|sort) || true)
  [ -n "$new" ] && break
  sleep 0.2
done
sock=${new##*/}
[ -z "$sock" ] && { echo "Failed to find nested Wayland socket"; exit 1; }
echo "Using socket: $sock"
WAYLAND_DISPLAY="$sock" WAYLAND_DEBUG=1 ./taskbar 2>&1 | tee /tmp/taskbar.log
