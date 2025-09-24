#!/bin/bash

# Start sway with your backend/renderer options
WLR_BACKENDS=x11 WLR_RENDERER=pixman sway -d -D noscanout &

# Give sway a moment to start and set up the display socket
sleep 1

# Detect and log which Wayland session is active
if [ -n "$WAYLAND_DISPLAY" ]; then
    echo "WAYLAND_DISPLAY is $WAYLAND_DISPLAY"
    echo "WAYLAND_DISPLAY=$WAYLAND_DISPLAY" >> sway-session.log
else
    # fallback: list sockets under runtime dir
    echo "Detected sockets:"
    ls "$XDG_RUNTIME_DIR"/wayland-* 2>/dev/null
    ls "$XDG_RUNTIME_DIR"/wayland-* 2>/dev/null >> sway-session.log
fi

# Launch your app on the active session
WAYLAND_DISPLAY=$WAYLAND_DISPLAY ./taskbar