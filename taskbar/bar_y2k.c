// bar_with_start_menu.c
// taskbar: Start + Win2000-style task buttons (min/max width) + right boxed clock
// PLUS: XP-like Start menu (header full-width, avatar top-right, square Power button)
// PLUS: "All Programs" panel on the right (parses .desktop files)
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
#include <dirent.h>
#include <math.h>

#include <wayland-client.h>
#include <cairo/cairo.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

// Forward declare in case of include order
struct zwlr_foreign_toplevel_handle_v1;

// ---------- Globals ----------
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
static int tfd = -1;
static char clock_buf[32] = "";

// ---------- Start menu globals ----------
static int menu_visible = 0;
static int menu_w = 420;
static int menu_h = 528;

static struct wl_surface *menu_surf = NULL;
static struct zwlr_layer_surface_v1 *menu_lsurf = NULL;
static double menu_px = -1, menu_py = -1;  // pointer within menu

struct recti { int x,y,w,h; const char *id; };
#define PINNED_N 6
#define RIGHT_N  5
static struct recti pinned_rects[PINNED_N];
static struct recti right_rects[RIGHT_N];
static struct recti power_btn;
static struct recti all_programs_btn;
static int all_programs_open = 0;

// "All programs" data
#define MAX_APPS 512
#define MAX_APPS_DRAW 30
static char *app_names[MAX_APPS];
static int   app_count = 0;
static struct recti app_rects[MAX_APPS_DRAW];

// ---------- Y2k palette ----------
static const struct { double r,g,b; } COL_BG      = {0.82, 0.82, 0.82}; // taskbar base grey
static const struct { double r,g,b; } COL_BEVEL_H = {1.00, 1.00, 1.00}; // top/left highlight
static const struct { double r,g,b; } COL_BEVEL_S = {0.55, 0.55, 0.55}; // bottom/right shadow
static const struct { double r,g,b; } COL_BTN     = {0.86, 0.86, 0.86}; // button face
static const struct { double r,g,b; } COL_BTN_HOV = {0.137, 0.769, 0.878}; // aqua hover
static const struct { double r,g,b; } COL_BTN_ACT = {0.106, 0.608, 0.702}; // active (deeper aqua)
static const struct { double r,g,b; } COL_TEXT    = {0.07, 0.07, 0.07}; // dark text
static const struct { double r,g,b; } COL_TEXT_INV= {1.00, 1.00, 1.00}; // inverse text (on aqua)
static const struct { double r,g,b; } COL_BOX     = {0.90, 0.90, 0.90}; // right clock box fill

// ---------- Task list data ----------
struct win_btn {
  struct zwlr_foreign_toplevel_handle_v1 *hdl;
  char *title;
  char *appid;
  cairo_surface_t *icon;
  int x,y,w,h;
  int closed;
  int active; // set via tl_state
};

static struct win_btn **wins = NULL;
static size_t wins_len = 0, wins_cap = 0;

// ---------- helpers ----------
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
  strftime(out, 32, "%I:%M %p", &lt); // 12h format like Win2k tray
  if (out[0] == '0') memmove(out, out+1, strlen(out));
}

static cairo_surface_t *try_png(const char *p){
  cairo_surface_t *s = cairo_image_surface_create_from_png(p);
  if (cairo_surface_status(s) == CAIRO_STATUS_SUCCESS) return s;
  cairo_surface_destroy(s); return NULL;
}

// very small icon theme lookup
static const char* sizes_arr[] = {"16","22","24","32"};
static cairo_surface_t* load_icon_for_appid(const char *appid){
  if (!appid || !*appid) return NULL;
  char p[PATH_MAX];
  for (size_t i=0;i<sizeof(sizes_arr)/sizeof(sizes_arr[0]);i++){
    snprintf(p,sizeof(p),"/usr/share/icons/hicolor/%sx%s/apps/%s.png", sizes_arr[i],sizes_arr[i], appid);
    cairo_surface_t *s = try_png(p); if (s) return s;
    snprintf(p,sizeof(p),"/usr/share/icons/hicolor/%sx%s/apps/%s-symbolic.png", sizes_arr[i],sizes_arr[i], appid);
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
    char exe[PATH_MAX] = {0};
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe)-1);
    if (n > 0) exe[n] = 0; else strcpy(exe, ".");
    char exe_copy[PATH_MAX]; strncpy(exe_copy, exe, sizeof(exe_copy)-1); exe_copy[sizeof(exe_copy)-1]=0;
    char *dir = dirname(exe_copy);
    snprintf(path, sizeof(path), "%s/../assets/icons/logo.png", dir);
    struct stat st;
    if (stat(path, &st) != 0) snprintf(path, sizeof(path), "%s/assets/icons/logo.png", dir);
  }
  struct stat st;
  if (stat(path, &st) != 0) return NULL;
  cairo_surface_t *s = cairo_image_surface_create_from_png(path);
  if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) { cairo_surface_destroy(s); return NULL; }
  return s;
}

static void draw_text(cairo_t *cr, double r,double g,double b, const char *txt, double x, double y){
  cairo_set_source_rgb(cr, r,g,b);
  cairo_move_to(cr, x, y);
  cairo_show_text(cr, txt?txt:"");
}

static void bevel_rect(cairo_t *cr, int x,int y,int w,int h, int inset){
  cairo_set_source_rgb(cr, COL_BEVEL_H.r, COL_BEVEL_H.g, COL_BEVEL_H.b);
  cairo_move_to(cr, x, y+h-1-inset); cairo_line_to(cr, x, y+inset); cairo_line_to(cr, x+w-1-inset, y+inset); cairo_stroke(cr);
  cairo_set_source_rgb(cr, COL_BEVEL_S.r, COL_BEVEL_S.g, COL_BEVEL_S.b);
  cairo_move_to(cr, x+w-1-inset, y+inset); cairo_line_to(cr, x+w-1-inset, y+h-1-inset); cairo_line_to(cr, x+inset, y+h-1-inset); cairo_stroke(cr);
}

// ---------- layout constants ----------
static int PAD_LR = 6, PAD_RIGHT = 6;
static int START_LABEL_BOLD = 1;
static const int ICON_PX = 18;
static const int BTN_VPAD = 6;
static const int BTN_MIN_W = 120;
static const int BTN_MAX_W = 220;
static const int CLOCK_BOX_MIN_W = 110; // room for tray later
static const int CLOCK_PAD = 8;

// computed per frame
static int start_w_cached = 110;
static int clock_box_w = 120;

// ---------- Start menu helpers ----------
static void ensure_menu(void);
static void paint_menu(void);
static void toggle_menu(int show){
  if (show) {
    ensure_menu();
    menu_visible = 1;
    paint_menu();
  } else {
    menu_visible = 0;
    all_programs_open = 0;
    for (int i=0;i<app_count;i++) { free(app_names[i]); app_names[i]=NULL; }
    app_count = 0;
    if (menu_lsurf) { zwlr_layer_surface_v1_destroy(menu_lsurf); menu_lsurf = NULL; }
    if (menu_surf)  { wl_surface_destroy(menu_surf); menu_surf = NULL; }
  }
}

// ---------- Task list maintenance ----------
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

// ---------- paint BAR ----------
static void paint(void){
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

  cairo_set_source_rgb(cr, COL_BG.r, COL_BG.g, COL_BG.b);
  cairo_paint(cr);

  // LEFT: Start
  const char *label = "Start";
  int pad = 4;
  int icon_box = bar_h - 2*pad;
  int x = PAD_LR, y = (bar_h - icon_box)/2;

  cairo_select_font_face(cr, "Sans",
      CAIRO_FONT_SLANT_NORMAL, START_LABEL_BOLD ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 14.0);
  cairo_text_extents_t ext; cairo_text_extents(cr, label, &ext);

  int text_x = x + icon_box + 8;
  int text_y = (bar_h + (int)(ext.height)) / 2 - 2;
  int btn_w = (text_x - x) + (int)ext.width + 8;
  int btn_h = bar_h - 4; int btn_y = (bar_h - btn_h)/2;

  start_btn.x = x-1; start_btn.y = 0; start_btn.w = btn_w+2; start_btn.h = bar_h;
  start_w_cached = start_btn.w;

  int hover = (ptr_x >= start_btn.x && ptr_x < start_btn.x + start_btn.w &&
               ptr_y >= start_btn.y && ptr_y < start_btn.y + start_btn.h);

  cairo_set_source_rgb(cr,
    hover ? COL_BTN_HOV.r : COL_BTN.r,
    hover ? COL_BTN_HOV.g : COL_BTN.g,
    hover ? COL_BTN_HOV.b : COL_BTN.b);
  cairo_rectangle(cr, start_btn.x, btn_y, btn_w, btn_h);
  cairo_fill(cr);
  bevel_rect(cr, start_btn.x, btn_y, btn_w, btn_h, 0);

  if (logo) {
    double w = cairo_image_surface_get_width(logo);
    double h = cairo_image_surface_get_height(logo);
    double scale = (double)icon_box / (w > h ? w : h);
    double dw = w * scale, dh = h * scale;
    cairo_save(cr);
    cairo_translate(cr, x + (icon_box - dw)/2.0, y + (icon_box - dh)/2.0);
    cairo_scale(cr, scale, scale);
    cairo_set_source_surface(cr, logo, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
  } else {
    cairo_set_source_rgb(cr, 0.95, 0.95, 0.95);
    cairo_rectangle(cr, x, y, icon_box, icon_box);
    cairo_fill(cr);
  }

  draw_text(cr, hover ? COL_TEXT_INV.r : COL_TEXT.r,
               hover ? COL_TEXT_INV.g : COL_TEXT.g,
               hover ? COL_TEXT_INV.b : COL_TEXT.b,
               label, text_x, text_y);

  // RIGHT: Clock boxed
  char now_buf[32]; format_clock(now_buf);
  cairo_text_extents_t cte; cairo_text_extents(cr, now_buf, &cte);
  int clock_text_w = (int)cte.width;
  int tray_stub_w = 0; // future tray width
  clock_box_w = CLOCK_BOX_MIN_W;
  int needed = clock_text_w + 2*CLOCK_PAD + tray_stub_w;
  if (needed > clock_box_w) clock_box_w = needed;

  int box_x = screen_w - PAD_RIGHT - clock_box_w;
  int box_y = 3, box_h = bar_h - 6;
  cairo_set_source_rgb(cr, COL_BOX.r, COL_BOX.g, COL_BOX.b);
  cairo_rectangle(cr, box_x, box_y, clock_box_w, box_h);
  cairo_fill(cr);
  bevel_rect(cr, box_x, box_y, clock_box_w, box_h, 0);

  int cx = box_x + clock_box_w - CLOCK_PAD - clock_text_w;
  int cy = (bar_h + (int)cte.height)/2 - 2;
  draw_text(cr, COL_TEXT.r, COL_TEXT.g, COL_TEXT.b, now_buf, cx, cy);

  // CENTER: Task buttons
  compact_wins();

  int area_x = start_w_cached + PAD_LR + 4;
  int area_w = box_x - 8 - area_x;
  int btn_hh = bar_h - BTN_VPAD;
  int btn_y2 = (bar_h - btn_hh)/2;

  int n = (int)wins_len;
  int each = n ? (area_w / n) : 0;
  if (each > BTN_MAX_W) each = BTN_MAX_W;
  if (n && each < BTN_MIN_W) each = BTN_MIN_W;

  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 12.0);

  int xcursor = area_x;
  for (size_t i=0;i<wins_len;i++){
    struct win_btn *w = wins[i];
    if (w->closed) continue;

    w->x = xcursor; w->y = btn_y2; w->w = each; w->h = btn_hh;
    xcursor += each + 4;
    if (w->x + w->w > box_x - 4) { w->w = (box_x - 4) - w->x; if (w->w < 20) break; }

    int hov = (ptr_x >= w->x && ptr_x < w->x + w->w && ptr_y >= w->y && ptr_y < w->y + w->h);
    int act = w->active;

    if (act) cairo_set_source_rgb(cr, COL_BTN_ACT.r, COL_BTN_ACT.g, COL_BTN_ACT.b);
    else if (hov) cairo_set_source_rgb(cr, COL_BTN_HOV.r, COL_BTN_HOV.g, COL_BTN_HOV.b);
    else          cairo_set_source_rgb(cr, COL_BTN.r,     COL_BTN.g,     COL_BTN.b);
    cairo_rectangle(cr, w->x, w->y, w->w, w->h);
    cairo_fill(cr);
    bevel_rect(cr, w->x, w->y, w->w, w->h, 0);

    int ix = w->x + 6, iy = w->y + (w->h - ICON_PX)/2;
    if (!w->icon && w->appid) w->icon = load_icon_for_appid(w->appid);
    if (w->icon) {
      double iw = cairo_image_surface_get_width(w->icon);
      double ih = cairo_image_surface_get_height(w->icon);
      double s = (double)ICON_PX / (iw > ih ? iw : ih);
      cairo_save(cr);
      cairo_translate(cr, ix, iy);
      cairo_scale(cr, s, s);
      cairo_set_source_surface(cr, w->icon, 0, 0);
      cairo_paint(cr);
      cairo_restore(cr);
    } else {
      cairo_set_source_rgb(cr, 0.75, 0.75, 0.75);
      cairo_rectangle(cr, ix, iy, ICON_PX, ICON_PX);
      cairo_fill(cr);
    }

    const char *title = (w->title && *w->title) ? w->title :
                        (w->appid && *w->appid) ? w->appid : "(untitled)";
    int tx = ix + ICON_PX + 6;
    int tw = w->x + w->w - tx - 6;
    if (tw > 8) {
      cairo_text_extents_t te; cairo_text_extents(cr, title, &te);
      double tr = (hov || act) ? COL_TEXT_INV.r : COL_TEXT.r;
      double tg = (hov || act) ? COL_TEXT_INV.g : COL_TEXT.g;
      double tb = (hov || act) ? COL_TEXT_INV.b : COL_TEXT.b;
      cairo_set_source_rgb(cr, tr,tg,tb);
      if ((int)te.width <= tw) {
        cairo_move_to(cr, tx, w->y + (w->h+12)/2 - 2);
        cairo_show_text(cr, title);
      } else {
        char buf[256]; strncpy(buf, title, sizeof(buf)-1); buf[sizeof(buf)-1]=0;
        int ncut = (int)strlen(buf);
        while (ncut>0) {
          buf[ncut--] = 0;
          char tmp[260]; snprintf(tmp, sizeof(tmp), "%s...", buf);
          cairo_text_extents(cr, tmp, &te);
          if ((int)te.width <= tw) {
            cairo_move_to(cr, tx, w->y + (w->h+12)/2 - 2);
            cairo_show_text(cr, tmp);
            break;
          }
        }
      }
    }
  }

  cairo_destroy(cr);
  cairo_surface_destroy(cs);
  wl_surface_attach(surf, buf, 0, 0);
  wl_surface_damage_buffer(surf, 0, 0, screen_w, bar_h);
  wl_surface_commit(surf);

  strncpy(clock_buf, now_buf, sizeof(clock_buf)-1);
  clock_buf[sizeof(clock_buf)-1] = 0;
}

// ---- utilities for "All Programs" (.desktop) ----
static int cmp_strptr(const void *a, const void *b){
  const char *sa = *(const char * const *)a;
  const char *sb = *(const char * const *)b;
  return strcmp(sa?sa:"", sb?sb:"");
}

static void add_app(const char *name){
  if (!name || !*name) return;
  if (app_count >= MAX_APPS) return;
  app_names[app_count++] = strdup(name);
}

static void scan_desktop_dir(const char *dir){
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *de;
  char path[PATH_MAX];
  while ((de = readdir(d))){
    size_t len = strlen(de->d_name);
    if (len < 8 || strcmp(de->d_name+len-8, ".desktop") != 0) continue;
    snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
    FILE *f = fopen(path, "r");
    if (!f) continue;
    char line[1024]; char *name = NULL;
    while (fgets(line, sizeof(line), f)){
      if (!strncmp(line, "Name=", 5)) {
        char *v = line+5;
        // strip newline
        size_t L = strlen(v);
        while (L>0 && (v[L-1]=='\n' || v[L-1]=='\r')) v[--L]=0;
        name = strdup(v);
        break;
      }
    }
    fclose(f);
    if (name) { add_app(name); free(name); }
  }
  closedir(d);
}

static void load_all_programs(void){
  // clear previous
  for (int i=0;i<app_count;i++) { free(app_names[i]); app_names[i]=NULL; }
  app_count = 0;

  const char *home = getenv("HOME");
  char p[PATH_MAX];
  if (home){
    snprintf(p, sizeof(p), "%s/.local/share/applications", home);
    scan_desktop_dir(p);
  }
  scan_desktop_dir("/usr/share/applications");

  if (app_count > 1) qsort(app_names, app_count, sizeof(app_names[0]), cmp_strptr);
}

// ---------- Start MENU paint (with All Programs panel) ----------
static void paint_menu(void){
  if (!menu_visible || !menu_surf) return;

  int stride = menu_w * 4;
  int size = stride * menu_h;
  int fd = mkshm(size);
  struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
  void *data = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
  if (data == MAP_FAILED) { perror("mmap(menu)"); return; }
  struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, menu_w, menu_h, stride, WL_SHM_FORMAT_XRGB8888);
  wl_shm_pool_destroy(pool);
  close(fd);

  cairo_surface_t *cs = cairo_image_surface_create_for_data((unsigned char*)data,
                            CAIRO_FORMAT_RGB24, menu_w, menu_h, stride);
  cairo_t *cr = cairo_create(cs);

  // Base split
  cairo_set_source_rgb(cr, 0.18, 0.40, 0.76);
  cairo_rectangle(cr, 0, 0, 260, menu_h);
  cairo_fill(cr);
  cairo_set_source_rgb(cr, 0.94, 0.94, 0.94);
  cairo_rectangle(cr, 260, 0, menu_w-260, menu_h);
  cairo_fill(cr);

  // FULL-WIDTH HEADER
  int hdr_h = 80;
  cairo_set_source_rgb(cr, 0.18, 0.40, 0.76);
  cairo_rectangle(cr, 0, 0, menu_w, hdr_h);
  cairo_fill(cr);
  cairo_set_source_rgba(cr, 1,1,1, 0.25);
  cairo_move_to(cr, 0, hdr_h + 0.5); cairo_line_to(cr, menu_w, hdr_h + 0.5); cairo_stroke(cr);

  // Avatar + name
  int av = 56;
  int avx = menu_w - av - 10;
  int avy = (hdr_h - av)/2;
  cairo_set_source_rgb(cr, 0.95,0.95,0.95);
  cairo_rectangle(cr, avx, avy, av, av); cairo_fill(cr);
  cairo_set_source_rgb(cr, 0.7,0.7,0.7);
  cairo_rectangle(cr, avx+0.5, avy+0.5, av-1, av-1); cairo_stroke(cr);
  const char *user = getenv("USER"); if (!user) user = "User";
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 16.0);
  cairo_set_source_rgb(cr, 1,1,1);
  cairo_text_extents_t name_te; cairo_text_extents(cr, user, &name_te);
  double name_x = avx - 12 - name_te.width;
  double name_y = hdr_h/2 + name_te.height/2 - 2;
  cairo_move_to(cr, name_x, name_y); cairo_show_text(cr, user);

  // Left title
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 12.0);
  cairo_set_source_rgba(cr, 1,1,1, 0.80);
  cairo_move_to(cr, 12, hdr_h - 12); cairo_show_text(cr, "Pinned");

  // Pinned
  const char *pinned[PINNED_N] = {"Browser","Mail","Files","Music","Photos","Notepad"};
  int y = hdr_h + 10;
  int pinned_height = 56;
  cairo_set_font_size(cr, 14.0);
  for (int i=0;i<PINNED_N;i++){
    int ih = pinned_height;
    int hov = (menu_px >= 8 && menu_px < 260-8 && menu_py >= y && menu_py < y+ih);
    cairo_set_source_rgb(cr, hov ? COL_BTN_HOV.r:0.25, hov ? COL_BTN_HOV.g:0.50, hov ? COL_BTN_HOV.b:0.80);
    cairo_rectangle(cr, 8, y, 260-16, ih); cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.95,0.95,0.95);
    int ico = 28; cairo_rectangle(cr, 14, y + (ih-ico)/2, ico, ico); cairo_fill(cr);
    cairo_set_source_rgb(cr, 1,1,1);
    cairo_move_to(cr, 14+ico+10, y + (ih+14)/2 - 3); cairo_show_text(cr, pinned[i]);
    pinned_rects[i] = (struct recti){8, y, 260-16, ih, pinned[i]};
    y += ih + 6;
  }

  // "All Programs" button at bottom of left column
  int ap_h = 40;
  int ap_y = menu_h - ap_h - 10;
  all_programs_btn = (struct recti){8, ap_y, 260-16, ap_h, "All Programs"};
  int ap_hov = (menu_px >= all_programs_btn.x && menu_px < all_programs_btn.x+all_programs_btn.w &&
                menu_py >= all_programs_btn.y && menu_py < all_programs_btn.y+all_programs_btn.h);
  cairo_set_source_rgb(cr, ap_hov ? COL_BTN_HOV.r:0.22, ap_hov ? COL_BTN_HOV.g:0.46, ap_hov ? COL_BTN_HOV.b:0.76);
  cairo_rectangle(cr, all_programs_btn.x, all_programs_btn.y, all_programs_btn.w, all_programs_btn.h); cairo_fill(cr);
  cairo_set_source_rgb(cr, 1,1,1);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 13.5);
  cairo_text_extents_t ap_te; cairo_text_extents(cr, all_programs_btn.id, &ap_te);
  cairo_move_to(cr, all_programs_btn.x + (all_programs_btn.w - ap_te.width)/2,
                    all_programs_btn.y + (all_programs_btn.h + ap_te.height)/2 - 2);
  cairo_show_text(cr, all_programs_btn.id);

  // Right shortcuts (hidden by All Programs panel when open)
  const char *shortcuts[RIGHT_N] = {"My Computer","Documents","Pictures","Control Panel","Run..."};
  int rx = 260 + 10; y = hdr_h + 10;
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 13.0);
  if (!all_programs_open){
    for (int i=0;i<RIGHT_N;i++){
      int ih = 32;
      int hov = (menu_px >= rx && menu_px < menu_w-10 && menu_py >= y && menu_py < y+ih);
      cairo_set_source_rgb(cr, hov ? 0.85:0.94, hov ? 0.85:0.94, hov ? 0.85:0.94);
      cairo_rectangle(cr, rx, y, (menu_w-10)-rx, ih); cairo_fill(cr);
      cairo_set_source_rgb(cr, 0.2,0.2,0.2);
      cairo_move_to(cr, rx+8, y + (ih+12)/2 - 2); cairo_show_text(cr, shortcuts[i]);
      right_rects[i] = (struct recti){rx, y, (menu_w-10)-rx, ih, shortcuts[i]};
      y += ih + 4;
    }
  }

  // Square Power button bottom-right
  const int PWR_SZ = 36;
  int bx = menu_w - 10 - PWR_SZ;
  int by = menu_h - 10 - PWR_SZ;
  power_btn = (struct recti){bx, by, PWR_SZ, PWR_SZ, "Power"};
  cairo_set_source_rgb(cr, 0.90,0.90,0.90);
  cairo_rectangle(cr, bx, by, PWR_SZ, PWR_SZ); cairo_fill(cr);
  bevel_rect(cr, bx, by, PWR_SZ, PWR_SZ, 0);
  cairo_set_line_width(cr, 2.0);
  cairo_set_source_rgb(cr, 0.15,0.15,0.15);
  double pcx = bx + PWR_SZ/2.0, pcy = by + PWR_SZ/2.0;
  double pr = (PWR_SZ/2.0) - 6.0;
  cairo_arc(cr, pcx, pcy+1, pr, 0.0, 2.0*M_PI); cairo_stroke(cr);
  cairo_move_to(cr, pcx, by + 6); cairo_line_to(cr, pcx, by + PWR_SZ/2.0); cairo_stroke(cr);

  // --- All Programs PANEL (overlays right side)
  if (all_programs_open){
    int px = 260;                  // panel starts at the right edge of left blue column
    int pw = menu_w - px - 10;     // leave 10px right margin
    int py = hdr_h + 10;
    int ph = by - 10 - py;         // up to above the power button

    // panel bg
    cairo_set_source_rgb(cr, 0.98, 0.98, 0.98);
    cairo_rectangle(cr, px, py, pw, ph); cairo_fill(cr);
    bevel_rect(cr, px, py, pw, ph, 0);

    // title
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 13.0);
    cairo_set_source_rgb(cr, 0.1,0.1,0.1);
    cairo_move_to(cr, px+8, py+18); cairo_show_text(cr, "All Programs");
    int list_y = py + 26;

    // draw first N apps
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12.0);
    int drawn = 0;
    for (int i=0;i<app_count && drawn<MAX_APPS_DRAW; i++){
      int ih = 28;
      int rw = pw - 8 - 8;
      int ry = list_y + drawn*(ih+4);
      if (ry + ih > py + ph - 6) break;
      int hov = (menu_px >= px+8 && menu_px < px+8+rw && menu_py >= ry && menu_py < ry+ih);
      cairo_set_source_rgb(cr, hov ? 0.88:0.96, hov ? 0.88:0.96, hov ? 0.88:0.96);
      cairo_rectangle(cr, px+8, ry, rw, ih); cairo_fill(cr);
      cairo_set_source_rgb(cr, 0.15,0.15,0.15);
      cairo_move_to(cr, px+12, ry + (ih+12)/2 - 2); cairo_show_text(cr, app_names[i]);
      app_rects[drawn] = (struct recti){px+8, ry, rw, ih, app_names[i]};
      drawn++;
    }
    // (no scroll yet; next step)
  }

  // Outer border
  cairo_set_source_rgb(cr, COL_BEVEL_H.r, COL_BEVEL_H.g, COL_BEVEL_H.b);
  cairo_rectangle(cr, 0.5, 0.5, menu_w-1, menu_h-1); cairo_stroke(cr);

  cairo_destroy(cr);
  cairo_surface_destroy(cs);
  wl_surface_attach(menu_surf, buf, 0, 0);
  wl_surface_damage_buffer(menu_surf, 0, 0, menu_w, menu_h);
  wl_surface_commit(menu_surf);
}

// ---------- menu layer listeners ----------
static void menu_config(void *data, struct zwlr_layer_surface_v1 *ls,
                         uint32_t serial, uint32_t w, uint32_t h){
  if (w) menu_w = (int)w;
  if (h) menu_h = (int)h;
  zwlr_layer_surface_v1_ack_configure(ls, serial);
  paint_menu();
}
static void menu_closed(void *data, struct zwlr_layer_surface_v1 *ls){
  (void)data; (void)ls;
}
// after menu_config() and menu_closed()
static const struct zwlr_layer_surface_v1_listener menu_listener = {
  .configure = menu_config,
  .closed    = menu_closed,
};

// create menu layer
static void ensure_menu(void){
  if (menu_surf) return;
  menu_surf = wl_compositor_create_surface(comp);
  menu_lsurf = zwlr_layer_shell_v1_get_layer_surface(layer, menu_surf, NULL,
              ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "start-menu");
  zwlr_layer_surface_v1_set_anchor(menu_lsurf,
      ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM);
  zwlr_layer_surface_v1_set_size(menu_lsurf, menu_w, menu_h);
  // place above bar using bottom margin = bar height
  zwlr_layer_surface_v1_set_margin(menu_lsurf, 0, 0, bar_h, 0);
  zwlr_layer_surface_v1_set_exclusive_zone(menu_lsurf, 0);
  zwlr_layer_surface_v1_add_listener(menu_lsurf, &menu_listener, NULL);
  wl_surface_commit(menu_surf);
  wl_display_roundtrip(dpy);
}

// ---------- protocol + events ----------
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

// foreign toplevel callbacks (track active)
static void tl_title(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *title){
  struct win_btn *w = d; free(w->title); w->title = title ? strdup(title) : strdup("");
  paint();
}
static void tl_appid(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *appid){
  struct win_btn *w = d; free(w->appid); w->appid = appid ? strdup(appid) : strdup("");
  paint();
}
static void tl_state(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_array *state){
  struct win_btn *w = d; w->active = 0;
  uint32_t *s;
  wl_array_for_each(s, state){
#ifdef ZWLR_FOREIGN_TOPLEVEL_STATE_ACTIVATED
    if (*s == ZWLR_FOREIGN_TOPLEVEL_STATE_ACTIVATED) w->active = 1;
#else
    if (*s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED) w->active = 1;
#endif
  }
  paint();
}
static void tl_parent(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct zwlr_foreign_toplevel_handle_v1 *p){ (void)d;(void)h;(void)p; }
static void tl_output_enter(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o){ (void)d;(void)h;(void)o; }
static void tl_output_leave(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o){ (void)d;(void)h;(void)o; }
static void tl_done(void *d, struct zwlr_foreign_toplevel_handle_v1 *h){ (void)d;(void)h; }
static void tl_closed(void *d, struct zwlr_foreign_toplevel_handle_v1 *h){
  struct win_btn *w = d; w->closed = 1; paint();
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
  if (wins_len == wins_cap) { wins_cap = wins_cap? wins_cap*2:8; wins = realloc(wins, wins_cap*sizeof*wins); }
  struct win_btn *w = calloc(1,sizeof* w);
  w->hdl = hdl; w->active = 0;
  zwlr_foreign_toplevel_handle_v1_add_listener(hdl, &tl_listener, w);
  wins[wins_len++] = w;
  paint();
}
static void ftm_finished(void *d, struct zwlr_foreign_toplevel_manager_v1 *mgr){ (void)d;(void)mgr; }
static const struct zwlr_foreign_toplevel_manager_v1_listener ftm_listener = {
  .toplevel = ftm_new,
  .finished = ftm_finished,
};

// ---------- pointer routing (bar + menu) ----------
static void ptr_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t sx, wl_fixed_t sy){
  (void)d;(void)p;(void)serial;
  if (s == surf) { ptr_x = wl_fixed_to_double(sx); ptr_y = wl_fixed_to_double(sy); paint(); }
  else if (s == menu_surf) { menu_px = wl_fixed_to_double(sx); menu_py = wl_fixed_to_double(sy); paint_menu(); }
}
static void ptr_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s){
  (void)d;(void)p;(void)serial;
  if (s == surf) { ptr_x = ptr_y = -1; paint(); }
  if (s == menu_surf) { menu_px = menu_py = -1; paint_menu(); }
}
static void ptr_motion(void *d, struct wl_pointer *p, uint32_t time, wl_fixed_t sx, wl_fixed_t sy){
  (void)d;(void)p;(void)time;
  if (menu_visible && menu_surf) {
    menu_px = wl_fixed_to_double(sx); menu_py = wl_fixed_to_double(sy); paint_menu();
  } else {
    ptr_x = wl_fixed_to_double(sx); ptr_y = wl_fixed_to_double(sy); paint();
  }
}
static void ptr_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t time, uint32_t button, uint32_t state){
  (void)d;(void)p;(void)serial;(void)time;
  const uint32_t BTN_LEFT = 0x110;
  if (button == BTN_LEFT && state == 1) {
    // Start toggles menu
    if (ptr_x >= start_btn.x && ptr_x < start_btn.x + start_btn.w &&
        ptr_y >= start_btn.y && ptr_y < start_btn.y + start_btn.h) {
      toggle_menu(!menu_visible);
      return;
    }
    // Menu clicks
    if (menu_visible && menu_surf) {
      // All Programs
      if (menu_px >= all_programs_btn.x && menu_px < all_programs_btn.x+all_programs_btn.w &&
          menu_py >= all_programs_btn.y && menu_py < all_programs_btn.y+all_programs_btn.h) {
        if (!all_programs_open) load_all_programs();
        all_programs_open = !all_programs_open;
        paint_menu();
        return;
      }
      // Pinned shortcuts (stub)
      for (int i=0;i<PINNED_N;i++){
        struct recti r = pinned_rects[i];
        if (menu_px >= r.x && menu_px < r.x+r.w && menu_py >= r.y && menu_py < r.y+r.h) {
          fprintf(stderr, "Start menu: launch pinned \"%s\"\n", r.id);
          toggle_menu(0);
          return;
        }
      }
      // Right shortcuts (when all_programs_open==0)
      if (!all_programs_open) {
        for (int i=0;i<RIGHT_N;i++){
          struct recti r = right_rects[i];
          if (menu_px >= r.x && menu_px < r.x+r.w && menu_py >= r.y && menu_py < r.y+r.h) {
            fprintf(stderr, "Start menu: launch shortcut \"%s\"\n", r.id);
            toggle_menu(0);
            return;
          }
        }
      } else {
        // Click an app in All Programs panel
        for (int i=0;i<MAX_APPS_DRAW; i++){
          struct recti r = app_rects[i];
          if (!r.w) break;
          if (menu_px >= r.x && menu_px < r.x+r.w && menu_py >= r.y && menu_py < r.y+r.h) {
            fprintf(stderr, "Launch: %s\n", r.id ? r.id : "(unknown)");
            toggle_menu(0);
            return;
          }
        }
      }
      // Power
      if (menu_px >= power_btn.x && menu_px < power_btn.x + power_btn.w &&
          menu_py >= power_btn.y && menu_py < power_btn.y + power_btn.h) {
        fprintf(stderr, "Start menu: Power clicked\n");
        toggle_menu(0);
        return;
      }
    }
    // Task activation
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
static void ptr_axis(void *d, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t value){ (void)d;(void)p;(void)time;(void)axis;(void)value; }
static void ptr_frame(void *d, struct wl_pointer *p){ (void)d;(void)p; }
static void ptr_axis_src(void *d, struct wl_pointer *p, uint32_t src){ (void)d;(void)p;(void)src; }
static const struct wl_pointer_listener pointer_listener = {
  .enter = ptr_enter, .leave = ptr_leave, .motion = ptr_motion,
  .button = ptr_button, .axis = ptr_axis, .frame = ptr_frame, .axis_source = ptr_axis_src,
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
static void seat_name(void *d, struct wl_seat *s, const char *name){ (void)d;(void)s;(void)name; }
static const struct wl_seat_listener seat_listener = { seat_cap, seat_name };

// ---------- main ----------
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
  if (ftm) {
    zwlr_foreign_toplevel_manager_v1_add_listener(ftm, &ftm_listener, NULL);
    wl_display_roundtrip(dpy);
  }

  // Taskbar
  surf = wl_compositor_create_surface(comp);
  lsurf = zwlr_layer_shell_v1_get_layer_surface(layer, surf, NULL,
            ZWLR_LAYER_SHELL_V1_LAYER_TOP, "bar");
  zwlr_layer_surface_v1_set_anchor(lsurf,
      ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
      ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT   |
      ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
  zwlr_layer_surface_v1_set_size(lsurf, 0, bar_h);
  zwlr_layer_surface_v1_set_exclusive_zone(lsurf, bar_h);
  zwlr_layer_surface_v1_add_listener(lsurf, &lsurf_listener, NULL);
  wl_surface_commit(surf);

  wl_display_roundtrip(dpy);
  paint();

  // Timer for clock
  tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (tfd < 0) { perror("timerfd_create"); return 1; }
  struct itimerspec its = {0};
  its.it_interval.tv_sec = 1; its.it_value.tv_sec = 1;
  if (timerfd_settime(tfd, 0, &its, NULL) < 0) { perror("timerfd_settime"); return 1; }
  format_clock(clock_buf);

  // Event loop
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

  if (menu_lsurf) zwlr_layer_surface_v1_destroy(menu_lsurf);
  if (menu_surf) wl_surface_destroy(menu_surf);
  if (logo) cairo_surface_destroy(logo);
  if (tfd >= 0) close(tfd);
  for (int i=0;i<app_count;i++) free(app_names[i]);
  return 0;
}