#ifndef HAYWARD_DECORATION_H
#define HAYWARD_DECORATION_H

#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/types/wlr_server_decoration.h>

struct hayward_server_decoration {
    struct wlr_server_decoration *wlr_server_decoration;
    struct wl_list link;

    struct wl_listener destroy;
    struct wl_listener mode;
};

struct hayward_server_decoration *
decoration_from_surface(struct wlr_surface *surface);

#endif
