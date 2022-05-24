#ifndef _WMIIV_SURFACE_H
#define _WMIIV_SURFACE_H
#include <wlr/types/wlr_compositor.h>

struct wmiiv_surface {
	struct wlr_surface *wlr_surface;

	struct wl_listener destroy;

	/**
	 * This timer can be used for issuing delayed frame done callbacks (for
	 * example, to improve presentation latency). Its handler is set to a
	 * function that issues a frame done callback to this surface.
	 */
	struct wl_event_source *frame_done_timer;
};

#endif
