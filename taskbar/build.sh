#!/usr/bin/env bash
set -euo pipefail

mkdir -p proto

# Copy (or just reference directly) the xdg-shell XML
cp /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml proto/xdg-shell.xml

# Generate protocol headers/code
wayland-scanner client-header proto/xdg-shell.xml \
  xdg-shell-client-protocol.h
wayland-scanner private-code proto/xdg-shell.xml \
  xdg-shell-protocol.c

wayland-scanner client-header proto/wlr-layer-shell-unstable-v1.xml \
  wlr-layer-shell-unstable-v1-client-protocol.h
wayland-scanner private-code proto/wlr-layer-shell-unstable-v1.xml \
  wlr-layer-shell-unstable-v1-protocol.c

wayland-scanner client-header proto/wlr-foreign-toplevel-management-unstable-v1.xml \
  wlr-foreign-toplevel-management-unstable-v1-client-protocol.h
wayland-scanner private-code proto/wlr-foreign-toplevel-management-unstable-v1.xml \
  wlr-foreign-toplevel-management-unstable-v1-protocol.c

# Compile/link
CFLAGS="$(pkg-config --cflags wayland-client)"
LIBS="$(pkg-config --libs wayland-client)"
cc -O2 -Wall -o taskbar main.c \
   xdg-shell-protocol.c \
   wlr-layer-shell-unstable-v1-protocol.c \
   wlr-foreign-toplevel-management-unstable-v1-protocol.c \
   $CFLAGS $LIBS

echo "Built ./taskbar"