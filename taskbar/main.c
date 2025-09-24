// SPDX-FileCopyrightText: 2025 Oxyde Contributors
// SPDX-License-Identifier: MPL-2.0

#define _GNU_SOURCE 1
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/mman.h>      // mmap, PROT_*, MAP_*, memfd_create (glibc)
#ifdef __has_include
#  if __has_include(<linux/memfd.h>)
#    include <linux/memfd.h>   // some libcs put memfd_create here
#  endif
#endif

#include <wayland-client.h>
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

static struct wl_display *display;
static struct wl_registry *registry;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct zwlr_foreign_toplevel_manager_v1 *toplevel_mgr;

static struct wl_surface *surface;
static struct zwlr_layer_surface_v1 *layer_surface;

static int bar_height = 32;
static int width = 1280; // Will update on configure
static int height = 32;

static struct wl_callback *frame_cb = NULL;

static int shm_fd_create(size_t size) {
    if (size == 0) {
        fprintf(stderr, "shm_fd_create: invalid size 0\n");
        return -1;
    }

#ifdef __linux__
    // Try memfd_create first if available
#  if defined(MFD_CLOEXEC)
    int fd = memfd_create("taskbar-shm", MFD_CLOEXEC);
#  else
    int fd = memfd_create("taskbar-shm", 0);
#  endif
    if (fd >= 0) {
        if (ftruncate(fd, (off_t)size) < 0) {
            perror("ftruncate");
            close(fd);
            return -1;
        }
        return fd;
    }
    // fallthrough to mkstemp if memfd_create not available/failed
#endif

    char name[] = "/tmp/taskbar-shm-XXXXXX";
    int tmpfd = mkstemp(name);
    if (tmpfd < 0) { perror("mkstemp"); return -1; }
    unlink(name);
    if (ftruncate(tmpfd, (off_t)size) < 0) { perror("ftruncate"); close(tmpfd); return -1; }
    return tmpfd;
}

static const struct wl_callback_listener frame_listener;

static void paint(void) {
    // Validate sizes
    if (width <= 0 || height <= 0) {
        fprintf(stderr, "paint: invalid width/height %d x %d\n", width, height);
        return;
    }

    // Compute stride and size with overflow checks
    if ((size_t)width > SIZE_MAX / 4) {
        fprintf(stderr, "paint: width too large\n");
        return;
    }
    int stride = width * 4;
    size_t size = (size_t)stride * (size_t)height;
    if (size == 0) return;

    int fd = shm_fd_create(size);
    if (fd < 0) { fprintf(stderr, "paint: failed to create shm fd\n"); return; }

    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int)size);
    if (!pool) { close(fd); fprintf(stderr, "paint: wl_shm_create_pool failed\n"); return; }

    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) { perror("mmap"); wl_shm_pool_destroy(pool); close(fd); return; }

    // Fill with a simple blue color (XRGB8888)
    uint32_t *px = (uint32_t *)data;
    uint8_t r = 49, g = 106, b = 197;
    uint32_t color = (uint32_t)0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    for (int i = 0; i < width * height; ++i) px[i] = color;

    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_XRGB8888);
    // We can destroy the pool and close fd after creating the buffer
    wl_shm_pool_destroy(pool);
    close(fd);

    if (!buffer) { munmap(data, size); fprintf(stderr, "paint: wl_shm_pool_create_buffer failed\n"); return; }

    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, width, height);
    wl_surface_commit(surface);

    // We can destroy the client-side buffer proxy now; compositor keeps its own reference.
    wl_buffer_destroy(buffer);

    // Unmap our mapping to avoid leaking client memory. The compositor uses the fd/reference internally.
    munmap(data, size);

    // Ensure bytes are sent out
    wl_display_flush(display);
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time) {
    (void)data; (void)time;
    if (cb) {
        wl_callback_destroy(cb);
        // clear only if cb equals stored frame_cb (defensive)
        if (cb == frame_cb) frame_cb = NULL;
    }

    // Request next frame callback
    frame_cb = wl_surface_frame(surface);
    wl_callback_add_listener(frame_cb, &frame_listener, NULL);

    // Paint update
    paint();
}
static const struct wl_callback_listener frame_listener = { .done = frame_done };

static void layer_surface_configure(void *data,
    struct zwlr_layer_surface_v1 *ls, uint32_t serial, uint32_t w, uint32_t h)
{
    (void)data;
    if (w > 0) {
        if (w > (uint32_t)INT_MAX) width = INT_MAX;
        else width = (int)w;
    }
    height = bar_height;

    zwlr_layer_surface_v1_set_size(ls, (uint32_t)width, (uint32_t)height);
    zwlr_layer_surface_v1_ack_configure(ls, serial);

    // Start the frame loop if not already started
    if (!frame_cb) {
        frame_cb = wl_surface_frame(surface);
        wl_callback_add_listener(frame_cb, &frame_listener, NULL);
    }

    // Draw initial content
    paint();
}
static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *ls)
{
    (void)data; (void)ls;
    fprintf(stderr, "Layer surface closed by compositor\n");
    wl_display_disconnect(display);
    exit(0);
}
static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

static void registry_global(void *data, struct wl_registry *reg,
                            uint32_t name, const char *interface, uint32_t version)
{
    (void)data;
    if (strcmp(interface, "wl_compositor") == 0) {
        compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
        if (!compositor) fprintf(stderr, "Failed to bind wl_compositor\n");
    } else if (strcmp(interface, "wl_shm") == 0) {
        shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
        if (!shm) fprintf(stderr, "Failed to bind wl_shm\n");
    } else if (strcmp(interface, "zwlr_layer_shell_v1") == 0) {
        layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 4);
        if (!layer_shell) fprintf(stderr, "Failed to bind zwlr_layer_shell_v1\n");
    } else if (strcmp(interface, "zwlr_foreign_toplevel_manager_v1") == 0) {
        toplevel_mgr = wl_registry_bind(reg, name, &zwlr_foreign_toplevel_manager_v1_interface, 3);
        if (!toplevel_mgr) fprintf(stderr, "Failed to bind zwlr_foreign_toplevel_manager_v1\n");
    }
}
static void registry_global_remove(void *data, struct wl_registry *reg, uint32_t name) {
    (void)data; (void)reg; (void)name;
}
static const struct wl_registry_listener registry_listener = {
    .global = registry_global, .global_remove = registry_global_remove
};

/* Foreign toplevel listeners (just log for now) */
static void toplevel_title(void *data, struct zwlr_foreign_toplevel_handle_v1 *hdl, const char *title){
    (void)data; (void)hdl;
    fprintf(stderr, "[WIN] %s\n", title ? title : "(no title)");
}
static void toplevel_done(void *d, struct zwlr_foreign_toplevel_handle_v1 *h){ (void)d; (void)h; }
static void toplevel_state(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_array *state){ (void)d; (void)h; (void)state; }
static void toplevel_app_id(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *app_id){ (void)d; (void)h; fprintf(stderr, "  app-id=%s\n", app_id?app_id:""); }
static void toplevel_output_enter(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o){ (void)d; (void)h; (void)o; }
static void toplevel_output_leave(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o){ (void)d; (void)h; (void)o; }
static void toplevel_closed(void *d, struct zwlr_foreign_toplevel_handle_v1 *h){ (void)d; (void)h; }
static void toplevel_parent(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct zwlr_foreign_toplevel_handle_v1 *p){ (void)d; (void)h; (void)p; }

static const struct zwlr_foreign_toplevel_handle_v1_listener toplevel_listener = {
    .title = toplevel_title,
    .app_id = toplevel_app_id,
    .output_enter = toplevel_output_enter,
    .output_leave = toplevel_output_leave,
    .state = toplevel_state,
    .parent = toplevel_parent,
    .done = toplevel_done,
    .closed = toplevel_closed,
};

static void toplevel_manager_new(void *data,
    struct zwlr_foreign_toplevel_manager_v1 *mgr,
    struct zwlr_foreign_toplevel_handle_v1 *hdl)
{
    (void)data; (void)mgr;
    zwlr_foreign_toplevel_handle_v1_add_listener(hdl, &toplevel_listener, NULL);
}
static void toplevel_manager_finished(void *data, struct zwlr_foreign_toplevel_manager_v1 *mgr){
    (void)data; (void)mgr;
}
static const struct zwlr_foreign_toplevel_manager_v1_listener mgr_listener = {
    .toplevel = toplevel_manager_new,
    .finished = toplevel_manager_finished,
};

int main(void) {
    display = wl_display_connect(NULL);
    if (!display) { fprintf(stderr, "Failed to connect to Wayland display\n"); return 1; }

    registry = wl_display_get_registry(display);
    if (!registry) { fprintf(stderr, "Failed to get registry\n"); wl_display_disconnect(display); return 1; }
    wl_registry_add_listener(registry, &registry_listener, NULL);

    // Roundtrip so we get globals
    wl_display_roundtrip(display);

    if (!compositor || !shm || !layer_shell) {
        fprintf(stderr, "Missing required globals (compositor/shm/layer_shell)\n");
        wl_display_disconnect(display);
        return 1;
    }

    if (toplevel_mgr) {
        zwlr_foreign_toplevel_manager_v1_add_listener(toplevel_mgr, &mgr_listener, NULL);
    } else {
        fprintf(stderr, "Warning: foreign_toplevel not available; window listing disabled.\n");
    }

    surface = wl_compositor_create_surface(compositor);
    if (!surface) { fprintf(stderr, "Failed to create surface\n"); wl_display_disconnect(display); return 1; }

    layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        layer_shell, surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, "taskbar");
    if (!layer_surface) { fprintf(stderr, "Failed to get layer surface\n"); wl_display_disconnect(display); return 1; }

    // Anchor bottom, left, right; request exclusive zone (height)
    zwlr_layer_surface_v1_set_anchor(layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT   |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_size(layer_surface, 0, (uint32_t)bar_height);
    zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, (int32_t)bar_height);
    zwlr_layer_surface_v1_add_listener(layer_surface, &layer_listener, NULL);

    wl_surface_commit(surface);

    // Enter the event loop. frame callbacks / painting will start after the compositor sends configure.
    while (wl_display_dispatch(display) != -1) { /* event loop */ }

    // Cleanup (we usually never get here)
    if (frame_cb) wl_callback_destroy(frame_cb);
    if (layer_surface) zwlr_layer_surface_v1_destroy(layer_surface);
    if (surface) wl_surface_destroy(surface);
    if (layer_shell) zwlr_layer_shell_v1_destroy(layer_shell);
    if (toplevel_mgr) zwlr_foreign_toplevel_manager_v1_destroy(toplevel_mgr);
    if (shm) wl_shm_destroy(shm);
    if (compositor) wl_compositor_destroy(compositor);
    if (registry) wl_registry_destroy(registry);
    if (display) wl_display_disconnect(display);

    return 0;
}
