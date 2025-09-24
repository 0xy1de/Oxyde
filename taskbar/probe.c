// SPDX-FileCopyrightText: 2025 Oxyde Contributors
// SPDX-License-Identifier: MPL-2.0
#include <stdio.h>
#include <string.h>
#include <wayland-client.h>
static void g(void* d, struct wl_registry* r, uint32_t name, const char* iface, uint32_t ver){
  if (strstr(iface, "zwlr_layer_shell_v1")) puts("OK: layer-shell present");
  if (strstr(iface, "zwlr_foreign_toplevel_manager_v1")) puts("OK: foreign-toplevel present");
}
static void gr(void* d, struct wl_registry* r, uint32_t n){}
static const struct wl_registry_listener L={g,gr};
int main(){ struct wl_display* d=wl_display_connect(NULL); if(!d){puts("no display");return 1;}
  struct wl_registry* r=wl_display_get_registry(d); wl_registry_add_listener(r,&L,NULL);
  wl_display_roundtrip(d); return 0; }