#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include "hayward/input/seat.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_idle.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_keyboard_shortcuts_inhibit_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_tablet_tool.h>
#include <wlr/types/wlr_touch.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/box.h>
#include <wlr/xcursor.h>
#include <wlr/xwayland/xwayland.h>

#include <hayward-common/list.h>
#include <hayward-common/log.h>

#include <wayland-server-protocol.h>

#include <hayward/config.h>
#include <hayward/desktop/layer_shell.h>
#include <hayward/desktop/xwayland.h>
#include <hayward/globals/root.h>
#include <hayward/globals/transaction.h>
#include <hayward/input/cursor.h>
#include <hayward/input/input-manager.h>
#include <hayward/input/keyboard.h>
#include <hayward/input/libinput.h>
#include <hayward/input/seatop_default.h>
#include <hayward/input/switch.h>
#include <hayward/input/tablet.h>
#include <hayward/input/text_input.h>
#include <hayward/output.h>
#include <hayward/server.h>
#include <hayward/transaction.h>
#include <hayward/tree/root.h>
#include <hayward/tree/view.h>
#include <hayward/tree/window.h>

#include <config.h>

static void
seat_send_focus(struct hayward_seat *seat, struct wlr_surface *surface);
static void
seat_commit_focus(struct hayward_seat *seat);

static void
handle_request_start_drag(struct wl_listener *listener, void *data);
static void
handle_start_drag(struct wl_listener *listener, void *data);
static void
handle_request_set_selection(struct wl_listener *listener, void *data);
static void
handle_request_set_primary_selection(struct wl_listener *listener, void *data);
static void
handle_transaction_before_commit(struct wl_listener *listener, void *data);

struct hayward_seat *
seat_create(const char *seat_name) {
    struct hayward_seat *seat = calloc(1, sizeof(struct hayward_seat));
    if (!seat) {
        return NULL;
    }

    seat->wlr_seat = wlr_seat_create(server.wl_display, seat_name);
    hayward_assert(seat->wlr_seat, "could not allocate seat");
    seat->wlr_seat->data = seat;

    seat->cursor = hayward_cursor_create(seat);
    if (!seat->cursor) {
        wlr_seat_destroy(seat->wlr_seat);
        free(seat);
        return NULL;
    }

    seat->idle_inhibit_sources = seat->idle_wake_sources =
        IDLE_SOURCE_KEYBOARD | IDLE_SOURCE_POINTER | IDLE_SOURCE_TOUCH |
        IDLE_SOURCE_TABLET_PAD | IDLE_SOURCE_TABLET_TOOL | IDLE_SOURCE_SWITCH;

    wl_list_init(&seat->devices);

    seat->deferred_bindings = create_list();

    wl_signal_add(
        &seat->wlr_seat->events.request_start_drag, &seat->request_start_drag
    );
    seat->request_start_drag.notify = handle_request_start_drag;

    wl_signal_add(&seat->wlr_seat->events.start_drag, &seat->start_drag);
    seat->start_drag.notify = handle_start_drag;

    wl_signal_add(
        &seat->wlr_seat->events.request_set_selection,
        &seat->request_set_selection
    );
    seat->request_set_selection.notify = handle_request_set_selection;

    wl_signal_add(
        &seat->wlr_seat->events.request_set_primary_selection,
        &seat->request_set_primary_selection
    );
    seat->request_set_primary_selection.notify =
        handle_request_set_primary_selection;

    wl_list_init(&seat->keyboard_groups);
    wl_list_init(&seat->keyboard_shortcuts_inhibitors);

    hayward_input_method_relay_init(seat, &seat->im_relay);

    wl_list_insert(&server.input->seats, &seat->link);

    seat->transaction_before_commit.notify = handle_transaction_before_commit;
    wl_signal_add(
        &transaction_manager->events.before_commit,
        &seat->transaction_before_commit
    );

    seatop_begin_default(seat);

    return seat;
}

static void
seat_device_destroy(struct hayward_seat_device *seat_device) {
    if (!seat_device) {
        return;
    }

    hayward_keyboard_destroy(seat_device->keyboard);
    hayward_tablet_destroy(seat_device->tablet);
    hayward_tablet_pad_destroy(seat_device->tablet_pad);
    hayward_switch_destroy(seat_device->switch_device);
    wlr_cursor_detach_input_device(
        seat_device->hayward_seat->cursor->cursor,
        seat_device->input_device->wlr_device
    );
    wl_list_remove(&seat_device->link);
    free(seat_device);
}

void
seat_destroy(struct hayward_seat *seat) {
    if (seat == config->handler_context.seat) {
        config->handler_context.seat = input_manager_get_default_seat();
    }
    struct hayward_seat_device *seat_device, *next;
    wl_list_for_each_safe(seat_device, next, &seat->devices, link) {
        seat_device_destroy(seat_device);
    }

    hayward_input_method_relay_finish(&seat->im_relay);
    hayward_cursor_destroy(seat->cursor);
    wl_list_remove(&seat->request_start_drag.link);
    wl_list_remove(&seat->start_drag.link);
    wl_list_remove(&seat->request_set_selection.link);
    wl_list_remove(&seat->request_set_primary_selection.link);
    wl_list_remove(&seat->transaction_before_commit.link);
    wl_list_remove(&seat->link);
    wlr_seat_destroy(seat->wlr_seat);
    for (int i = 0; i < seat->deferred_bindings->length; i++) {
        free_hayward_binding(seat->deferred_bindings->items[i]);
    }
    list_free(seat->deferred_bindings);
    free(seat);
}

void
seat_idle_notify_activity(
    struct hayward_seat *seat, enum hayward_input_idle_source source
) {
    uint32_t mask = seat->idle_inhibit_sources;
    struct wlr_idle_timeout *timeout;
    int ntimers = 0, nidle = 0;
    wl_list_for_each(timeout, &server.idle->idle_timers, link) {
        ++ntimers;
        if (timeout->idle_state) {
            ++nidle;
        }
    }
    if (nidle == ntimers) {
        mask = seat->idle_wake_sources;
    }
    if ((source & mask) > 0) {
        wlr_idle_notify_activity(server.idle, seat->wlr_seat);
    }
}

static struct hayward_keyboard *
hayward_keyboard_for_wlr_keyboard(
    struct hayward_seat *seat, struct wlr_keyboard *wlr_keyboard
) {
    struct hayward_seat_device *seat_device;
    wl_list_for_each(seat_device, &seat->devices, link) {
        struct hayward_input_device *input_device = seat_device->input_device;
        if (input_device->wlr_device->type != WLR_INPUT_DEVICE_KEYBOARD) {
            continue;
        }
        if (wlr_keyboard_from_input_device(input_device->wlr_device) ==
            wlr_keyboard) {
            return seat_device->keyboard;
        }
    }
    struct hayward_keyboard_group *group;
    wl_list_for_each(group, &seat->keyboard_groups, link) {
        struct hayward_input_device *input_device =
            group->seat_device->input_device;
        if (wlr_keyboard_from_input_device(input_device->wlr_device) ==
            wlr_keyboard) {
            return group->seat_device->keyboard;
        }
    }
    return NULL;
}

static void
seat_keyboard_notify_enter(
    struct hayward_seat *seat, struct wlr_surface *surface
) {
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat->wlr_seat);
    if (!keyboard) {
        wlr_seat_keyboard_notify_enter(seat->wlr_seat, surface, NULL, 0, NULL);
        return;
    }

    struct hayward_keyboard *hayward_keyboard =
        hayward_keyboard_for_wlr_keyboard(seat, keyboard);
    assert(
        hayward_keyboard && "Cannot find hayward_keyboard for seat keyboard"
    );

    struct hayward_shortcut_state *state =
        &hayward_keyboard->state_pressed_sent;
    wlr_seat_keyboard_notify_enter(
        seat->wlr_seat, surface, state->pressed_keycodes, state->npressed,
        &keyboard->modifiers
    );
}

static void
seat_tablet_pads_notify_enter(
    struct hayward_seat *seat, struct wlr_surface *surface
) {
    struct hayward_seat_device *seat_device;
    wl_list_for_each(seat_device, &seat->devices, link) {
        hayward_tablet_pad_notify_enter(seat_device->tablet_pad, surface);
    }
}

bool
seat_is_input_allowed(struct hayward_seat *seat, struct wlr_surface *surface) {
    struct wl_client *client = wl_resource_get_client(surface->resource);
    return seat->exclusive_client == client ||
        (seat->exclusive_client == NULL && !server.session_lock.locked);
}

void
drag_icon_update_position(struct hayward_drag_icon *icon) {
    struct wlr_drag_icon *wlr_icon = icon->wlr_drag_icon;
    struct hayward_seat *seat = icon->seat;
    struct wlr_cursor *cursor = seat->cursor->cursor;
    switch (wlr_icon->drag->grab_type) {
    case WLR_DRAG_GRAB_KEYBOARD:
        return;
    case WLR_DRAG_GRAB_KEYBOARD_POINTER:
        icon->x = cursor->x + icon->dx;
        icon->y = cursor->y + icon->dy;
        break;
    case WLR_DRAG_GRAB_KEYBOARD_TOUCH:;
        struct wlr_touch_point *point =
            wlr_seat_touch_get_point(seat->wlr_seat, wlr_icon->drag->touch_id);
        if (point == NULL) {
            return;
        }
        icon->x = seat->touch_x + icon->dx;
        icon->y = seat->touch_y + icon->dy;
    }
}

static void
drag_icon_handle_surface_commit(struct wl_listener *listener, void *data) {
    struct hayward_drag_icon *icon =
        wl_container_of(listener, icon, surface_commit);

    hayward_transaction_manager_begin_transaction(transaction_manager);

    struct wlr_drag_icon *wlr_icon = icon->wlr_drag_icon;
    icon->dx += wlr_icon->surface->current.dx;
    icon->dy += wlr_icon->surface->current.dy;
    drag_icon_update_position(icon);

    hayward_transaction_manager_end_transaction(transaction_manager);
}

static void
drag_icon_handle_map(struct wl_listener *listener, void *data) {
    struct hayward_drag_icon *icon = wl_container_of(listener, icon, map);
}

static void
drag_icon_handle_unmap(struct wl_listener *listener, void *data) {
    struct hayward_drag_icon *icon = wl_container_of(listener, icon, unmap);
}

static void
drag_icon_handle_destroy(struct wl_listener *listener, void *data) {
    struct hayward_drag_icon *icon = wl_container_of(listener, icon, destroy);
    icon->wlr_drag_icon->data = NULL;
    wl_list_remove(&icon->link);
    wl_list_remove(&icon->surface_commit.link);
    wl_list_remove(&icon->unmap.link);
    wl_list_remove(&icon->map.link);
    wl_list_remove(&icon->destroy.link);
    free(icon);
}

static void
drag_handle_destroy(struct wl_listener *listener, void *data) {
    struct hayward_drag *drag = wl_container_of(listener, drag, destroy);

    hayward_transaction_manager_begin_transaction(transaction_manager);

    // Focus enter isn't sent during drag, so refocus the focused node, layer
    // surface or unmanaged surface.
    struct hayward_seat *seat = drag->seat;
    if (seat->focused_surface) {
        seat_send_focus(seat, seat->focused_surface);
    }

    drag->wlr_drag->data = NULL;
    wl_list_remove(&drag->destroy.link);
    free(drag);

    hayward_transaction_manager_end_transaction(transaction_manager);
}

static void
handle_request_start_drag(struct wl_listener *listener, void *data) {
    struct hayward_seat *seat =
        wl_container_of(listener, seat, request_start_drag);
    struct wlr_seat_request_start_drag_event *event = data;

    hayward_transaction_manager_begin_transaction(transaction_manager);

    if (wlr_seat_validate_pointer_grab_serial(
            seat->wlr_seat, event->origin, event->serial
        )) {
        wlr_seat_start_pointer_drag(seat->wlr_seat, event->drag, event->serial);

        hayward_transaction_manager_end_transaction(transaction_manager);
        return;
    }

    struct wlr_touch_point *point;
    if (wlr_seat_validate_touch_grab_serial(
            seat->wlr_seat, event->origin, event->serial, &point
        )) {
        wlr_seat_start_touch_drag(
            seat->wlr_seat, event->drag, event->serial, point
        );

        hayward_transaction_manager_end_transaction(transaction_manager);
        return;
    }

    // TODO: tablet grabs

    hayward_log(
        HAYWARD_DEBUG,
        "Ignoring start_drag request: "
        "could not validate pointer or touch serial %" PRIu32,
        event->serial
    );
    wlr_data_source_destroy(event->drag->source);

    hayward_transaction_manager_end_transaction(transaction_manager);
}

static void
handle_start_drag(struct wl_listener *listener, void *data) {
    struct hayward_seat *seat = wl_container_of(listener, seat, start_drag);
    struct wlr_drag *wlr_drag = data;

    hayward_transaction_manager_begin_transaction(transaction_manager);

    struct hayward_drag *drag = calloc(1, sizeof(struct hayward_drag));
    if (drag == NULL) {
        hayward_log(HAYWARD_ERROR, "Allocation failed");

        hayward_transaction_manager_end_transaction(transaction_manager);
        return;
    }
    drag->seat = seat;
    drag->wlr_drag = wlr_drag;
    wlr_drag->data = drag;

    drag->destroy.notify = drag_handle_destroy;
    wl_signal_add(&wlr_drag->events.destroy, &drag->destroy);

    struct wlr_drag_icon *wlr_drag_icon = wlr_drag->icon;
    if (wlr_drag_icon != NULL) {
        struct hayward_drag_icon *icon =
            calloc(1, sizeof(struct hayward_drag_icon));
        if (icon == NULL) {
            hayward_log(HAYWARD_ERROR, "Allocation failed");

            hayward_transaction_manager_end_transaction(transaction_manager);
            return;
        }
        icon->seat = seat;
        icon->wlr_drag_icon = wlr_drag_icon;
        wlr_drag_icon->data = icon;

        icon->surface_commit.notify = drag_icon_handle_surface_commit;
        wl_signal_add(
            &wlr_drag_icon->surface->events.commit, &icon->surface_commit
        );
        icon->unmap.notify = drag_icon_handle_unmap;
        wl_signal_add(&wlr_drag_icon->events.unmap, &icon->unmap);
        icon->map.notify = drag_icon_handle_map;
        wl_signal_add(&wlr_drag_icon->events.map, &icon->map);
        icon->destroy.notify = drag_icon_handle_destroy;
        wl_signal_add(&wlr_drag_icon->events.destroy, &icon->destroy);

        wl_list_insert(&root->drag_icons, &icon->link);

        drag_icon_update_position(icon);
    }
    seatop_begin_default(seat);

    hayward_transaction_manager_end_transaction(transaction_manager);
}

static void
handle_request_set_selection(struct wl_listener *listener, void *data) {
    struct hayward_seat *seat =
        wl_container_of(listener, seat, request_set_selection);
    struct wlr_seat_request_set_selection_event *event = data;

    hayward_transaction_manager_begin_transaction(transaction_manager);

    wlr_seat_set_selection(seat->wlr_seat, event->source, event->serial);

    hayward_transaction_manager_end_transaction(transaction_manager);
}

static void
handle_request_set_primary_selection(struct wl_listener *listener, void *data) {
    struct hayward_seat *seat =
        wl_container_of(listener, seat, request_set_primary_selection);
    struct wlr_seat_request_set_primary_selection_event *event = data;

    hayward_transaction_manager_begin_transaction(transaction_manager);

    wlr_seat_set_primary_selection(
        seat->wlr_seat, event->source, event->serial
    );

    hayward_transaction_manager_end_transaction(transaction_manager);
}

static void
handle_transaction_before_commit(struct wl_listener *listener, void *data) {
    struct hayward_seat *seat =
        wl_container_of(listener, seat, transaction_before_commit);

    seat_commit_focus(seat);
}

static void
seat_update_capabilities(struct hayward_seat *seat) {
    uint32_t caps = 0;
    uint32_t previous_caps = seat->wlr_seat->capabilities;
    struct hayward_seat_device *seat_device;
    wl_list_for_each(seat_device, &seat->devices, link) {
        switch (seat_device->input_device->wlr_device->type) {
        case WLR_INPUT_DEVICE_KEYBOARD:
            caps |= WL_SEAT_CAPABILITY_KEYBOARD;
            break;
        case WLR_INPUT_DEVICE_POINTER:
            caps |= WL_SEAT_CAPABILITY_POINTER;
            break;
        case WLR_INPUT_DEVICE_TOUCH:
            caps |= WL_SEAT_CAPABILITY_TOUCH;
            break;
        case WLR_INPUT_DEVICE_TABLET_TOOL:
            caps |= WL_SEAT_CAPABILITY_POINTER;
            break;
        case WLR_INPUT_DEVICE_SWITCH:
        case WLR_INPUT_DEVICE_TABLET_PAD:
            break;
        }
    }

    // Hide cursor if seat doesn't have pointer capability.
    // We must call cursor_set_image while the wlr_seat has the capabilities
    // otherwise it's a no op.
    if ((caps & WL_SEAT_CAPABILITY_POINTER) == 0) {
        cursor_set_image(seat->cursor, NULL, NULL);
        wlr_seat_set_capabilities(seat->wlr_seat, caps);
    } else {
        wlr_seat_set_capabilities(seat->wlr_seat, caps);
        if ((previous_caps & WL_SEAT_CAPABILITY_POINTER) == 0) {
            cursor_set_image(seat->cursor, "left_ptr", NULL);
        }
    }
}

static void
seat_reset_input_config(
    struct hayward_seat *seat, struct hayward_seat_device *hayward_device
) {
    hayward_log(
        HAYWARD_DEBUG, "Resetting output mapping for input device %s",
        hayward_device->input_device->identifier
    );
    wlr_cursor_map_input_to_output(
        seat->cursor->cursor, hayward_device->input_device->wlr_device, NULL
    );
}

static bool
has_prefix(const char *str, const char *prefix) {
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

/**
 * Get the name of the built-in output, if any. Returns NULL if there isn't
 * exactly one built-in output.
 */
static const char *
get_builtin_output_name(void) {
    const char *match = NULL;
    for (int i = 0; i < root->outputs->length; ++i) {
        struct hayward_output *output = root->outputs->items[i];
        const char *name = output->wlr_output->name;
        if (has_prefix(name, "eDP-") || has_prefix(name, "LVDS-") ||
            has_prefix(name, "DSI-")) {
            if (match != NULL) {
                return NULL;
            }
            match = name;
        }
    }
    return match;
}

static bool
is_touch_or_tablet_tool(struct hayward_seat_device *seat_device) {
    switch (seat_device->input_device->wlr_device->type) {
    case WLR_INPUT_DEVICE_TOUCH:
    case WLR_INPUT_DEVICE_TABLET_TOOL:
        return true;
    default:
        return false;
    }
}

static void
seat_apply_input_config(
    struct hayward_seat *seat, struct hayward_seat_device *hayward_device
) {
    struct input_config *ic =
        input_device_get_config(hayward_device->input_device);

    hayward_log(
        HAYWARD_DEBUG, "Applying input config to %s",
        hayward_device->input_device->identifier
    );

    const char *mapped_to_output = ic == NULL ? NULL : ic->mapped_to_output;
    struct wlr_box *mapped_to_region = ic == NULL ? NULL : ic->mapped_to_region;
    enum input_config_mapped_to mapped_to =
        ic == NULL ? MAPPED_TO_DEFAULT : ic->mapped_to;

    switch (mapped_to) {
    case MAPPED_TO_DEFAULT:;
        /*
         * If the wlroots backend provides an output name, use that.
         *
         * Otherwise, try to map built-in touch and pointer devices to the
         * built-in output.
         */
        struct wlr_input_device *dev = hayward_device->input_device->wlr_device;
        switch (dev->type) {
        case WLR_INPUT_DEVICE_POINTER:
            mapped_to_output = wlr_pointer_from_input_device(dev)->output_name;
            break;
        case WLR_INPUT_DEVICE_TOUCH:
            mapped_to_output = wlr_touch_from_input_device(dev)->output_name;
            break;
        default:
            mapped_to_output = NULL;
            break;
        }
        if (mapped_to_output == NULL &&
            is_touch_or_tablet_tool(hayward_device) &&
            hayward_libinput_device_is_builtin(hayward_device->input_device)) {
            mapped_to_output = get_builtin_output_name();
            if (mapped_to_output) {
                hayward_log(
                    HAYWARD_DEBUG, "Auto-detected output '%s' for device '%s'",
                    mapped_to_output, hayward_device->input_device->identifier
                );
            }
        }
        if (mapped_to_output == NULL) {
            return;
        }
        /* fallthrough */
    case MAPPED_TO_OUTPUT:
        hayward_log(
            HAYWARD_DEBUG, "Mapping input device %s to output %s",
            hayward_device->input_device->identifier, mapped_to_output
        );
        if (strcmp("*", mapped_to_output) == 0) {
            wlr_cursor_map_input_to_output(
                seat->cursor->cursor, hayward_device->input_device->wlr_device,
                NULL
            );
            wlr_cursor_map_input_to_region(
                seat->cursor->cursor, hayward_device->input_device->wlr_device,
                NULL
            );
            hayward_log(HAYWARD_DEBUG, "Reset output mapping");
            return;
        }
        struct hayward_output *output = output_by_name_or_id(mapped_to_output);
        if (!output) {
            hayward_log(
                HAYWARD_DEBUG,
                "Requested output %s for device %s isn't present",
                mapped_to_output, hayward_device->input_device->identifier
            );
            return;
        }
        wlr_cursor_map_input_to_output(
            seat->cursor->cursor, hayward_device->input_device->wlr_device,
            output->wlr_output
        );
        wlr_cursor_map_input_to_region(
            seat->cursor->cursor, hayward_device->input_device->wlr_device, NULL
        );
        hayward_log(
            HAYWARD_DEBUG, "Mapped to output %s", output->wlr_output->name
        );
        return;
    case MAPPED_TO_REGION:
        hayward_log(
            HAYWARD_DEBUG, "Mapping input device %s to %d,%d %dx%d",
            hayward_device->input_device->identifier, mapped_to_region->x,
            mapped_to_region->y, mapped_to_region->width,
            mapped_to_region->height
        );
        wlr_cursor_map_input_to_output(
            seat->cursor->cursor, hayward_device->input_device->wlr_device, NULL
        );
        wlr_cursor_map_input_to_region(
            seat->cursor->cursor, hayward_device->input_device->wlr_device,
            mapped_to_region
        );
        return;
    }
}

static void
seat_configure_pointer(
    struct hayward_seat *seat, struct hayward_seat_device *hayward_device
) {
    if ((seat->wlr_seat->capabilities & WL_SEAT_CAPABILITY_POINTER) == 0) {
        seat_configure_xcursor(seat);
    }
    wlr_cursor_attach_input_device(
        seat->cursor->cursor, hayward_device->input_device->wlr_device
    );
    seat_apply_input_config(seat, hayward_device);
    wl_event_source_timer_update(
        seat->cursor->hide_source, cursor_get_timeout(seat->cursor)
    );
}

static void
seat_configure_keyboard(
    struct hayward_seat *seat, struct hayward_seat_device *seat_device
) {
    if (!seat_device->keyboard) {
        hayward_keyboard_create(seat, seat_device);
    }
    hayward_keyboard_configure(seat_device->keyboard);
    wlr_seat_set_keyboard(
        seat->wlr_seat,
        wlr_keyboard_from_input_device(seat_device->input_device->wlr_device)
    );

    // force notify reenter to pick up the new configuration.  This reuses
    // the current focused surface to avoid breaking input grabs.
    struct wlr_surface *surface =
        seat->wlr_seat->keyboard_state.focused_surface;
    if (surface) {
        wlr_seat_keyboard_notify_clear_focus(seat->wlr_seat);
        seat_keyboard_notify_enter(seat, surface);
    }
}

static void
seat_configure_switch(
    struct hayward_seat *seat, struct hayward_seat_device *seat_device
) {
    if (!seat_device->switch_device) {
        hayward_switch_create(seat, seat_device);
    }
    seat_apply_input_config(seat, seat_device);
    hayward_switch_configure(seat_device->switch_device);
}

static void
seat_configure_touch(
    struct hayward_seat *seat, struct hayward_seat_device *hayward_device
) {
    wlr_cursor_attach_input_device(
        seat->cursor->cursor, hayward_device->input_device->wlr_device
    );
    seat_apply_input_config(seat, hayward_device);
}

static void
seat_configure_tablet_tool(
    struct hayward_seat *seat, struct hayward_seat_device *hayward_device
) {
    if (!hayward_device->tablet) {
        hayward_device->tablet = hayward_tablet_create(seat, hayward_device);
    }
    hayward_configure_tablet(hayward_device->tablet);
    wlr_cursor_attach_input_device(
        seat->cursor->cursor, hayward_device->input_device->wlr_device
    );
    seat_apply_input_config(seat, hayward_device);
}

static void
seat_configure_tablet_pad(
    struct hayward_seat *seat, struct hayward_seat_device *hayward_device
) {
    if (!hayward_device->tablet_pad) {
        hayward_device->tablet_pad =
            hayward_tablet_pad_create(seat, hayward_device);
    }
    hayward_configure_tablet_pad(hayward_device->tablet_pad);
}

static struct hayward_seat_device *
seat_get_device(
    struct hayward_seat *seat, struct hayward_input_device *input_device
) {
    struct hayward_seat_device *seat_device = NULL;
    wl_list_for_each(seat_device, &seat->devices, link) {
        if (seat_device->input_device == input_device) {
            return seat_device;
        }
    }

    struct hayward_keyboard_group *group = NULL;
    wl_list_for_each(group, &seat->keyboard_groups, link) {
        if (group->seat_device->input_device == input_device) {
            return group->seat_device;
        }
    }

    return NULL;
}

void
seat_configure_device(
    struct hayward_seat *seat, struct hayward_input_device *input_device
) {
    struct hayward_seat_device *seat_device =
        seat_get_device(seat, input_device);
    if (!seat_device) {
        return;
    }

    switch (input_device->wlr_device->type) {
    case WLR_INPUT_DEVICE_POINTER:
        seat_configure_pointer(seat, seat_device);
        break;
    case WLR_INPUT_DEVICE_KEYBOARD:
        seat_configure_keyboard(seat, seat_device);
        break;
    case WLR_INPUT_DEVICE_SWITCH:
        seat_configure_switch(seat, seat_device);
        break;
    case WLR_INPUT_DEVICE_TOUCH:
        seat_configure_touch(seat, seat_device);
        break;
    case WLR_INPUT_DEVICE_TABLET_TOOL:
        seat_configure_tablet_tool(seat, seat_device);
        break;
    case WLR_INPUT_DEVICE_TABLET_PAD:
        seat_configure_tablet_pad(seat, seat_device);
        break;
    }
}

void
seat_reset_device(
    struct hayward_seat *seat, struct hayward_input_device *input_device
) {
    struct hayward_seat_device *seat_device =
        seat_get_device(seat, input_device);
    if (!seat_device) {
        return;
    }

    switch (input_device->wlr_device->type) {
    case WLR_INPUT_DEVICE_POINTER:
        seat_reset_input_config(seat, seat_device);
        break;
    case WLR_INPUT_DEVICE_KEYBOARD:
        hayward_keyboard_disarm_key_repeat(seat_device->keyboard);
        hayward_keyboard_configure(seat_device->keyboard);
        break;
    case WLR_INPUT_DEVICE_TOUCH:
        seat_reset_input_config(seat, seat_device);
        break;
    case WLR_INPUT_DEVICE_TABLET_TOOL:
        seat_reset_input_config(seat, seat_device);
        break;
    case WLR_INPUT_DEVICE_TABLET_PAD:
        hayward_log(HAYWARD_DEBUG, "TODO: reset tablet pad");
        break;
    case WLR_INPUT_DEVICE_SWITCH:
        hayward_log(HAYWARD_DEBUG, "TODO: reset switch device");
        break;
    }
}

void
seat_add_device(
    struct hayward_seat *seat, struct hayward_input_device *input_device
) {
    if (seat_get_device(seat, input_device)) {
        seat_configure_device(seat, input_device);
        return;
    }

    struct hayward_seat_device *seat_device =
        calloc(1, sizeof(struct hayward_seat_device));
    if (!seat_device) {
        hayward_log(HAYWARD_DEBUG, "could not allocate seat device");
        return;
    }

    hayward_log(
        HAYWARD_DEBUG, "adding device %s to seat %s", input_device->identifier,
        seat->wlr_seat->name
    );

    seat_device->hayward_seat = seat;
    seat_device->input_device = input_device;
    wl_list_insert(&seat->devices, &seat_device->link);

    seat_configure_device(seat, input_device);

    seat_update_capabilities(seat);
}

void
seat_remove_device(
    struct hayward_seat *seat, struct hayward_input_device *input_device
) {
    struct hayward_seat_device *seat_device =
        seat_get_device(seat, input_device);

    if (!seat_device) {
        return;
    }

    hayward_log(
        HAYWARD_DEBUG, "removing device %s from seat %s",
        input_device->identifier, seat->wlr_seat->name
    );

    seat_device_destroy(seat_device);

    seat_update_capabilities(seat);
}

static bool
xcursor_manager_is_named(
    const struct wlr_xcursor_manager *manager, const char *name
) {
    return (!manager->name && !name) ||
        (name && manager->name && strcmp(name, manager->name) == 0);
}

void
seat_configure_xcursor(struct hayward_seat *seat) {
    unsigned cursor_size = 24;
    const char *cursor_theme = NULL;

    const struct seat_config *seat_config = seat_get_config(seat);
    if (!seat_config) {
        seat_config = seat_get_config_by_name("*");
    }
    if (seat_config) {
        cursor_size = seat_config->xcursor_theme.size;
        cursor_theme = seat_config->xcursor_theme.name;
    }

    if (seat == input_manager_get_default_seat()) {
        char cursor_size_fmt[16];
        snprintf(cursor_size_fmt, sizeof(cursor_size_fmt), "%u", cursor_size);
        setenv("XCURSOR_SIZE", cursor_size_fmt, 1);
        if (cursor_theme != NULL) {
            setenv("XCURSOR_THEME", cursor_theme, 1);
        }

#if HAVE_XWAYLAND
        if (server.xwayland != NULL &&
            (!server.xwayland->xcursor_manager ||
             !xcursor_manager_is_named(
                 server.xwayland->xcursor_manager, cursor_theme
             ) ||
             server.xwayland->xcursor_manager->size != cursor_size)) {

            wlr_xcursor_manager_destroy(server.xwayland->xcursor_manager);

            server.xwayland->xcursor_manager =
                wlr_xcursor_manager_create(cursor_theme, cursor_size);
            hayward_assert(
                server.xwayland->xcursor_manager,
                "Cannot create XCursor manager for theme"
            );

            wlr_xcursor_manager_load(server.xwayland->xcursor_manager, 1);
            struct wlr_xcursor *xcursor = wlr_xcursor_manager_get_xcursor(
                server.xwayland->xcursor_manager, "left_ptr", 1
            );
            if (xcursor != NULL) {
                struct wlr_xcursor_image *image = xcursor->images[0];
                wlr_xwayland_set_cursor(
                    server.xwayland->xwayland, image->buffer, image->width * 4,
                    image->width, image->height, image->hotspot_x,
                    image->hotspot_y
                );
            }
        }
#endif
    }

    /* Create xcursor manager if we don't have one already, or if the
     * theme has changed */
    if (!seat->cursor->xcursor_manager ||
        !xcursor_manager_is_named(
            seat->cursor->xcursor_manager, cursor_theme
        ) ||
        seat->cursor->xcursor_manager->size != cursor_size) {

        wlr_xcursor_manager_destroy(seat->cursor->xcursor_manager);
        seat->cursor->xcursor_manager =
            wlr_xcursor_manager_create(cursor_theme, cursor_size);
        if (!seat->cursor->xcursor_manager) {
            hayward_log(
                HAYWARD_ERROR, "Cannot create XCursor manager for theme '%s'",
                cursor_theme
            );
        }
    }

    for (int i = 0; i < root->outputs->length; ++i) {
        struct hayward_output *hayward_output = root->outputs->items[i];
        struct wlr_output *output = hayward_output->wlr_output;
        bool result = wlr_xcursor_manager_load(
            seat->cursor->xcursor_manager, output->scale
        );
        if (!result) {
            hayward_log(
                HAYWARD_ERROR,
                "Cannot load xcursor theme for output '%s' with scale %f",
                output->name, output->scale
            );
        }
    }

    // Reset the cursor so that we apply it to outputs that just appeared
    cursor_set_image(seat->cursor, NULL, NULL);
    cursor_set_image(seat->cursor, "left_ptr", NULL);
    wlr_cursor_warp(
        seat->cursor->cursor, NULL, seat->cursor->cursor->x,
        seat->cursor->cursor->y
    );
}

/**
 * If container is a view, set it as active and enable keyboard input.
 * If container is a container, set all child views as active and don't enable
 * keyboard input on any.
 */
static void
seat_send_focus(struct hayward_seat *seat, struct wlr_surface *surface) {
    if (!seat_is_input_allowed(seat, surface)) {
        hayward_log(HAYWARD_DEBUG, "Refusing to set focus, input is inhibited");
        return;
    }

    seat_keyboard_notify_enter(seat, surface);
    seat_tablet_pads_notify_enter(seat, surface);
    hayward_input_method_relay_set_focus(&seat->im_relay, surface);

    struct wlr_pointer_constraint_v1 *constraint =
        wlr_pointer_constraints_v1_constraint_for_surface(
            server.pointer_constraints, surface, seat->wlr_seat
        );
    hayward_cursor_constrain(seat->cursor, constraint);
}

static void
seat_send_unfocus(struct hayward_seat *seat, struct wlr_surface *surface) {
    hayward_cursor_constrain(seat->cursor, NULL);
    wlr_seat_keyboard_notify_clear_focus(seat->wlr_seat);
}

static void
seat_commit_focus(struct hayward_seat *seat) {
    hayward_assert(seat != NULL, "Expected seat");

    struct wlr_surface *old_surface = seat->focused_surface;
    struct wlr_surface *new_surface = root_get_focused_surface(root);

    if (old_surface == new_surface) {
        return;
    }

    if (old_surface && new_surface != old_surface) {
        seat_send_unfocus(seat, old_surface);
    }

    if (new_surface && new_surface != old_surface) {
        seat_send_focus(seat, new_surface);
    }

    seat->focused_surface = new_surface;
}

void
hayward_force_focus(struct wlr_surface *surface) {
    struct hayward_seat *seat;
    wl_list_for_each(seat, &server.input->seats, link) {
        seat_keyboard_notify_enter(seat, surface);
        seat_tablet_pads_notify_enter(seat, surface);
        hayward_input_method_relay_set_focus(&seat->im_relay, surface);
    }
}

void
seat_set_exclusive_client(struct hayward_seat *seat, struct wl_client *client) {
    if (!client) {
        seat->exclusive_client = client;
        // Triggers a refocus of the topmost surface layer if necessary
        // TODO: Make layer surface focus per-output based on cursor position
        for (int i = 0; i < root->outputs->length; ++i) {
            struct hayward_output *output = root->outputs->items[i];
            arrange_layers(output);
        }
        return;
    }
    struct wlr_layer_surface_v1 *focused_layer = root_get_focused_layer(root);
    if (focused_layer) {
        if (wl_resource_get_client(focused_layer->resource) != client) {
            root_set_focused_layer(root, NULL);
        }
    }
    struct hayward_window *focused_window = root_get_focused_window(root);
    if (focused_window) {
        if (wl_resource_get_client(focused_window->view->surface->resource) !=
            client) {
            // TODO
            root_set_focused_window(root, NULL);
        }
    }
    if (seat->wlr_seat->pointer_state.focused_client) {
        if (seat->wlr_seat->pointer_state.focused_client->client != client) {
            wlr_seat_pointer_notify_clear_focus(seat->wlr_seat);
        }
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    struct wlr_touch_point *point;
    wl_list_for_each(point, &seat->wlr_seat->touch_state.touch_points, link) {
        if (point->client->client != client) {
            wlr_seat_touch_point_clear_focus(
                seat->wlr_seat, now.tv_nsec / 1000, point->touch_id
            );
        }
    }
    seat->exclusive_client = client;
}

void
seat_apply_config(struct hayward_seat *seat, struct seat_config *seat_config) {
    struct hayward_seat_device *seat_device = NULL;

    if (!seat_config) {
        return;
    }

    seat->idle_inhibit_sources = seat_config->idle_inhibit_sources;
    seat->idle_wake_sources = seat_config->idle_wake_sources;

    wl_list_for_each(seat_device, &seat->devices, link) {
        seat_configure_device(seat, seat_device->input_device);
        cursor_handle_activity_from_device(
            seat->cursor, seat_device->input_device->wlr_device
        );
    }
}

struct seat_config *
seat_get_config(struct hayward_seat *seat) {
    struct seat_config *seat_config = NULL;
    for (int i = 0; i < config->seat_configs->length; ++i) {
        seat_config = config->seat_configs->items[i];
        if (strcmp(seat->wlr_seat->name, seat_config->name) == 0) {
            return seat_config;
        }
    }

    return NULL;
}

struct seat_config *
seat_get_config_by_name(const char *name) {
    struct seat_config *seat_config = NULL;
    for (int i = 0; i < config->seat_configs->length; ++i) {
        seat_config = config->seat_configs->items[i];
        if (strcmp(name, seat_config->name) == 0) {
            return seat_config;
        }
    }

    return NULL;
}

void
seat_pointer_notify_button(
    struct hayward_seat *seat, uint32_t time_msec, uint32_t button,
    enum wlr_button_state state
) {
    seat->last_button_serial = wlr_seat_pointer_notify_button(
        seat->wlr_seat, time_msec, button, state
    );
}

void
seatop_button(
    struct hayward_seat *seat, uint32_t time_msec,
    struct wlr_input_device *device, uint32_t button,
    enum wlr_button_state state
) {
    if (seat->seatop_impl->button) {
        seat->seatop_impl->button(seat, time_msec, device, button, state);
    }
}

void
seatop_pointer_motion(struct hayward_seat *seat, uint32_t time_msec) {
    if (seat->seatop_impl->pointer_motion) {
        seat->seatop_impl->pointer_motion(seat, time_msec);
    }
}

void
seatop_pointer_axis(
    struct hayward_seat *seat, struct wlr_pointer_axis_event *event
) {
    if (seat->seatop_impl->pointer_axis) {
        seat->seatop_impl->pointer_axis(seat, event);
    }
}

void
seatop_tablet_tool_tip(
    struct hayward_seat *seat, struct hayward_tablet_tool *tool,
    uint32_t time_msec, enum wlr_tablet_tool_tip_state state
) {
    if (seat->seatop_impl->tablet_tool_tip) {
        seat->seatop_impl->tablet_tool_tip(seat, tool, time_msec, state);
    }
}

void
seatop_tablet_tool_motion(
    struct hayward_seat *seat, struct hayward_tablet_tool *tool,
    uint32_t time_msec
) {
    if (seat->seatop_impl->tablet_tool_motion) {
        seat->seatop_impl->tablet_tool_motion(seat, tool, time_msec);
    } else {
        seatop_pointer_motion(seat, time_msec);
    }
}

void
seatop_rebase(struct hayward_seat *seat, uint32_t time_msec) {
    if (seat->seatop_impl->rebase) {
        seat->seatop_impl->rebase(seat, time_msec);
    }
}

void
seatop_end(struct hayward_seat *seat) {
    if (seat->seatop_impl && seat->seatop_impl->end) {
        seat->seatop_impl->end(seat);
    }
    free(seat->seatop_data);
    seat->seatop_data = NULL;
    seat->seatop_impl = NULL;
}

void
seatop_unref(struct hayward_seat *seat, struct hayward_window *container) {
    if (seat->seatop_impl->unref) {
        seat->seatop_impl->unref(seat, container);
    }
}

bool
seatop_allows_set_cursor(struct hayward_seat *seat) {
    return seat->seatop_impl->allow_set_cursor;
}

struct hayward_keyboard_shortcuts_inhibitor *
keyboard_shortcuts_inhibitor_get_for_surface(
    const struct hayward_seat *seat, const struct wlr_surface *surface
) {
    struct hayward_keyboard_shortcuts_inhibitor *hayward_inhibitor = NULL;
    wl_list_for_each(
        hayward_inhibitor, &seat->keyboard_shortcuts_inhibitors, link
    ) {
        if (hayward_inhibitor->inhibitor->surface == surface) {
            return hayward_inhibitor;
        }
    }

    return NULL;
}

struct hayward_keyboard_shortcuts_inhibitor *
keyboard_shortcuts_inhibitor_get_for_focused_surface(
    const struct hayward_seat *seat
) {
    return keyboard_shortcuts_inhibitor_get_for_surface(
        seat, seat->wlr_seat->keyboard_state.focused_surface
    );
}
