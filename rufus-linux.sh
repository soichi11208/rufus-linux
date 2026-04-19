#!/bin/sh
# Wrapper: preserve Wayland session variables across sudo/pkexec.
exec sudo \
  --preserve-env=WAYLAND_DISPLAY,XDG_RUNTIME_DIR,GDK_BACKEND,DBUS_SESSION_BUS_ADDRESS,HOME \
  "$(dirname "$0")/build/rufus-linux" "$@"
