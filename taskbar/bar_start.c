// minimal layer-shell bar with clickable "Start" + PNG icon + right-aligned clock
#define _GNU_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <poll.h>
#include <time.h>
#include <limits.h>
#include <libgen.h>

#include <wayland-client.h>
#include <cairo/cairo.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

// ---- Globals
static struct wl_display *dpy;
static struct wl_registry *reg;
static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct wl_pointer *pointer;
static struct zwlr_layer_shell_v1 *layer;

static struct wl_surface *surf;
static struct zwlr_layer_surface_v1 *lsurf;

static int screen_w = 640;
static const int bar_h = 36;

static double ptr_x = -1, ptr_y = -1;
static struct { int x,y,w,h; } start_btn = {0,0,0,0};

static cairo_surface_t *logo = NULL;
static int tfd = -1;                // timerfd for clock ticks
static char clock_buf[32] = "";     // last formatted time

// ---- helpers
static int mkshm(size_t size){
  int fd = -1;
#ifdef MFD_CLOEXEC
  fd = memfd_create("bar", MFD_CLOEXEC);
#endif
  if (fd < 0) {
    char tmpl[] = "/dev/shm/bar-XXXXXX";
    fd = mkstemp(tmpl);
    if (fd < 0) { perror("mkstemp"); exit(1); }
    unlink(tmpl);
  }
  if (ftruncate(fd, (off_t)size) < 0) { perror("ftruncate"); exit(1); }
  return fd;
}

static void format_clock(char out[32]) {
  time_t now = time(NULL);
  struct tm lt;
  localtime_r(&now, &lt);
  // Change format if you prefer 24h: "%H:%M"
  strftime(out, 32, "%I:%M %p", &lt);
  // trim leading 0 in hour
  if (out[0] == '0') memmove(out, out+1, strlen(out));
}

static cairo_surface_t *load_icon_flexible(int argc, char **argv) {
  char path[PATH_MAX];

  if (argc > 1) {
    // use provided arg verbatim
    strncpy(path, argv[1], sizeof(path)-1); path[sizeof(path)-1] = 0;
  } else {
    // try <exe_dir>/../assets/icons/logo.png, then <exe_dir>/assets/icons/logo.png
    char exe[PATH_MAX] = {0};
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe)-1);
    if (n > 0) exe[n] = 0; else strcpy(exe, ".");
    char exe_copy[PATH_MAX]; strncpy(exe_copy, exe, sizeof(exe_copy)-1);
    char *dir = dirname(exe_copy);

    // attempt ../assets/icons/logo.png
    snprintf(path, sizeof(path), "%s/../assets/icons/logo.png", dir);
    struct stat st;
    if (stat(path, &st) != 0) {
      // fallback: ./assets/icons/logo.png next to exe dir
      snprintf(path, sizeof(path), "%s/assets/icons/logo.png", dir);
    }
  }

  // Preflight
  struct stat st;
  if (stat(path, &st) != 0) {
    fprintf(stderr, "Icon preflight: stat('%s') failed: %s\n", path, strerror(errno));
  } else {
    fprintf(stderr, "Icon preflight: '%s' exists (%lld bytes)\n", path, (long long)st.st_size);
  }

  cairo_surface_t *s = cairo_image_surface_create_from_png(path);
  cairo_status_t cs = cairo_surface_status(s);
  if (cs != CAIRO_STATUS_SUCCESS) {
    fprintf(stderr, "Cairo PNG load failed (%s): %s\n", path, cairo_status_to_string(cs));
    cairo_surface_destroy(s);
    return NULL;
  }
  fprintf(stderr, "Loaded icon from: %s\n", path);
  return s;
}

static void paint(void){
  // allocate buffer
  int stride = screen_w * 4;
  int size = stride * bar_h;
  int fd = mkshm(size);
  struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
  void *data = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
  if (data == MAP_FAILED) { perror("mmap"); exit(1); }
  struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, screen_w, bar_h, stride, WL_SHM_FORMAT_XRGB8888);
  wl_shm_pool_destroy(pool);
  close(fd);

  cairo_surface_t *cs = cairo_image_surface_create_for_data((unsigned char*)data,
                            CAIRO_FORMAT_RGB24, screen_w, bar_h, stride);
  cairo_t *cr = cairo_create(cs);

  // background (XP-ish blue)
  cairo_set_source_rgb(cr, 0.192, 0.416, 0.773);
  cairo_paint(cr);

  // --- LEFT: Start button
  int pad = 6;
  int icon_box = bar_h - pad*2;  // square icon
  int icon_draw = icon_box;
  int x = 6, y = (bar_h - icon_draw)/2;

  const char *label = "Start";
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 14.0);
  cairo_text_extents_t ext;
  cairo_text_extents(cr, label, &ext);

  int text_x = x + icon_draw + 8;
  int text_y = (bar_h + (int)(ext.height)) / 2 - 2;
  int btn_w = (text_x - x) + (int)ext.width + pad;
  int btn_h = bar_h - pad;
  int btn_y = (bar_h - btn_h) / 2;

  start_btn.x = x - 2; start_btn.y = 0; start_btn.w = btn_w + 4; start_btn.h = bar_h;

  int hover = (ptr_x >= start_btn.x && ptr_x < start_btn.x + start_btn.w &&
               ptr_y >= start_btn.y && ptr_y < start_btn.y + start_btn.h);

  if (hover) cairo_set_source_rgb(cr, 0.27, 0.52, 0.88);
  else       cairo_set_source_rgb(cr, 0.16, 0.36, 0.68);
  cairo_rectangle(cr, start_btn.x, btn_y, btn_w, btn_h);
  cairo_fill(cr);

  if (logo) {
    double w = cairo_image_surface_get_width(logo);
    double h = cairo_image_surface_get_height(logo);
    double scale = (double)icon_draw / (w > h ? w : h);
    double dw = w * scale, dh = h * scale;
    cairo_save(cr);
    cairo_translate(cr, x + (icon_draw - dw)/2.0, y + (icon_draw - dh)/2.0);
    cairo_scale(cr, scale, scale);
    cairo_set_source_surface(cr, logo, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
  } else {
    cairo_set_source_rgb(cr, 0.95, 0.95, 0.95);
    cairo_rectangle(cr, x, y, icon_draw, icon_draw);
    cairo_fill(cr);
  }

  cairo_set_source_rgb(cr, 1, 1, 1);
  cairo_move_to(cr, text_x, text_y);
  cairo_show_text(cr, label);

  // --- RIGHT: Clock
  char now_buf[32];
  format_clock(now_buf);

  cairo_text_extents_t te;
  cairo_text_extents(cr, now_buf, &te);
  int right_pad = 10;
  int cx = screen_w - right_pad - (int)te.width;
  int cy = (bar_h + (int)te.height)/2 - 2;

  // optional: subtle darker bg behind clock
  // cairo_set_source_rgba(cr, 0, 0, 0, 0.15);
  // cairo_rectangle(cr, cx - 6, 4, (int)te.width + 12, bar_h - 8);
  // cairo_fill(cr);

  cairo_set_source_rgb(cr, 1, 1, 1);
  cairo_move_to(cr, cx, cy);
  cairo_show_text(cr, now_buf);

  // finish
  cairo_destroy(cr);
  cairo_surface_destroy(cs);

  wl_surface_attach(surf, buf, 0, 0);
  wl_surface_damage_buffer(surf, 0, 0, screen_w, bar_h);
  wl_surface_commit(surf);

  // store last drawn time to avoid redundant full repaints later (optional)
  strncpy(clock_buf, now_buf, sizeof(clock_buf)-1);
  clock_buf[sizeof(clock_buf)-1] = 0;
}

// ---- listeners
static void lsurf_config(void *data, struct zwlr_layer_surface_v1 *ls,
                         uint32_t serial, uint32_t w, uint32_t h){
  if (w > 0) screen_w = (int)w;
  zwlr_layer_surface_v1_ack_configure(ls, serial);
  paint();
}
static void lsurf_closed(void *data, struct zwlr_layer_surface_v1 *ls){ exit(0); }
static const struct zwlr_layer_surface_v1_listener lsurf_listener = {
  .configure = lsurf_config, .closed = lsurf_closed
};

static void reg_global(void *data, struct wl_registry *r, uint32_t name,
                       const char *iface, uint32_t ver){
  if (!strcmp(iface, "wl_compositor")) comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
  else if (!strcmp(iface, "wl_shm")) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
  else if (!strcmp(iface, "zwlr_layer_shell_v1")) layer = wl_registry_bind(r, name, &zwlr_layer_shell_v1_interface, 4);
  else if (!strcmp(iface, "wl_seat")) seat = wl_registry_bind(r, name, &wl_seat_interface, 7);
}
static void reg_global_remove(void *d, struct wl_registry *r, uint32_t n){}
static const struct wl_registry_listener reg_listener = { reg_global, reg_global_remove };

// pointer events (click detection)
static void ptr_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t sx, wl_fixed_t sy){
  (void)d; (void)p; (void)serial; (void)s;
  ptr_x = wl_fixed_to_double(sx);
  ptr_y = wl_fixed_to_double(sy);
  paint();
}
static void ptr_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s){
  (void)d; (void)p; (void)serial; (void)s;
  ptr_x = ptr_y = -1;
  paint();
}
static void ptr_motion(void *d, struct wl_pointer *p, uint32_t time, wl_fixed_t sx, wl_fixed_t sy){
  (void)d; (void)p; (void)time;
  ptr_x = wl_fixed_to_double(sx);
  ptr_y = wl_fixed_to_double(sy);
  paint();
}
static void ptr_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t time, uint32_t button, uint32_t state){
  (void)d; (void)p; (void)serial; (void)time;
  const uint32_t BTN_LEFT = 0x110; // linux/input-event-codes.h
  if (button == BTN_LEFT && state == 1) { // press
    if (ptr_x >= start_btn.x && ptr_x < start_btn.x + start_btn.w &&
        ptr_y >= start_btn.y && ptr_y < start_btn.y + start_btn.h) {
      fprintf(stderr, "Start clicked!\n");
      // TODO: launch your menu
    }
  }
}
static void ptr_axis(void *d, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t value){ (void)d; (void)p; (void)time; (void)axis; (void)value; }
static void ptr_frame(void *d, struct wl_pointer *p){ (void)d; (void)p; }
static void ptr_axis_src(void *d, struct wl_pointer *p, uint32_t src){ (void)d; (void)p; (void)src; }

static const struct wl_pointer_listener pointer_listener = {
  .enter = ptr_enter,
  .leave = ptr_leave,
  .motion = ptr_motion,
  .button = ptr_button,
  .axis = ptr_axis,
  .frame = ptr_frame,
  .axis_source = ptr_axis_src,
};

static void seat_cap(void *d, struct wl_seat *s, uint32_t caps){
  if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
    pointer = wl_seat_get_pointer(s);
    wl_pointer_add_listener(pointer, &pointer_listener, NULL);
  } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && pointer) {
    wl_pointer_release(pointer);
    pointer = NULL;
  }
}
static void seat_name(void *d, struct wl_seat *s, const char *name){ (void)d; (void)s; (void)name; }
static const struct wl_seat_listener seat_listener = { seat_cap, seat_name };

// ---- main
int main(int argc, char **argv){
  // load icon
  logo = load_icon_flexible(argc, argv);

  // connect to Wayland
  dpy = wl_display_connect(NULL);
  if (!dpy) { fprintf(stderr, "no wayland display\n"); return 1; }
  reg = wl_display_get_registry(dpy);
  wl_registry_add_listener(reg, &reg_listener, NULL);
  wl_display_roundtrip(dpy);
  if (!comp || !shm || !layer) {
    fprintf(stderr, "missing globals comp=%d shm=%d layer=%d\n", !!comp, !!shm, !!layer);
    return 1;
  }
  if (seat) wl_seat_add_listener(seat, &seat_listener, NULL);

  // create surface
  surf = wl_compositor_create_surface(comp);
  lsurf = zwlr_layer_shell_v1_get_layer_surface(layer, surf, NULL,
            ZWLR_LAYER_SHELL_V1_LAYER_TOP, "bar-start");
  zwlr_layer_surface_v1_set_anchor(lsurf,
      ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
      ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
      ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
  zwlr_layer_surface_v1_set_size(lsurf, 0, bar_h);
  zwlr_layer_surface_v1_set_exclusive_zone(lsurf, bar_h);
  zwlr_layer_surface_v1_add_listener(lsurf, &lsurf_listener, NULL);
  wl_surface_commit(surf);

  // initial configure + first paint
  wl_display_roundtrip(dpy);

  // timer for clock (1s periodic)
  tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (tfd < 0) { perror("timerfd_create"); return 1; }
  struct itimerspec its = {0};
  its.it_interval.tv_sec = 1; its.it_value.tv_sec = 1;
  if (timerfd_settime(tfd, 0, &its, NULL) < 0) { perror("timerfd_settime"); return 1; }
  format_clock(clock_buf); // initialize

  // event loop: poll wayland fd + timerfd
  struct pollfd fds[2];
  fds[0].fd = wl_display_get_fd(dpy); fds[0].events = POLLIN;
  fds[1].fd = tfd;                    fds[1].events = POLLIN;

  for (;;) {
    // flush outgoing requests
    if (wl_display_flush(dpy) < 0 && errno != EAGAIN) break;

    int ret = poll(fds, 2, -1);
    if (ret < 0) { if (errno == EINTR) continue; perror("poll"); break; }

    if (fds[0].revents & POLLIN) {
      if (wl_display_dispatch(dpy) < 0) break;
    }

    if (fds[1].revents & POLLIN) {
      uint64_t exp; (void)read(tfd, &exp, sizeof(exp)); // clear timer
      char now[32]; format_clock(now);
      if (strcmp(now, clock_buf) != 0) {
        // time changed; repaint
        paint();
      }
    }
  }

  if (logo) cairo_surface_destroy(logo);
  if (tfd >= 0) close(tfd);
  return 0;
}