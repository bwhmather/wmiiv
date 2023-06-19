#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include "hayward/tree/view.h"

#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>
#include <wlr/xwayland/xwayland.h>

#include <hayward-common/log.h>
#include <hayward-common/pango.h>
#include <hayward-common/stringop.h>

#include <hayward/config.h>
#include <hayward/desktop/idle_inhibit_v1.h>
#include <hayward/desktop/xdg_shell.h>
#include <hayward/desktop/xwayland.h>
#include <hayward/globals/root.h>
#include <hayward/input/cursor.h>
#include <hayward/input/input-manager.h>
#include <hayward/input/seat.h>
#include <hayward/ipc-server.h>
#include <hayward/output.h>
#include <hayward/server.h>
#include <hayward/transaction.h>
#include <hayward/tree.h>
#include <hayward/tree/arrange.h>
#include <hayward/tree/column.h>
#include <hayward/tree/root.h>
#include <hayward/tree/window.h>
#include <hayward/tree/workspace.h>
#include <hayward/xdg_decoration.h>

#include <config.h>

void
view_init(
    struct hayward_view *view, enum hayward_view_type type,
    const struct hayward_view_impl *impl
) {
    view->scene_tree = wlr_scene_tree_create(root->orphans); // TODO
    hayward_assert(view->scene_tree != NULL, "Allocation failed");

    view->content_tree = wlr_scene_tree_create(view->scene_tree);
    hayward_assert(view->content_tree != NULL, "Allocation failed");

    view->type = type;
    view->impl = impl;
    view->allow_request_urgent = true;
    view->shortcuts_inhibit = SHORTCUTS_INHIBIT_DEFAULT;
    wl_signal_init(&view->events.unmap);
}

void
view_destroy(struct hayward_view *view) {
    hayward_assert(view->surface == NULL, "Tried to free mapped view");
    hayward_assert(
        view->destroying, "Tried to free view which wasn't marked as destroying"
    );
    hayward_assert(
        view->window == NULL,
        "Tried to free view which still has a container "
        "(might have a pending transaction?)"
    );
    wl_list_remove(&view->events.unmap.listener_list);

    wlr_scene_node_destroy(&view->content_tree->node);
    wlr_scene_node_destroy(&view->scene_tree->node);

    free(view->title_format);

    if (view->impl->destroy) {
        view->impl->destroy(view);
    } else {
        free(view);
    }
}

void
view_begin_destroy(struct hayward_view *view) {
    hayward_assert(view->surface == NULL, "Tried to destroy a mapped view");

    // Unmapping will mark the window as dead and trigger a transaction.  It
    // isn't safe to fully destroy the window until this transaction has
    // completed.  Setting `view->destroying` will tell the window to clean up
    // the view once it has finished cleaning up itself.
    view->destroying = true;
    if (!view->window) {
        view_destroy(view);
    }
}

const char *
view_get_title(struct hayward_view *view) {
    if (view->impl->get_string_prop) {
        return view->impl->get_string_prop(view, VIEW_PROP_TITLE);
    }
    return NULL;
}

const char *
view_get_app_id(struct hayward_view *view) {
    if (view->impl->get_string_prop) {
        return view->impl->get_string_prop(view, VIEW_PROP_APP_ID);
    }
    return NULL;
}

const char *
view_get_class(struct hayward_view *view) {
    if (view->impl->get_string_prop) {
        return view->impl->get_string_prop(view, VIEW_PROP_CLASS);
    }
    return NULL;
}

const char *
view_get_instance(struct hayward_view *view) {
    if (view->impl->get_string_prop) {
        return view->impl->get_string_prop(view, VIEW_PROP_INSTANCE);
    }
    return NULL;
}
#if HAVE_XWAYLAND
uint32_t
view_get_x11_window_id(struct hayward_view *view) {
    if (view->impl->get_int_prop) {
        return view->impl->get_int_prop(view, VIEW_PROP_X11_WINDOW_ID);
    }
    return 0;
}

uint32_t
view_get_x11_parent_id(struct hayward_view *view) {
    if (view->impl->get_int_prop) {
        return view->impl->get_int_prop(view, VIEW_PROP_X11_PARENT_ID);
    }
    return 0;
}
#endif
const char *
view_get_window_role(struct hayward_view *view) {
    if (view->impl->get_string_prop) {
        return view->impl->get_string_prop(view, VIEW_PROP_WINDOW_ROLE);
    }
    return NULL;
}

uint32_t
view_get_window_type(struct hayward_view *view) {
    if (view->impl->get_int_prop) {
        return view->impl->get_int_prop(view, VIEW_PROP_WINDOW_TYPE);
    }
    return 0;
}

const char *
view_get_shell(struct hayward_view *view) {
    switch (view->type) {
    case HAYWARD_VIEW_XDG_SHELL:
        return "xdg_shell";
#if HAVE_XWAYLAND
    case HAYWARD_VIEW_XWAYLAND:
        return "xwayland";
#endif
    }
    return "unknown";
}

void
view_get_constraints(
    struct hayward_view *view, double *min_width, double *max_width,
    double *min_height, double *max_height
) {
    if (view->impl->get_constraints) {
        view->impl->get_constraints(
            view, min_width, max_width, min_height, max_height
        );
    } else {
        *min_width = DBL_MIN;
        *max_width = DBL_MAX;
        *min_height = DBL_MIN;
        *max_height = DBL_MAX;
    }
}

uint32_t
view_configure(
    struct hayward_view *view, double lx, double ly, int width, int height
) {
    if (view->impl->configure) {
        return view->impl->configure(view, lx, ly, width, height);
    }
    return 0;
}

bool
view_inhibit_idle(struct hayward_view *view) {
    struct hayward_idle_inhibitor_v1 *application_inhibitor =
        hayward_idle_inhibit_v1_application_inhibitor_for_view(view);

    if (!application_inhibitor) {
        return false;
    }

    return hayward_idle_inhibit_v1_is_active(application_inhibitor);
}

void
view_set_activated(struct hayward_view *view, bool activated) {
    if (view->impl->set_activated) {
        view->impl->set_activated(view, activated);
    }
    if (view->foreign_toplevel) {
        wlr_foreign_toplevel_handle_v1_set_activated(
            view->foreign_toplevel, activated
        );
    }
}

void
view_request_activate(struct hayward_view *view) {
    struct hayward_workspace *workspace = view->window->pending.workspace;

    switch (config->focus_on_window_activation) {
    case FOWA_SMART:
        if (workspace_is_visible(workspace)) {
            root_set_focused_window(root, view->window);
        } else {
            view_set_urgent(view, true);
        }
        break;
    case FOWA_URGENT:
        view_set_urgent(view, true);
        break;
    case FOWA_FOCUS:
        root_set_focused_window(root, view->window);
        break;
    case FOWA_NONE:
        break;
    }
}

void
view_set_csd_from_server(struct hayward_view *view, bool enabled) {
    hayward_log(
        HAYWARD_DEBUG, "Telling view %p to set CSD to %i", (void *)view, enabled
    );
    if (view->xdg_decoration) {
        uint32_t mode = enabled
            ? WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
            : WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
        wlr_xdg_toplevel_decoration_v1_set_mode(
            view->xdg_decoration->wlr_xdg_decoration, mode
        );
    }
    view->using_csd = enabled;
}

void
view_update_csd_from_client(struct hayward_view *view, bool enabled) {
    hayward_log(
        HAYWARD_DEBUG, "View %p updated CSD to %i", (void *)view, enabled
    );
    struct hayward_window *window = view->window;
    if (enabled && window && window->pending.border != B_CSD) {
        window->saved_border = window->pending.border;
        if (window_is_floating(window)) {
            window->pending.border = B_CSD;
        }
    } else if (!enabled && window && window->pending.border == B_CSD) {
        window->pending.border = window->saved_border;
    }
    view->using_csd = enabled;
}

void
view_set_tiled(struct hayward_view *view, bool tiled) {
    if (view->impl->set_tiled) {
        view->impl->set_tiled(view, tiled);
    }
}

void
view_close(struct hayward_view *view) {
    if (view->impl->close) {
        view->impl->close(view);
    }
}

void
view_close_popups(struct hayward_view *view) {
    if (view->impl->close_popups) {
        view->impl->close_popups(view);
    }
}

static void
view_populate_pid(struct hayward_view *view) {
    pid_t pid;
    switch (view->type) {
#if HAVE_XWAYLAND
    case HAYWARD_VIEW_XWAYLAND:;
        struct wlr_xwayland_surface *surf =
            wlr_xwayland_surface_from_wlr_surface(view->surface);
        pid = surf->pid;
        break;
#endif
    case HAYWARD_VIEW_XDG_SHELL:;
        struct wl_client *client =
            wl_resource_get_client(view->surface->resource);
        wl_client_get_credentials(client, &pid, NULL, NULL);
        break;
    }
    view->pid = pid;
}

static bool
should_focus(struct hayward_view *view) {
    struct hayward_workspace *active_workspace =
        root_get_active_workspace(root);
    struct hayward_workspace *map_workspace = view->window->pending.workspace;
    struct hayward_output *map_output = view->window->pending.output;

    // Views cannot be focused if not mapped.
    if (map_workspace == NULL) {
        return false;
    }

    // Views can only take focus if they are mapped into the active workspace.
    if (map_workspace != active_workspace) {
        return false;
    }

    // View opened "under" fullscreen view should not be given focus.
    if (map_output != NULL && map_output->pending.fullscreen_window != NULL) {
        return false;
    }

    return true;
}

static void
handle_foreign_activate_request(struct wl_listener *listener, void *data) {
    struct hayward_view *view =
        wl_container_of(listener, view, foreign_activate_request);

    root_set_focused_window(root, view->window);
    window_raise_floating(view->window);

    transaction_flush();
}

static void
handle_foreign_fullscreen_request(struct wl_listener *listener, void *data) {
    struct hayward_view *view =
        wl_container_of(listener, view, foreign_fullscreen_request);
    struct wlr_foreign_toplevel_handle_v1_fullscreen_event *event = data;

    struct hayward_window *window = view->window;

    if (event->fullscreen && event->output && event->output->data) {
        struct hayward_output *output = event->output->data;
        hayward_move_window_to_output(window, output);
    }

    window_set_fullscreen(window, event->fullscreen);
    if (event->fullscreen) {
        arrange_root(root);
    } else {
        if (window->pending.parent) {
            arrange_column(window->pending.parent);
        } else if (window->pending.workspace) {
            arrange_workspace(window->pending.workspace);
        }
    }
    transaction_flush();
}

static void
handle_foreign_close_request(struct wl_listener *listener, void *data) {
    struct hayward_view *view =
        wl_container_of(listener, view, foreign_close_request);
    view_close(view);
}

static void
handle_foreign_destroy(struct wl_listener *listener, void *data) {
    struct hayward_view *view =
        wl_container_of(listener, view, foreign_destroy);

    wl_list_remove(&view->foreign_activate_request.link);
    wl_list_remove(&view->foreign_fullscreen_request.link);
    wl_list_remove(&view->foreign_close_request.link);
    wl_list_remove(&view->foreign_destroy.link);
}

void
view_map(
    struct hayward_view *view, struct wlr_surface *wlr_surface, bool fullscreen,
    struct wlr_output *fullscreen_output, bool decoration
) {
    hayward_assert(view->surface == NULL, "cannot map mapped view");
    view->surface = wlr_surface;
    view_populate_pid(view);
    view->window = window_create(view);

    // If there is a request to be opened fullscreen on a specific output, try
    // to honor that request. Otherwise, fallback to assigns, pid mappings,
    // focused workspace, etc
    struct hayward_workspace *workspace = root_get_active_workspace(root);
    hayward_assert(workspace != NULL, "Expected workspace");

    struct hayward_output *output = root_get_active_output(root);
    if (fullscreen_output && fullscreen_output->data) {
        output = fullscreen_output->data;
    }
    hayward_assert(output != NULL, "Expected output");

    view->foreign_toplevel =
        wlr_foreign_toplevel_handle_v1_create(server.foreign_toplevel_manager);
    view->foreign_activate_request.notify = handle_foreign_activate_request;
    wl_signal_add(
        &view->foreign_toplevel->events.request_activate,
        &view->foreign_activate_request
    );
    view->foreign_fullscreen_request.notify = handle_foreign_fullscreen_request;
    wl_signal_add(
        &view->foreign_toplevel->events.request_fullscreen,
        &view->foreign_fullscreen_request
    );
    view->foreign_close_request.notify = handle_foreign_close_request;
    wl_signal_add(
        &view->foreign_toplevel->events.request_close,
        &view->foreign_close_request
    );
    view->foreign_destroy.notify = handle_foreign_destroy;
    wl_signal_add(
        &view->foreign_toplevel->events.destroy, &view->foreign_destroy
    );

    const char *app_id;
    const char *class;
    if ((app_id = view_get_app_id(view)) != NULL) {
        wlr_foreign_toplevel_handle_v1_set_app_id(
            view->foreign_toplevel, app_id
        );
    } else if ((class = view_get_class(view)) != NULL) {
        wlr_foreign_toplevel_handle_v1_set_app_id(
            view->foreign_toplevel, class
        );
    }

    if (view->impl->wants_floating && view->impl->wants_floating(view)) {
        workspace_add_floating(workspace, view->window);

        view->window->pending.border = config->floating_border;
        view->window->pending.border_thickness =
            config->floating_border_thickness;
        hayward_move_window_to_floating(view->window);
    } else {
        struct hayward_window *target_sibling =
            workspace_get_active_tiling_window(workspace);
        if (target_sibling) {
            column_add_sibling(target_sibling, view->window, 1);
        } else {
            struct hayward_column *column = column_create();
            workspace_insert_tiling(workspace, output, column, 0);
            column_add_child(column, view->window);
        }

        view->window->pending.border = config->border;
        view->window->pending.border_thickness = config->border_thickness;
        view_set_tiled(view, true);

        if (target_sibling) {
            arrange_column(view->window->pending.parent);
        } else {
            arrange_workspace(workspace);
        }
    }

    if (config->popup_during_fullscreen == POPUP_LEAVE &&
        view->window->pending.output &&
        view->window->pending.output->pending.fullscreen_window &&
        view->window->pending.output->pending.fullscreen_window->view) {
        struct hayward_window *fs =
            view->window->pending.output->pending.fullscreen_window;
        if (view_is_transient_for(view, fs->view)) {
            window_set_fullscreen(fs, false);
        }
    }

    if (decoration) {
        view_update_csd_from_client(view, decoration);
    }

    if (fullscreen) {
        // Fullscreen windows still have to have a place as regular
        // tiling or floating windows, so this does not make the
        // previous logic unnecessary.
        window_set_fullscreen(view->window, true);
    }

    view_update_title(view, false);

    bool set_focus = should_focus(view);

#if HAVE_XWAYLAND
    if (wlr_surface_is_xwayland_surface(wlr_surface)) {
        struct wlr_xwayland_surface *xsurface =
            wlr_xwayland_surface_from_wlr_surface(wlr_surface);
        set_focus &= wlr_xwayland_icccm_input_model(xsurface) !=
            WLR_ICCCM_INPUT_MODEL_NONE;
    }
#endif

    if (set_focus) {
        root_set_focused_window(root, view->window);
    }

    ipc_event_window(view->window, "new");
}

void
view_unmap(struct hayward_view *view) {
    wl_signal_emit(&view->events.unmap, view);

    if (view->urgent_timer) {
        wl_event_source_remove(view->urgent_timer);
        view->urgent_timer = NULL;
    }

    if (view->foreign_toplevel) {
        wlr_foreign_toplevel_handle_v1_destroy(view->foreign_toplevel);
        view->foreign_toplevel = NULL;
    }

    struct hayward_column *parent = view->window->pending.parent;
    struct hayward_workspace *workspace = view->window->pending.workspace;
    window_begin_destroy(view->window);
    if (parent) {
        column_consider_destroy(parent);
    } else if (workspace) {
        workspace_consider_destroy(workspace);
    }

    if (workspace && !workspace->pending.dead) {
        arrange_workspace(workspace);
        workspace_detect_urgent(workspace);
    }

    struct hayward_seat *seat;
    wl_list_for_each(seat, &server.input->seats, link) {
        if (seat->cursor->active_constraint) {
            struct wlr_surface *constrain_surface =
                seat->cursor->active_constraint->surface;
            if (view_from_wlr_surface(constrain_surface) == view) {
                hayward_cursor_constrain(seat->cursor, NULL);
            }
        }
    }

    transaction_flush();
    view->surface = NULL;
}

void
view_update_size(struct hayward_view *view) {
    struct hayward_window *container = view->window;
    container->pending.content_width = view->geometry.width;
    container->pending.content_height = view->geometry.height;
    window_set_geometry_from_content(container);
}

void
view_center_surface(struct hayward_view *view) {
    struct hayward_window *window = view->window;

    // We always center the current coordinates rather than the next, as the
    // geometry immediately affects the currently active rendering.
    int x = (int
    )fmax(0, (window->committed.content_width - view->geometry.width) / 2);
    int y = (int
    )fmax(0, (window->committed.content_height - view->geometry.height) / 2);

    wlr_scene_node_set_position(&view->content_tree->node, x, y);
}

struct hayward_view *
view_from_wlr_surface(struct wlr_surface *wlr_surface) {
    if (wlr_surface_is_xdg_surface(wlr_surface)) {
        struct wlr_xdg_surface *xdg_surface =
            wlr_xdg_surface_from_wlr_surface(wlr_surface);
        if (xdg_surface == NULL) {
            return NULL;
        }
        return view_from_wlr_xdg_surface(xdg_surface);
    }
#if HAVE_XWAYLAND
    if (wlr_surface_is_xwayland_surface(wlr_surface)) {
        struct wlr_xwayland_surface *xsurface =
            wlr_xwayland_surface_from_wlr_surface(wlr_surface);
        if (xsurface == NULL) {
            return NULL;
        }
        return view_from_wlr_xwayland_surface(xsurface);
    }
#endif
    if (wlr_surface_is_subsurface(wlr_surface)) {
        struct wlr_subsurface *subsurface =
            wlr_subsurface_from_wlr_surface(wlr_surface);
        if (subsurface == NULL) {
            return NULL;
        }
        return view_from_wlr_surface(subsurface->parent);
    }
    if (wlr_surface_is_layer_surface(wlr_surface)) {
        return NULL;
    }

    const char *role = wlr_surface->role ? wlr_surface->role->name : NULL;
    hayward_log(
        HAYWARD_DEBUG, "Surface of unknown type (role %s): %p", role,
        (void *)wlr_surface
    );
    return NULL;
}

static char *
escape_pango_markup(const char *buffer) {
    size_t length = escape_markup_text(buffer, NULL);
    char *escaped_title = calloc(length + 1, sizeof(char));
    escape_markup_text(buffer, escaped_title);
    return escaped_title;
}

static size_t
append_prop(char *buffer, const char *value) {
    if (!value) {
        return 0;
    }
    // If using pango_markup in font, we need to escape all markup chars
    // from values to make sure tags are not inserted by clients
    if (config->pango_markup) {
        char *escaped_value = escape_pango_markup(value);
        lenient_strcat(buffer, escaped_value);
        size_t len = strlen(escaped_value);
        free(escaped_value);
        return len;
    } else {
        lenient_strcat(buffer, value);
        return strlen(value);
    }
}

/**
 * Calculate and return the length of the formatted title.
 * If buffer is not NULL, also populate the buffer with the formatted title.
 */
static size_t
parse_title_format(struct hayward_view *view, char *buffer) {
    if (!view->title_format || strcmp(view->title_format, "%title") == 0) {
        return append_prop(buffer, view_get_title(view));
    }

    size_t len = 0;
    char *format = view->title_format;
    char *next = strchr(format, '%');
    while (next) {
        // Copy everything up to the %
        lenient_strncat(buffer, format, next - format);
        len += next - format;
        format = next;

        if (strncmp(next, "%title", 6) == 0) {
            len += append_prop(buffer, view_get_title(view));
            format += 6;
        } else if (strncmp(next, "%app_id", 7) == 0) {
            len += append_prop(buffer, view_get_app_id(view));
            format += 7;
        } else if (strncmp(next, "%class", 6) == 0) {
            len += append_prop(buffer, view_get_class(view));
            format += 6;
        } else if (strncmp(next, "%instance", 9) == 0) {
            len += append_prop(buffer, view_get_instance(view));
            format += 9;
        } else if (strncmp(next, "%shell", 6) == 0) {
            len += append_prop(buffer, view_get_shell(view));
            format += 6;
        } else {
            lenient_strcat(buffer, "%");
            ++format;
            ++len;
        }
        next = strchr(format, '%');
    }
    lenient_strcat(buffer, format);
    len += strlen(format);

    return len;
}

void
view_update_title(struct hayward_view *view, bool force) {
    const char *title = view_get_title(view);

    if (!force) {
        if (title && view->window->title &&
            strcmp(title, view->window->title) == 0) {
            return;
        }
        if (!title && !view->window->title) {
            return;
        }
    }

    free(view->window->title);
    free(view->window->formatted_title);
    if (title) {
        size_t len = parse_title_format(view, NULL);
        char *buffer = calloc(len + 1, sizeof(char));
        hayward_assert(buffer, "Unable to allocate title string");
        parse_title_format(view, buffer);

        view->window->title = strdup(title);
        view->window->formatted_title = buffer;
    } else {
        view->window->title = NULL;
        view->window->formatted_title = NULL;
    }

    window_set_dirty(view->window);

    ipc_event_window(view->window, "title");

    if (view->foreign_toplevel && title) {
        wlr_foreign_toplevel_handle_v1_set_title(view->foreign_toplevel, title);
    }
}

bool
view_is_visible(struct hayward_view *view) {
    if (view->window->pending.dead) {
        return false;
    }
    struct hayward_workspace *workspace = view->window->pending.workspace;
    if (!workspace) {
        return false;
    }

    struct hayward_output *output = view->window->pending.output;
    if (!output) {
        return false;
    }

    // Check view isn't in a stacked container on an inactive tab
    struct hayward_window *window = view->window;
    struct hayward_column *column = window->pending.parent;
    if (column != NULL) {
        enum hayward_column_layout parent_layout = column->pending.layout;
        if (parent_layout == L_STACKED &&
            column->pending.active_child != window) {
            return false;
        }
    }

    // Check view isn't hidden by another fullscreen view
    struct hayward_window *fs = output->pending.fullscreen_window;
    if (fs && !window_is_fullscreen(view->window) &&
        !window_is_transient_for(view->window, fs)) {
        return false;
    }
    return true;
}

void
view_set_urgent(struct hayward_view *view, bool enable) {
    if (view_is_urgent(view) == enable) {
        return;
    }
    if (enable) {
        if (root_get_focused_window(root) == view->window) {
            return;
        }
        clock_gettime(CLOCK_MONOTONIC, &view->urgent);
    } else {
        view->urgent = (struct timespec){0};
        if (view->urgent_timer) {
            wl_event_source_remove(view->urgent_timer);
            view->urgent_timer = NULL;
        }
    }

    ipc_event_window(view->window, "urgent");

    workspace_detect_urgent(view->window->pending.workspace);
}

bool
view_is_urgent(struct hayward_view *view) {
    return view->urgent.tv_sec || view->urgent.tv_nsec;
}

void
view_remove_saved_buffer(struct hayward_view *view) {
    hayward_assert(view->saved_surface_tree != NULL, "Expected a saved buffer");
    wlr_scene_node_destroy(&view->saved_surface_tree->node);
    view->saved_surface_tree = NULL;
    wlr_scene_node_set_enabled(&view->content_tree->node, true);
}

static void
view_save_buffer_iterator(
    struct wlr_scene_buffer *buffer, int sx, int sy, void *data
) {
    struct wlr_scene_tree *tree = data;

    struct wlr_scene_buffer *sbuf = wlr_scene_buffer_create(tree, NULL);
    hayward_assert(sbuf != NULL, "Allocation failed");

    wlr_scene_buffer_set_dest_size(sbuf, buffer->dst_width, buffer->dst_height);
    wlr_scene_buffer_set_opaque_region(sbuf, &buffer->opaque_region);
    wlr_scene_buffer_set_source_box(sbuf, &buffer->src_box);
    wlr_scene_node_set_position(&sbuf->node, sx, sy);
    wlr_scene_buffer_set_transform(sbuf, buffer->transform);
    wlr_scene_buffer_set_buffer(sbuf, buffer->buffer);
}

void
view_save_buffer(struct hayward_view *view) {
    hayward_assert(
        view->saved_surface_tree == NULL, "Didn't expect saved buffer"
    );

    view->saved_surface_tree = wlr_scene_tree_create(view->scene_tree);
    hayward_assert(view->saved_surface_tree != NULL, "Allocation failed");

    // Enable and disable the saved surface tree like so to atomitaclly update
    // the tree. This will prevent over damaging or other weirdness.
    wlr_scene_node_set_enabled(&view->saved_surface_tree->node, false);

    wlr_scene_node_for_each_buffer(
        &view->content_tree->node, view_save_buffer_iterator,
        view->saved_surface_tree
    );

    wlr_scene_node_set_enabled(&view->content_tree->node, false);
    wlr_scene_node_set_enabled(&view->saved_surface_tree->node, true);
}

bool
view_is_transient_for(
    struct hayward_view *child, struct hayward_view *ancestor
) {
    return child->impl->is_transient_for &&
        child->impl->is_transient_for(child, ancestor);
}
