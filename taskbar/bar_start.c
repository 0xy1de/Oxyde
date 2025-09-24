// layer-shell taskbar: Start + clock + window buttons (icon + title) for wlroots compositors
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
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

// ---- Globals
static struct wl_display *dpy;
static struct wl_registry *reg;
static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct wl_pointer *pointer;
static struct zwlr_layer_shell_v1 *layer;
static struct zwlr_foreign_toplevel_manager_v1 *ftm;

static struct wl_surface *surf;
static struct zwlr_layer_surface_v1 *lsurf;

static int screen_w = 640;
static const int bar_h = 36;

static double ptr_x = -1, ptr_y = -1;
static struct { int x,y,w,h; } start_btn = {0,0,0,0};

static cairo_surface_t *logo = NULL;
static int tfd = -1;                // timerfd for clock ticks
static char clock_buf[32] = "";     // last formatted time

// ---- Task list data
struct win_btn {
  struct zwlr_foreign_toplevel_handle_v1 *hdl;
  char *title;
  char *appid;
  cairo_surface_t *icon;   // cached app icon (16..24 px)
  int x,y,w,h;             // draw rect
  int closed;
};
static struct win_btn **wins = NULL;
static size_t wins_len = 0, wins_cap = 0;

// ----------------- helpers -----------------
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
  strftime(out, 32, "%I:%M %p", &lt);  // 12h; change to "%H:%M" for 24h
  if (out[0] == '0') memmove(out, out+1, strlen(out));
}

static cairo_surface_t *try_png(const char *p){
  cairo_surface_t *s = cairo_image_surface_create_from_png(p);
  if (cairo_surface_status(s) == CAIRO_STATUS_SUCCESS) return s;
  cairo_surface_destroy(s); return NULL;
}

// quick icon theme lookup (hicolor + pixmaps)
static const char* sizes[] = {"16","22","24","32","48"};
static cairo_surface_t* load_icon_for_appid(const char *appid){
  if (!appid || !*appid) return NULL;
  char p[PATH_MAX];
  for (size_t i=0;i<sizeof(sizes)/sizeof(sizes[0]);i++){
    snprintf(p,sizeof(p),"/usr/share/icons/hicolor/%sx%s/apps/%s.png", sizes[i],sizes[i], appid);
    cairo_surface_t *s = try_png(p); if (s) return s;
    snprintf(p,sizeof(p),"/usr/share/icons/hicolor/%sx%s/apps/%s-symbolic.png", sizes[i],sizes[i], appid);
    s = try_png(p); if (s) return s;
  }
  snprintf(p,sizeof(p),"/usr/share/pixmaps/%s.png", appid);
  cairo_surface_t *s = try_png(p); if (s) return s;
  return NULL;
}

static cairo_surface_t *load_logo_flexible(int argc, char **argv) {
  char path[PATH_MAX];

  if (argc > 1) {
    strncpy(path, argv[1], sizeof(path)-1); path[sizeof(path)-1] = 0;
  } else {
    // try <exe_dir>/../assets/icons/logo.png, then <exe_dir>/assets/icons/logo.png
    char exe[PATH_MAX] = {0};
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe)-1);
    if (n > 0) exe[n] = 0; else strcpy(exe, ".");
    char exe_copy[PATH_MAX]; strncpy(exe_copy, exe, sizeof(exe_copy)-1); exe_copy[sizeof(exe_copy)-1]=0;
    char *dir = dirname(exe_copy);

    snprintf(path, sizeof(path), "%s/../assets/icons/logo.png", dir);
    struct stat st;
    if (stat(path, &st) != 0) {
      snprintf(path, sizeof(path), "%s/assets/icons/logo.png", dir);
    }
  }

  struct stat st;
  if (stat(path, &st) != 0) {
    fprintf(stderr, "Logo stat('%s') failed: %s (placeholder used)\n", path, strerror(errno));
    return NULL;
  }
  cairo_surface_t *s = cairo_image_surface_create_from_png(path);
  if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
    fprintf(stderr, "Logo PNG load failed (%s)\n", path);
    cairo_surface_destroy(s);
    return NULL;
  }
  fprintf(stderr, "Loaded logo: %s\n", path);
  return s;
}

static void draw_ellipsis_text(cairo_t *cr, const char *txt, int maxw){
  if (!txt) txt = "";
  cairo_text_extents_t te;
  cairo_text_extents(cr, txt, &te);
  if ((int)te.width <= maxw) { cairo_show_text(cr, txt); return; }
  char buf[256]; strncpy(buf, txt, sizeof(buf)-1); buf[sizeof(buf)-1]=0;
  int n = (int)strlen(buf);
  while (n>0) {
    buf[n--] = 0;
    char tmp[260]; snprintf(tmp, sizeof(tmp), "%s...", buf);
    cairo_text_extents(cr, tmp, &te);
    if ((int)te.width <= maxw) { cairo_show_text(cr, tmp); return; }
  }
  cairo_show_text(cr, "...");
}

// ----------------- layout + paint -----------------
static int left_pad = 6, right_pad = 10;
static int start_w_cached = 110; // measured on first paint
static int clock_w_cached  = 80;
static const int icon_px = 18;
static const int btn_h_pad = 6;

static void win_refresh_layout(void); // fwd

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

  // background (XP-ish blue -> Win2000-ish slightly darker)
  cairo_set_source_rgb(cr, 0.16, 0.36, 0.68);
  cairo_paint(cr);

  // --- LEFT: Start button
  int pad = 6;
  int icon_box = bar_h - pad*2;  // square icon
  int icon_draw = icon_box;
  int x = left_pad, y = (bar_h - icon_draw)/2;

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
  start_w_cached = start_btn.w;

  int hover = (ptr_x >= start_btn.x && ptr_x < start_btn.x + start_btn.w &&
               ptr_y >= start_btn.y && ptr_y < start_btn.y + start_btn.h);

  if (hover) cairo_set_source_rgb(cr, 0.27, 0.52, 0.88);
  else       cairo_set_source_rgb(cr, 0.20, 0.44, 0.80);
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
  int cx = screen_w - right_pad - (int)te.width;
  int cy = (bar_h + (int)te.height)/2 - 2;
  cairo_set_source_rgb(cr, 1, 1, 1);
  cairo_move_to(cr, cx, cy);
  cairo_show_text(cr, now_buf);
  clock_w_cached = (int)te.width + 12; // keep some padding

  // --- CENTER: Window buttons
int xbtn = left_pad + start_w_cached + 8;
int avail = screen_w - right_pad - clock_w_cached - 8 - xbtn;
int tb_h = bar_h - btn_h_pad;
int tb_y = (bar_h - tb_h)/2;

int visible = (int)wins_len;
int each = visible ? (avail / visible) : 0;
int minw = 120; if (visible && each < minw) each = avail / visible;

cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
cairo_set_font_size(cr, 12.0);

  for (size_t i=0;i<wins_len;i++){
    struct win_btn *w = wins[i];
    if (w->closed) continue;

    int w_x = xbtn;
    int w_w = each > 0 ? each : 0;
    xbtn += w_w + 4;

    w->x = w_x; w->y = tb_y; w->w = w_w; w->h = tb_h;

    // background + borders (Win2000 vibe)
    int hov = (ptr_x >= w->x && ptr_x < w->x + w->w && ptr_y >= w->y && ptr_y < w->y + w->h);
    cairo_set_source_rgb(cr, hov ? 0.27:0.22, hov ? 0.52:0.46, hov ? 0.88:0.76);
    cairo_rectangle(cr, w->x, w->y, w->w, w->h);
    cairo_fill(cr);

    cairo_set_source_rgb(cr, 0.08, 0.18, 0.36); // dark outer
    cairo_rectangle(cr, w->x+0.5, w->y+0.5, w->w-1, w->h-1);
    cairo_stroke(cr);
    cairo_set_source_rgba(cr, 1,1,1, 0.15);     // inner highlight
    cairo_rectangle(cr, w->x+1.5, w->y+1.5, w->w-3, w->h-3);
    cairo_stroke(cr);

    // icon
    int ix = w->x + 6, iy = w->y + (w->h - icon_px)/2;
    if (!w->icon && w->appid) w->icon = load_icon_for_appid(w->appid);
    if (w->icon) {
      double iw = cairo_image_surface_get_width(w->icon);
      double ih = cairo_image_surface_get_height(w->icon);
      double s = (double)icon_px / (iw > ih ? iw : ih);
      cairo_save(cr);
      cairo_translate(cr, ix, iy);
      cairo_scale(cr, s, s);
      cairo_set_source_surface(cr, w->icon, 0, 0);
      cairo_paint(cr);
      cairo_restore(cr);
    } else {
      cairo_set_source_rgb(cr, 0.95, 0.95, 0.95);
      cairo_rectangle(cr, ix, iy, icon_px, icon_px);
      cairo_fill(cr);
    }

    // title
    const char *title = (w->title && *w->title) ? w->title :
                        (w->appid && *w->appid) ? w->appid : "(untitled)";
    int tx = ix + icon_px + 6;
    int tw = w->x + w->w - tx - 6;
    if (tw > 8) {
      cairo_set_source_rgb(cr, 1, 1, 1);
      cairo_move_to(cr, tx, w->y + (w->h+12)/2 - 2);
      draw_ellipsis_text(cr, title, tw);
    }
  }

  // finish frame
  cairo_destroy(cr);
  cairo_surface_destroy(cs);

  wl_surface_attach(surf, buf, 0, 0);
  wl_surface_damage_buffer(surf, 0, 0, screen_w, bar_h);
  wl_surface_commit(surf);

  strncpy(clock_buf, now_buf, sizeof(clock_buf)-1);
  clock_buf[sizeof(clock_buf)-1] = 0;
}

static void compact_wins(void){
  size_t j=0;
  for (size_t i=0;i<wins_len;i++){
    if (!wins[i]->closed) wins[j++] = wins[i];
    else {
      if (wins[i]->icon) cairo_surface_destroy(wins[i]->icon);
      free(wins[i]->title);
      free(wins[i]->appid);
      free(wins[i]);
    }
  }
  wins_len = j;
}

static void win_refresh_layout(void){
  compact_wins();
  paint();
}

// ----------------- listeners -----------------
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
  else if (!strcmp(iface, "zwlr_foreign_toplevel_manager_v1")) ftm = wl_registry_bind(r, name, &zwlr_foreign_toplevel_manager_v1_interface, 3);
  else if (!strcmp(iface, "wl_seat")) seat = wl_registry_bind(r, name, &wl_seat_interface, 7);
}
static void reg_global_remove(void *d, struct wl_registry *r, uint32_t n){}
static const struct wl_registry_listener reg_listener = { reg_global, reg_global_remove };

// foreign toplevel callbacks
static void tl_title(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *title){
  size_t n = title ? strlen(title) : 0;
  fprintf(stderr, "[ftm] title(len=%zu): \"%.*s\"", n, (int)n, title ? title : "");
  fflush(stderr);
  
  struct win_btn *w = d; free(w->title); w->title = title ? strdup(title) : strdup("");
  win_refresh_layout();
}
static void tl_appid(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *appid){
  size_t n = appid ? strlen(appid) : 0;
  fprintf(stderr, "[ftm] appid(len=%zu): \"%.*s\"", n, (int)n, appid ? appid : "");
  fflush(stderr);
  
  struct win_btn *w = d; free(w->appid); w->appid = appid ? strdup(appid) : strdup("");
  // icon lazy-loaded in paint()
  win_refresh_layout();
}
static void tl_state(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_array *state){ (void)d;(void)h;(void)state; }
static void tl_parent(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct zwlr_foreign_toplevel_handle_v1 *p){ (void)d;(void)h;(void)p; }
static void tl_output_enter(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o){ (void)d;(void)h;(void)o; }
static void tl_output_leave(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o){ (void)d;(void)h;(void)o; }
static void tl_done(void *d, struct zwlr_foreign_toplevel_handle_v1 *h){ (void)d;(void)h; }
static void tl_closed(void *d, struct zwlr_foreign_toplevel_handle_v1 *h){
  struct win_btn *w = d; w->closed = 1; win_refresh_layout();
}
static const struct zwlr_foreign_toplevel_handle_v1_listener tl_listener = {
  .title = tl_title,
  .app_id = tl_appid,
  .output_enter = tl_output_enter,
  .output_leave = tl_output_leave,
  .state = tl_state,
  .parent = tl_parent,
  .done = tl_done,
  .closed = tl_closed,
};

static void ftm_new(void *d, struct zwlr_foreign_toplevel_manager_v1 *mgr,
                    struct zwlr_foreign_toplevel_handle_v1 *hdl){
  fprintf(stderr, "[ftm] new toplevel handle=%p\n", (void*)hdl);
  fflush(stderr);
  
  if (wins_len == wins_cap) { wins_cap = wins_cap? wins_cap*2:8; wins = realloc(wins, wins_cap*sizeof*wins); }
  struct win_btn *w = calloc(1,sizeof* w);
  w->hdl = hdl;
  zwlr_foreign_toplevel_handle_v1_add_listener(hdl, &tl_listener, w);
  wins[wins_len++] = w;
  win_refresh_layout();
}
static void ftm_finished(void *d, struct zwlr_foreign_toplevel_manager_v1 *mgr){ (void)d;(void)mgr; }
static const struct zwlr_foreign_toplevel_manager_v1_listener ftm_listener = {
  .toplevel = ftm_new,
  .finished = ftm_finished,
};

// pointer events
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
    // Start button
    if (ptr_x >= start_btn.x && ptr_x < start_btn.x + start_btn.w &&
        ptr_y >= start_btn.y && ptr_y < start_btn.y + start_btn.h) {
      fprintf(stderr, "Start clicked!\n");
      return;
    }
    // Window button
    for (size_t i=0;i<wins_len;i++){
      struct win_btn *w = wins[i];
      if (w->closed) continue;
      if (ptr_x >= w->x && ptr_x < w->x + w->w &&
          ptr_y >= w->y && ptr_y < w->y + w->h) {
        if (seat) zwlr_foreign_toplevel_handle_v1_activate(w->hdl, seat);
        break;
      }
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
  logo = load_logo_flexible(argc, argv);

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
  if (ftm)  {zwlr_foreign_toplevel_manager_v1_add_listener(ftm, &ftm_listener, NULL);
  wl_display_roundtrip(dpy);
  }

  // surface
  surf = wl_compositor_create_surface(comp);
  lsurf = zwlr_layer_shell_v1_get_layer_surface(layer, surf, NULL,
            ZWLR_LAYER_SHELL_V1_LAYER_TOP, "bar-tasklist");
  // bottom dock
  zwlr_layer_surface_v1_set_anchor(lsurf,
      ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
      ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT   |
      ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
  zwlr_layer_surface_v1_set_size(lsurf, 0, bar_h);
  zwlr_layer_surface_v1_set_exclusive_zone(lsurf, bar_h);
  zwlr_layer_surface_v1_add_listener(lsurf, &lsurf_listener, NULL);
  wl_surface_commit(surf);

  // initial configure + first paint
  wl_display_roundtrip(dpy);

  // 1s clock
  tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (tfd < 0) { perror("timerfd_create"); return 1; }
  struct itimerspec its = {0};
  its.it_interval.tv_sec = 1; its.it_value.tv_sec = 1;
  if (timerfd_settime(tfd, 0, &its, NULL) < 0) { perror("timerfd_settime"); return 1; }
  format_clock(clock_buf);

  // event loop
  struct pollfd fds[2];
  fds[0].fd = wl_display_get_fd(dpy); fds[0].events = POLLIN;
  fds[1].fd = tfd;                    fds[1].events = POLLIN;

  for (;;) {
    if (wl_display_flush(dpy) < 0 && errno != EAGAIN) break;
    int ret = poll(fds, 2, -1);
    if (ret < 0) { if (errno == EINTR) continue; perror("poll"); break; }

    if (fds[0].revents & POLLIN) {
      if (wl_display_dispatch(dpy) < 0) break;
    }
    if (fds[1].revents & POLLIN) {
      uint64_t exp; (void)read(tfd, &exp, sizeof(exp));
      char now[32]; format_clock(now);
      if (strcmp(now, clock_buf) != 0) paint();
    }
  }

  // cleanup (never reached in normal run)
  if (logo) cairo_surface_destroy(logo);
  if (tfd >= 0) close(tfd);
  return 0;
}