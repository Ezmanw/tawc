/*
 * Regression test for tawc issue #29: an EGL client with a second
 * wl_display in the same process (Qt plus GDK from the gtk3 platform
 * theme). libhybris maps every eglGetDisplay() to one Android display,
 * so the wayland EGL platform used to build every window on the first
 * connection's queue and abort in wl_proxy_set_queue ("Proxy and queue
 * point to different wl_displays").
 *
 * Flow: eglGetDisplay() on connection A, then render frames into one
 * window on A and one on a secondary connection, tearing the secondary
 * down and reconnecting a few times.
 *
 * Exit codes:
 *   0 all frames rendered and read back green
 *   1 wayland setup failed
 *   2 EGL setup failed
 *   3 wrong pixel read back
 *   (SIGABRT is the original bug)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "xdg-shell-client-protocol.h"

#define WIN_W 64
#define WIN_H 64
#define FRAMES 30
#define RECONNECTS 3

struct conn {
    struct wl_display *dpy;
    struct wl_compositor *compositor;
    struct xdg_wm_base *wm_base;
    struct wl_surface *surface;
    struct xdg_surface *xsurf;
    struct xdg_toplevel *toplevel;
    struct wl_egl_window *ewin;
    EGLSurface esurf;
    int configured;
};

static void wm_base_ping(void *d, struct xdg_wm_base *b, uint32_t s)
{ (void)d; xdg_wm_base_pong(b, s); }
static const struct xdg_wm_base_listener wm_base_listener = { .ping = wm_base_ping };

static void registry_global(void *data, struct wl_registry *r, uint32_t id,
                            const char *iface, uint32_t v)
{
    struct conn *c = data;
    if (strcmp(iface, "wl_compositor") == 0)
        c->compositor = wl_registry_bind(r, id, &wl_compositor_interface, v >= 4 ? 4 : v);
    else if (strcmp(iface, "xdg_wm_base") == 0) {
        c->wm_base = wl_registry_bind(r, id, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(c->wm_base, &wm_base_listener, NULL);
    }
}
static void registry_global_remove(void *d, struct wl_registry *r, uint32_t id)
{ (void)d; (void)r; (void)id; }
static const struct wl_registry_listener registry_listener = {
    .global = registry_global, .global_remove = registry_global_remove,
};

static void xsurf_configure(void *d, struct xdg_surface *s, uint32_t serial)
{ struct conn *c = d; xdg_surface_ack_configure(s, serial); c->configured = 1; }
static const struct xdg_surface_listener xsurf_listener = { .configure = xsurf_configure };

static void tl_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *s)
{ (void)d; (void)t; (void)w; (void)h; (void)s; }
static void tl_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; }
static const struct xdg_toplevel_listener tl_listener = {
    .configure = tl_configure, .close = tl_close,
};

static int conn_open(struct conn *c, const char *title)
{
    memset(c, 0, sizeof(*c));
    c->esurf = EGL_NO_SURFACE;
    c->dpy = wl_display_connect(NULL);
    if (!c->dpy) { fprintf(stderr, "wl_display_connect failed\n"); return 0; }
    struct wl_registry *reg = wl_display_get_registry(c->dpy);
    wl_registry_add_listener(reg, &registry_listener, c);
    wl_display_roundtrip(c->dpy);
    wl_registry_destroy(reg);
    if (!c->compositor || !c->wm_base) { fprintf(stderr, "no compositor/wm_base\n"); return 0; }
    c->surface = wl_compositor_create_surface(c->compositor);
    c->xsurf = xdg_wm_base_get_xdg_surface(c->wm_base, c->surface);
    xdg_surface_add_listener(c->xsurf, &xsurf_listener, c);
    c->toplevel = xdg_surface_get_toplevel(c->xsurf);
    xdg_toplevel_add_listener(c->toplevel, &tl_listener, c);
    xdg_toplevel_set_title(c->toplevel, title);
    xdg_toplevel_set_app_id(c->toplevel, "egl-second-connection");
    wl_surface_commit(c->surface);
    /* The first configure can arrive after a roundtrip (window host setup). */
    while (!c->configured)
        if (wl_display_dispatch(c->dpy) < 0) { fprintf(stderr, "no configure\n"); return 0; }
    c->ewin = wl_egl_window_create(c->surface, WIN_W, WIN_H);
    if (!c->ewin) { fprintf(stderr, "wl_egl_window_create failed\n"); return 0; }
    return 1;
}

static void conn_close(struct conn *c, EGLDisplay edpy)
{
    if (c->esurf != EGL_NO_SURFACE)
        eglDestroySurface(edpy, c->esurf);
    if (c->ewin) wl_egl_window_destroy(c->ewin);
    if (c->toplevel) xdg_toplevel_destroy(c->toplevel);
    if (c->xsurf) xdg_surface_destroy(c->xsurf);
    if (c->surface) wl_surface_destroy(c->surface);
    if (c->wm_base) xdg_wm_base_destroy(c->wm_base);
    if (c->compositor) wl_compositor_destroy(c->compositor);
    if (c->dpy) wl_display_disconnect(c->dpy);
    memset(c, 0, sizeof(*c));
}

/* Clear to green, read back the centre, present. */
static int draw(struct conn *c, EGLDisplay edpy, EGLContext ectx)
{
    if (!eglMakeCurrent(edpy, c->esurf, c->esurf, ectx)) {
        fprintf(stderr, "eglMakeCurrent failed: 0x%x\n", eglGetError());
        return 2;
    }
    /* Only the visible toplevel gets frame callbacks; don't wait on them. */
    eglSwapInterval(edpy, 0);
    glViewport(0, 0, WIN_W, WIN_H);
    glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    unsigned char px[4] = {0};
    glReadPixels(WIN_W / 2, WIN_H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    if (!(px[1] >= 200 && px[0] < 50 && px[2] < 50)) {
        fprintf(stderr, "bad pixel R%u G%u B%u A%u\n", px[0], px[1], px[2], px[3]);
        return 3;
    }
    if (!eglSwapBuffers(edpy, c->esurf)) {
        fprintf(stderr, "eglSwapBuffers failed: 0x%x\n", eglGetError());
        return 2;
    }
    /* Keep each connection's default queue serviced (xdg pings). */
    wl_display_dispatch_pending(c->dpy);
    wl_display_flush(c->dpy);
    return 0;
}

int main(void)
{
    struct conn primary, secondary;
    if (!conn_open(&primary, "egl-second-connection primary")) return 1;

    EGLDisplay edpy = eglGetDisplay((EGLNativeDisplayType)primary.dpy);
    if (!eglInitialize(edpy, NULL, NULL)) { fprintf(stderr, "eglInitialize failed\n"); return 2; }
    EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg; EGLint n_cfg = 0;
    if (!eglChooseConfig(edpy, cfg_attribs, &cfg, 1, &n_cfg) || n_cfg < 1) {
        fprintf(stderr, "eglChooseConfig failed\n"); return 2;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ectx = eglCreateContext(edpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
    if (ectx == EGL_NO_CONTEXT) { fprintf(stderr, "eglCreateContext failed\n"); return 2; }

    primary.esurf = eglCreateWindowSurface(edpy, cfg, (EGLNativeWindowType)primary.ewin, NULL);
    if (primary.esurf == EGL_NO_SURFACE) { fprintf(stderr, "primary eglCreateWindowSurface failed\n"); return 2; }

    for (int round = 0; round < RECONNECTS; round++) {
        if (!conn_open(&secondary, "egl-second-connection secondary")) return 1;
        /* Like GDK beside Qt: same process, different wl_display. */
        EGLDisplay edpy2 = eglGetDisplay((EGLNativeDisplayType)secondary.dpy);
        if (edpy2 != edpy || !eglInitialize(edpy2, NULL, NULL)) {
            fprintf(stderr, "secondary eglGetDisplay/eglInitialize failed\n");
            return 2;
        }
        secondary.esurf = eglCreateWindowSurface(edpy, cfg, (EGLNativeWindowType)secondary.ewin, NULL);
        if (secondary.esurf == EGL_NO_SURFACE) {
            fprintf(stderr, "secondary eglCreateWindowSurface failed\n");
            return 2;
        }
        for (int i = 0; i < FRAMES; i++) {
            int r = draw(&primary, edpy, ectx);
            if (!r) r = draw(&secondary, edpy, ectx);
            if (r) return r;
        }
        /* No eglTerminate: it would tear down the shared Android display. */
        eglMakeCurrent(edpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        conn_close(&secondary, edpy);
        fprintf(stderr, "round %d OK\n", round);
    }

    eglMakeCurrent(edpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(edpy, ectx);
    eglDestroySurface(edpy, primary.esurf);
    primary.esurf = EGL_NO_SURFACE;
    /* Terminate before disconnecting: the platform still uses this connection. */
    eglTerminate(edpy);
    conn_close(&primary, edpy);
    fprintf(stderr, "egl-second-connection: OK\n");
    return 0;
}
