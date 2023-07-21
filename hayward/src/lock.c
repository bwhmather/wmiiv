#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_session_lock_v1.h>

#include <hayward-common/log.h>

#include <hayward/globals/transaction.h>
#include <hayward/input/input_manager.h>
#include <hayward/input/seat.h>
#include <hayward/output.h>
#include <hayward/server.h>
#include <hayward/transaction.h>

#include <config.h>

struct hwd_session_lock_surface {
    struct wlr_session_lock_surface_v1 *lock_surface;
    struct hwd_output *output;
    struct wlr_surface *surface;
    struct wl_listener map;
    struct wl_listener destroy;
    struct wl_listener surface_commit;
    struct wl_listener output_commit;
};

static void
handle_surface_map(struct wl_listener *listener, void *data) {
    struct hwd_session_lock_surface *surf = wl_container_of(listener, surf, map);

    hwd_transaction_manager_begin_transaction(transaction_manager);

    hwd_force_focus(surf->surface);

    hwd_transaction_manager_end_transaction(transaction_manager);
}

static void
handle_surface_commit(struct wl_listener *listener, void *data) {
    struct hwd_session_lock_surface *surf = wl_container_of(listener, surf, surface_commit);

    hwd_transaction_manager_begin_transaction(transaction_manager);

    hwd_transaction_manager_end_transaction(transaction_manager);
}

static void
handle_output_commit(struct wl_listener *listener, void *data) {
    struct hwd_session_lock_surface *surf = wl_container_of(listener, surf, output_commit);
    struct wlr_output_event_commit *event = data;

    hwd_transaction_manager_begin_transaction(transaction_manager);

    if (event->committed &
        (WLR_OUTPUT_STATE_MODE | WLR_OUTPUT_STATE_SCALE | WLR_OUTPUT_STATE_TRANSFORM)) {
        wlr_session_lock_surface_v1_configure(
            surf->lock_surface, surf->output->width, surf->output->height
        );
    }

    hwd_transaction_manager_end_transaction(transaction_manager);
}

static void
handle_surface_destroy(struct wl_listener *listener, void *data) {
    struct hwd_session_lock_surface *surf = wl_container_of(listener, surf, destroy);

    hwd_transaction_manager_begin_transaction(transaction_manager);

    wl_list_remove(&surf->map.link);
    wl_list_remove(&surf->destroy.link);
    wl_list_remove(&surf->surface_commit.link);
    wl_list_remove(&surf->output_commit.link);
    free(surf);

    hwd_transaction_manager_end_transaction(transaction_manager);
}

static void
handle_new_surface(struct wl_listener *listener, void *data) {
    struct wlr_session_lock_surface_v1 *lock_surface = data;

    hwd_transaction_manager_begin_transaction(transaction_manager);

    struct hwd_session_lock_surface *surf = calloc(1, sizeof(*surf));
    if (surf == NULL) {
        hwd_transaction_manager_end_transaction(transaction_manager);
        return;
    }

    hwd_log(HWD_DEBUG, "new lock layer surface");

    struct hwd_output *output = lock_surface->output->data;
    wlr_session_lock_surface_v1_configure(lock_surface, output->width, output->height);

    surf->lock_surface = lock_surface;
    surf->surface = lock_surface->surface;
    surf->output = output;
    surf->map.notify = handle_surface_map;
    wl_signal_add(&lock_surface->surface->events.map, &surf->map);
    surf->destroy.notify = handle_surface_destroy;
    wl_signal_add(&lock_surface->events.destroy, &surf->destroy);
    surf->surface_commit.notify = handle_surface_commit;
    wl_signal_add(&surf->surface->events.commit, &surf->surface_commit);
    surf->output_commit.notify = handle_output_commit;
    wl_signal_add(&output->wlr_output->events.commit, &surf->output_commit);

    hwd_transaction_manager_end_transaction(transaction_manager);
}

static void
handle_unlock(struct wl_listener *listener, void *data) {
    hwd_transaction_manager_begin_transaction(transaction_manager);

    hwd_log(HWD_DEBUG, "session unlocked");
    server.session_lock.locked = false;
    server.session_lock.lock = NULL;

    wl_list_remove(&server.session_lock.lock_new_surface.link);
    wl_list_remove(&server.session_lock.lock_unlock.link);
    wl_list_remove(&server.session_lock.lock_destroy.link);

    hwd_transaction_manager_end_transaction(transaction_manager);
}

static void
handle_abandon(struct wl_listener *listener, void *data) {
    hwd_transaction_manager_begin_transaction(transaction_manager);

    hwd_log(HWD_INFO, "session lock abandoned");
    server.session_lock.lock = NULL;

    wl_list_remove(&server.session_lock.lock_new_surface.link);
    wl_list_remove(&server.session_lock.lock_unlock.link);
    wl_list_remove(&server.session_lock.lock_destroy.link);

    struct hwd_seat *seat;
    wl_list_for_each(seat, &server.input->seats, link) { seat->exclusive_client = NULL; }

    hwd_transaction_manager_end_transaction(transaction_manager);
}

static void
handle_session_lock(struct wl_listener *listener, void *data) {
    struct wlr_session_lock_v1 *lock = data;

    hwd_transaction_manager_begin_transaction(transaction_manager);

    struct wl_client *client = wl_resource_get_client(lock->resource);

    if (server.session_lock.lock) {
        wlr_session_lock_v1_destroy(lock);
        hwd_transaction_manager_end_transaction(transaction_manager);
        return;
    }

    hwd_log(HWD_DEBUG, "session locked");
    server.session_lock.locked = true;
    server.session_lock.lock = lock;

    struct hwd_seat *seat;
    wl_list_for_each(seat, &server.input->seats, link) { seat_set_exclusive_client(seat, client); }

    wl_signal_add(&lock->events.new_surface, &server.session_lock.lock_new_surface);
    wl_signal_add(&lock->events.unlock, &server.session_lock.lock_unlock);
    wl_signal_add(&lock->events.destroy, &server.session_lock.lock_destroy);

    wlr_session_lock_v1_send_locked(lock);

    hwd_transaction_manager_end_transaction(transaction_manager);
}

static void
handle_session_lock_destroy(struct wl_listener *listener, void *data) {
    hwd_transaction_manager_begin_transaction(transaction_manager);

    assert(server.session_lock.lock == NULL);
    wl_list_remove(&server.session_lock.new_lock.link);
    wl_list_remove(&server.session_lock.manager_destroy.link);

    hwd_transaction_manager_end_transaction(transaction_manager);
}

void
hwd_session_lock_init(void) {
    server.session_lock.manager = wlr_session_lock_manager_v1_create(server.wl_display);

    server.session_lock.lock_new_surface.notify = handle_new_surface;
    server.session_lock.lock_unlock.notify = handle_unlock;
    server.session_lock.lock_destroy.notify = handle_abandon;
    server.session_lock.new_lock.notify = handle_session_lock;
    server.session_lock.manager_destroy.notify = handle_session_lock_destroy;
    wl_signal_add(&server.session_lock.manager->events.new_lock, &server.session_lock.new_lock);
    wl_signal_add(
        &server.session_lock.manager->events.destroy, &server.session_lock.manager_destroy
    );
}
