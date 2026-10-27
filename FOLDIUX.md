# Foldiux fork of TAWC

This branch (`foldiux`) lets [Foldiux](https://github.com/Ezmanw/Foldiux) embed TAWC as its
Wayland backend. Changes from upstream:

- `app` builds as an Android **library** (AAR) instead of an application.
- `TawcRuntime.init(app)` holds what `TawcApplication.onCreate` did, so a host Application can call it.
- `CompositorActivity` is `open`, exposes `activityId` and `rootView`, and
  `CompositorActivity.hostClass` lets the host substitute a subclass for window hosts.
- `me.phie.tawc.embed.TawcEmbed`: a small public API (launch a GUI program, exit everything).

All TAWC credit goes to its authors; see README.md and LICENSE.
