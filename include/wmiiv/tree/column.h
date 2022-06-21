#ifndef _WMIIV_COLUMN_H
#define _WMIIV_COLUMN_H
#include <stdint.h>
#include <sys/types.h>
#include <wlr/types/wlr_compositor.h>
#include "list.h"
#include "wmiiv/tree/node.h"

struct wmiiv_container *column_create(void);

void column_destroy(struct wmiiv_container *column);

void column_begin_destroy(struct wmiiv_container *column);

void column_consider_destroy(struct wmiiv_container *container);

/**
 * Search a container's descendants a container based on test criteria. Returns
 * the first container that passes the test.
 */
struct wmiiv_container *column_find_child(struct wmiiv_container *container,
		bool (*test)(struct wmiiv_container *view, void *data), void *data);

void column_add_child(struct wmiiv_container *parent,
		struct wmiiv_container *child);

void column_insert_child(struct wmiiv_container *parent,
		struct wmiiv_container *child, int i);

/**
 * Side should be 0 to add before, or 1 to add after.
 */
void column_add_sibling(struct wmiiv_container *parent,
		struct wmiiv_container *child, bool after);

void column_detach(struct wmiiv_container *column);

void column_for_each_child(struct wmiiv_container *column,
		void (*f)(struct wmiiv_container *window, void *data), void *data);

void column_damage_whole(struct wmiiv_container *column);

size_t column_build_representation(enum wmiiv_container_layout layout,
		list_t *children, char *buffer);

void column_update_representation(struct wmiiv_container *column);

/**
 * Get a column's box in layout coordinates.
 */
void column_get_box(struct wmiiv_container *column, struct wlr_box *box);

void column_set_resizing(struct wmiiv_container *column, bool resizing);

list_t *column_get_siblings(struct wmiiv_container *column);

int column_sibling_index(struct wmiiv_container *child);

list_t *column_get_current_siblings(struct wmiiv_container *column);

struct wmiiv_container *column_get_previous_sibling(struct wmiiv_container *column);
struct wmiiv_container *column_get_next_sibling(struct wmiiv_container *column);

/**
 * Return the output which will be used for scale purposes.
 * This is the most recently entered output.
 * If the container is not on any output, return NULL.
 */
struct wmiiv_output *column_get_effective_output(struct wmiiv_container *column);

void column_discover_outputs(struct wmiiv_container *column);

bool column_has_urgent_child(struct wmiiv_container *column);

#endif
