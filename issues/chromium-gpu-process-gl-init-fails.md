# Chromium's GPU process fails GL init; falls back to software

Chromium and Electron start under tawcroot (OnePlus 9, Arch Linux ARM,
libhybris, 2026-09-28) but the GPU process exits during init and they
composite in software (SHM buffers).

The guest env sets `XDG_SESSION_TYPE=wayland`, so Chromium/Electron
pick the Wayland ozone platform. There (Electron 44.4.5):

    ANGLE Display::initialize error 12289: Failed to get system egl display
    Initialization of all (2) EGL display types failed.
    Exiting GPU process due to errors during initialization

Also logs `drmGetDevices2() has not found any devices: Permission
denied`. Suspected cause, unverified: ANGLE asks for a
surfaceless/device EGL platform, while libhybris only advertises
`EGL_KHR_platform_wayland` (`deps/libhybris/hybris/egl/egl.c`).

Under X11 (Xwayland, the old default without `XDG_SESSION_TYPE`) it
fails earlier with `Could not load GLX entry point glXCreateContext`
(`GLX is not present` with the cpu backend).

Sandbox: plain Chromium still needs `--no-sandbox`; Electron gets it
from `ELECTRON_DISABLE_SANDBOX=1` in the guest env.
