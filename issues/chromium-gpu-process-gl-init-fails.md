# Chromium's GPU process fails GL init; falls back to software

`chromium --no-sandbox` starts under tawcroot (OnePlus 9, Arch Linux
ARM, 2026-09-28) since the `/dev/shm` read-only reopen fix, but its GPU
process exits during init:

    ANGLE Display::initialize error 12289: Could not load GLX entry point glXCreateContext
    Initialization of all (2) EGL display types failed.
    Exiting GPU process due to errors during initialization

(`GLX is not present` with the cpu backend.) Chromium picks the X11
ozone platform by default (`DISPLAY=:0` is set), runs through
Xwayland, and composites in software: the compositor sees one SHM X11
surface. `--ozone-platform=wayland` was not tried; an earlier run (plan
notes) reported `Failed to get system egl display` on libhybris.
