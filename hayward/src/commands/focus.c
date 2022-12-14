#include <float.h>
#include <strings.h>
#include <wlr/types/wlr_output_layout.h>
#include "hayward-common/log.h"
#include "hayward/commands.h"
#include "hayward/input/input-manager.h"
#include "hayward/input/cursor.h"
#include "hayward/input/seat.h"
#include "hayward/output.h"
#include "hayward/tree/arrange.h"
#include "hayward/tree/root.h"
#include "hayward/tree/view.h"
#include "hayward/tree/workspace.h"
#include "hayward-common/stringop.h"
#include "hayward-common/util.h"

static bool parse_direction(const char *name,
		enum wlr_direction *out) {
	if (strcasecmp(name, "left") == 0) {
		*out = WLR_DIRECTION_LEFT;
	} else if (strcasecmp(name, "right") == 0) {
		*out = WLR_DIRECTION_RIGHT;
	} else if (strcasecmp(name, "up") == 0) {
		*out = WLR_DIRECTION_UP;
	} else if (strcasecmp(name, "down") == 0) {
		*out = WLR_DIRECTION_DOWN;
	} else {
		return false;
	}

	return true;
}

/**
 * Returns the node that should be focused if entering an output by moving
 * in the given direction.
 *
 *  Node should always be either a workspace or a window.
 */
static struct hayward_window *get_window_in_output_direction(struct hayward_output *output, enum wlr_direction dir) {
	hayward_assert(output != NULL, "Expected output");

	struct hayward_workspace *workspace = root_get_active_workspace();
	hayward_assert(workspace != NULL, "Expected workspace");

	if (output->pending.fullscreen_window) {
		return output->pending.fullscreen_window;
	}

	struct hayward_column *column = NULL;
	struct hayward_window *window = NULL;

	// TODO this is completly broken now that one workspace is spread across all outputs.
	if (workspace->pending.tiling->length > 0) {
		switch (dir) {
		case WLR_DIRECTION_LEFT:
			// get most right child of new output
			column = workspace->pending.tiling->items[workspace->pending.tiling->length-1];
			window = column->pending.active_child;
			break;
		case WLR_DIRECTION_RIGHT:
			// get most left child of new output
			column = workspace->pending.tiling->items[0];
			window = column->pending.active_child;
			break;
		case WLR_DIRECTION_UP:
			window = workspace_get_active_tiling_window(workspace);
			break;
		case WLR_DIRECTION_DOWN:
			window = workspace_get_active_tiling_window(workspace);
			break;
		}
	}

	return window;
}

static struct hayward_window *window_get_in_direction_tiling(
		struct hayward_window *window, struct hayward_seat *seat,
		enum wlr_direction dir) {
	struct hayward_window *wrap_candidate = NULL;

	if (window->pending.fullscreen) {
		// Fullscreen container with a direction - go straight to outputs
		struct hayward_output *output = window_get_output(window);
		struct hayward_output *new_output =
			output_get_in_direction(output, dir);
		if (!new_output) {
			return NULL;
		}
		return get_window_in_output_direction(new_output, dir);
	}

	// TODO (hayward) this is a manually unrolled recursion over container.  Make it nice.
	// Window iteration.
	if (dir == WLR_DIRECTION_UP || dir == WLR_DIRECTION_DOWN) {
		// Try to move up and down within the current column.
		int current_idx = window_sibling_index(window);
		int desired_idx = current_idx + (dir == WLR_DIRECTION_UP ? -1 : 1);

		list_t *siblings = window_get_siblings(window);

		if (desired_idx >= 0 && desired_idx < siblings->length) {
			return siblings->items[desired_idx];
		}

		if (config->focus_wrapping != WRAP_NO && !wrap_candidate && siblings->length > 1) {
			if (desired_idx < 0) {
				wrap_candidate = siblings->items[siblings->length - 1];
			} else {
				wrap_candidate = siblings->items[0];
			}
			if (config->focus_wrapping == WRAP_FORCE) {
				return wrap_candidate;
			}
		}
	} else {
		// Try to move to the next column to the left of right within
		// the current workspace.
		struct hayward_column *column = window->pending.parent;

		int current_idx = column_sibling_index(column);
		int desired_idx = current_idx + (dir == WLR_DIRECTION_LEFT ? -1 : 1);

		list_t *siblings = column_get_siblings(column);

		if (desired_idx >= 0 && desired_idx < siblings->length) {
			struct hayward_column *next_column = siblings->items[desired_idx];
			struct hayward_window *next_window = next_column->pending.active_child;
			return next_window;
		}

		if (config->focus_wrapping != WRAP_NO && !wrap_candidate && siblings->length > 1) {
			struct hayward_column *wrap_candidate_column;
			if (desired_idx < 0) {
				wrap_candidate_column = siblings->items[siblings->length - 1];
			} else {
				wrap_candidate_column = siblings->items[0];
			}
			wrap_candidate = wrap_candidate_column->pending.active_child;
			if (config->focus_wrapping == WRAP_FORCE) {
				return wrap_candidate;
			}
		}
	}

	// Check a different output
	struct hayward_output *output = window_get_output(window);
	struct hayward_output *new_output = output_get_in_direction(output, dir);
	if (config->focus_wrapping != WRAP_WORKSPACE && new_output) {
		return get_window_in_output_direction(new_output, dir);
	}

	// If there is a wrap candidate, return its focus inactive view
	if (wrap_candidate) {
		return wrap_candidate;
	}

	return NULL;
}

static struct hayward_window *window_get_in_direction_floating(
		struct hayward_window *container, struct hayward_seat *seat,
		enum wlr_direction dir) {
	double ref_lx = container->pending.x + container->pending.width / 2;
	double ref_ly = container->pending.y + container->pending.height / 2;
	double closest_distance = DBL_MAX;
	struct hayward_window *closest_container = NULL;

	if (!container->pending.workspace) {
		return NULL;
	}

	for (int i = 0; i < container->pending.workspace->pending.floating->length; i++) {
		struct hayward_window *floater = container->pending.workspace->pending.floating->items[i];
		if (floater == container) {
			continue;
		}
		float distance = dir == WLR_DIRECTION_LEFT || dir == WLR_DIRECTION_RIGHT
			? (floater->pending.x + floater->pending.width / 2) - ref_lx
			: (floater->pending.y + floater->pending.height / 2) - ref_ly;
		if (dir == WLR_DIRECTION_LEFT || dir == WLR_DIRECTION_UP) {
			distance = -distance;
		}
		if (distance < 0) {
			continue;
		}
		if (distance < closest_distance) {
			closest_distance = distance;
			closest_container = floater;
		}
	}

	return closest_container;
}

static struct cmd_results *focus_mode(struct hayward_workspace *workspace, bool floating) {
	struct hayward_window *new_focus = NULL;
	if (floating) {
		new_focus = workspace_get_active_floating_window(workspace);
	} else {
		new_focus = workspace_get_active_tiling_window(workspace);
	}
	if (new_focus) {
		workspace_set_active_window(workspace, new_focus);
	} else {
		return cmd_results_new(CMD_FAILURE,
				"Failed to find a %s container in workspace.",
				floating ? "floating" : "tiling");
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_focus(int argc, char **argv) {
	if (config->reading || !config->active) {
		return cmd_results_new(CMD_DEFER, NULL);
	}
	if (!root->outputs->length) {
		return cmd_results_new(CMD_INVALID,
				"Can't run this command while there are no outputs connected.");
	}
	struct hayward_workspace *workspace = config->handler_context.workspace;
	struct hayward_seat *seat = config->handler_context.seat;

	struct hayward_output *output = root_get_active_output();
	hayward_assert(output != NULL, "Expected output");

	struct hayward_window *window = root_get_focused_window();

	if (argc == 0) {
		return cmd_results_new(CMD_INVALID,
			"Expected 'focus <direction|mode_toggle|floating|tiling>' ");
	}

	if (strcmp(argv[0], "floating") == 0) {
		return focus_mode(workspace, true);
	} else if (strcmp(argv[0], "tiling") == 0) {
		return focus_mode(workspace, false);
	} else if (strcmp(argv[0], "mode_toggle") == 0) {
		bool floating = window && window_is_floating(window);
		return focus_mode(workspace, !floating);
	}

	enum wlr_direction direction = 0;
	if (!parse_direction(argv[0], &direction)) {
		return cmd_results_new(CMD_INVALID,
			"Expected 'focus <direction|mode_toggle|floating|tiling>' ");
	}

	if (!direction) {
		return cmd_results_new(CMD_SUCCESS, NULL);
	}

	if (window == NULL) {
		// Jump to the next output
		struct hayward_output *new_output =
			output_get_in_direction(output, direction);
		if (!new_output) {
			return cmd_results_new(CMD_SUCCESS, NULL);
		}

		window = get_window_in_output_direction(new_output, direction);
		if (window != NULL) {
			root_set_focused_window(window);
		} else {
			// TODO might make more sense to move this to the root.
			workspace_set_active_window(workspace, NULL);
			root_set_active_output(new_output);
		}
		return cmd_results_new(CMD_SUCCESS, NULL);
	}

	struct hayward_window *next_focus = NULL;
	if (window_is_floating(window) && !window->pending.fullscreen) {
		next_focus = window_get_in_direction_floating(window, seat, direction);
	} else {
		next_focus = window_get_in_direction_tiling(window, seat, direction);
	}
	if (next_focus) {
		root_set_focused_window(next_focus);
	}

	return cmd_results_new(CMD_SUCCESS, NULL);
}
