#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include <config.h>

#include "hayward/config.h"

#include <assert.h>
#include <drm_fourcc.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-util.h>
#include <wlr/backend/drm.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/box.h>
#include <xf86drmMode.h>

#include <hayward-common/log.h>
#include <hayward-common/util.h>

#include <wayland-server-protocol.h>

#include <hayward/globals/root.h>
#include <hayward/input/cursor.h>
#include <hayward/input/input_manager.h>
#include <hayward/input/seat.h>
#include <hayward/output.h>
#include <hayward/server.h>
#include <hayward/tree/root.h>

void
output_get_identifier(char *identifier, size_t len, struct hwd_output *output) {
    struct wlr_output *wlr_output = output->wlr_output;
    snprintf(identifier, len, "%s %s %s", wlr_output->make, wlr_output->model, wlr_output->serial);
}

const char *
hwd_output_scale_filter_to_string(enum scale_filter_mode scale_filter) {
    switch (scale_filter) {
    case SCALE_FILTER_DEFAULT:
        return "smart";
    case SCALE_FILTER_LINEAR:
        return "linear";
    case SCALE_FILTER_NEAREST:
        return "nearest";
    case SCALE_FILTER_SMART:
        return "smart";
    }
    hwd_assert(false, "Unknown value for scale_filter.");
    return NULL;
}

struct output_config *
new_output_config(const char *name) {
    struct output_config *oc = calloc(1, sizeof(struct output_config));
    if (oc == NULL) {
        return NULL;
    }
    oc->name = strdup(name);
    if (oc->name == NULL) {
        free(oc);
        return NULL;
    }
    oc->enabled = -1;
    oc->width = oc->height = -1;
    oc->refresh_rate = -1;
    oc->custom_mode = -1;
    oc->drm_mode.type = -1;
    oc->x = oc->y = -1;
    oc->scale = -1;
    oc->scale_filter = SCALE_FILTER_DEFAULT;
    oc->transform = -1;
    oc->subpixel = WL_OUTPUT_SUBPIXEL_UNKNOWN;
    oc->max_render_time = -1;
    oc->adaptive_sync = -1;
    oc->render_bit_depth = RENDER_BIT_DEPTH_DEFAULT;
    return oc;
}

static void
merge_output_config(struct output_config *dst, struct output_config *src) {
    if (src->enabled != -1) {
        dst->enabled = src->enabled;
    }
    if (src->width != -1) {
        dst->width = src->width;
    }
    if (src->height != -1) {
        dst->height = src->height;
    }
    if (src->x != -1) {
        dst->x = src->x;
    }
    if (src->y != -1) {
        dst->y = src->y;
    }
    if (src->scale != -1) {
        dst->scale = src->scale;
    }
    if (src->scale_filter != SCALE_FILTER_DEFAULT) {
        dst->scale_filter = src->scale_filter;
    }
    if (src->subpixel != WL_OUTPUT_SUBPIXEL_UNKNOWN) {
        dst->subpixel = src->subpixel;
    }
    if (src->refresh_rate != -1) {
        dst->refresh_rate = src->refresh_rate;
    }
    if (src->custom_mode != -1) {
        dst->custom_mode = src->custom_mode;
    }
    if (src->drm_mode.type != (uint32_t)-1) {
        memcpy(&dst->drm_mode, &src->drm_mode, sizeof(src->drm_mode));
    }
    if (src->transform != -1) {
        dst->transform = src->transform;
    }
    if (src->max_render_time != -1) {
        dst->max_render_time = src->max_render_time;
    }
    if (src->adaptive_sync != -1) {
        dst->adaptive_sync = src->adaptive_sync;
    }
    if (src->render_bit_depth != RENDER_BIT_DEPTH_DEFAULT) {
        dst->render_bit_depth = src->render_bit_depth;
    }
    if (src->dpms_state != 0) {
        dst->dpms_state = src->dpms_state;
    }
}

static void
set_mode(struct wlr_output *output, int width, int height, float refresh_rate, bool custom) {
    // Not all floating point integers can be represented exactly
    // as (int)(1000 * mHz / 1000.f)
    // round() the result to avoid any error
    int mhz = (int)round(refresh_rate * 1000);

    if (wl_list_empty(&output->modes) || custom) {
        hwd_log(HWD_DEBUG, "Assigning custom mode to %s", output->name);
        wlr_output_set_custom_mode(output, width, height, refresh_rate > 0 ? mhz : 0);
        return;
    }

    struct wlr_output_mode *mode, *best = NULL;
    wl_list_for_each(mode, &output->modes, link) {
        if (mode->width == width && mode->height == height) {
            if (mode->refresh == mhz) {
                best = mode;
                break;
            }
            if (best == NULL || mode->refresh > best->refresh) {
                best = mode;
            }
        }
    }
    if (!best) {
        hwd_log(HWD_ERROR, "Configured mode for %s not available", output->name);
        hwd_log(HWD_INFO, "Picking preferred mode instead");
        best = wlr_output_preferred_mode(output);
    } else {
        hwd_log(HWD_DEBUG, "Assigning configured mode to %s", output->name);
    }
    wlr_output_set_mode(output, best);
}

static void
set_modeline(struct wlr_output *output, drmModeModeInfo *drm_mode) {
    if (!wlr_output_is_drm(output)) {
        hwd_log(HWD_ERROR, "Modeline can only be set to DRM output");
        return;
    }
    hwd_log(HWD_DEBUG, "Assigning custom modeline to %s", output->name);
    struct wlr_output_mode *mode = wlr_drm_connector_add_mode(output, drm_mode);
    if (mode) {
        wlr_output_set_mode(output, mode);
    }
}

/* Some manufacturers hardcode the aspect-ratio of the output in the physical
 * size field. */
static bool
phys_size_is_aspect_ratio(struct wlr_output *output) {
    return (output->phys_width == 1600 && output->phys_height == 900) ||
        (output->phys_width == 1600 && output->phys_height == 1000) ||
        (output->phys_width == 160 && output->phys_height == 90) ||
        (output->phys_width == 160 && output->phys_height == 100) ||
        (output->phys_width == 16 && output->phys_height == 9) ||
        (output->phys_width == 16 && output->phys_height == 10);
}

// The minimum DPI at which we turn on a scale of 2
#define HIDPI_DPI_LIMIT (2 * 96)
// The minimum screen height at which we turn on a scale of 2
#define HIDPI_MIN_HEIGHT 1200
// 1 inch = 25.4 mm
#define MM_PER_INCH 25.4

static int
compute_default_scale(struct wlr_output *output) {
    struct wlr_box box = {.width = output->width, .height = output->height};
    if (output->pending.committed & WLR_OUTPUT_STATE_MODE) {
        switch (output->pending.mode_type) {
        case WLR_OUTPUT_STATE_MODE_FIXED:
            box.width = output->pending.mode->width;
            box.height = output->pending.mode->height;
            break;
        case WLR_OUTPUT_STATE_MODE_CUSTOM:
            box.width = output->pending.custom_mode.width;
            box.height = output->pending.custom_mode.height;
            break;
        }
    }
    enum wl_output_transform transform = output->transform;
    if (output->pending.committed & WLR_OUTPUT_STATE_TRANSFORM) {
        transform = output->pending.transform;
    }
    wlr_box_transform(&box, &box, transform, box.width, box.height);

    int width = box.width;
    int height = box.height;

    if (height < HIDPI_MIN_HEIGHT) {
        return 1;
    }

    if (output->phys_width == 0 || output->phys_height == 0) {
        return 1;
    }

    if (phys_size_is_aspect_ratio(output)) {
        return 1;
    }

    double dpi_x = (double)width / (output->phys_width / MM_PER_INCH);
    double dpi_y = (double)height / (output->phys_height / MM_PER_INCH);
    hwd_log(HWD_DEBUG, "Output DPI: %fx%f", dpi_x, dpi_y);
    if (dpi_x <= HIDPI_DPI_LIMIT || dpi_y <= HIDPI_DPI_LIMIT) {
        return 1;
    }

    return 2;
}

/* Lists of formats to try, in order, when a specific render bit depth has
 * been asked for. The second to last format in each list should always
 * be XRGB8888, as a reliable backup in case the others are not available;
 * the last should be DRM_FORMAT_INVALID, to indicate the end of the list. */
static const uint32_t *bit_depth_preferences[] = {
    [RENDER_BIT_DEPTH_8] =
        (const uint32_t[]){
            DRM_FORMAT_XRGB8888,
            DRM_FORMAT_INVALID,
        },
    [RENDER_BIT_DEPTH_10] =
        (const uint32_t[]){
            DRM_FORMAT_XRGB2101010,
            DRM_FORMAT_XBGR2101010,
            DRM_FORMAT_XRGB8888,
            DRM_FORMAT_INVALID,
        },
};

static void
queue_output_config(struct output_config *oc, struct hwd_output *output) {
    if (output == root->fallback_output) {
        return;
    }

    struct wlr_output *wlr_output = output->wlr_output;

    if (oc && (!oc->enabled || oc->dpms_state == DPMS_OFF)) {
        hwd_log(HWD_DEBUG, "Turning off output %s", wlr_output->name);
        wlr_output_enable(wlr_output, false);
        return;
    }

    hwd_log(HWD_DEBUG, "Turning on output %s", wlr_output->name);
    wlr_output_enable(wlr_output, true);

    if (oc && oc->drm_mode.type != 0 && oc->drm_mode.type != (uint32_t)-1) {
        hwd_log(HWD_DEBUG, "Set %s modeline", wlr_output->name);
        set_modeline(wlr_output, &oc->drm_mode);
    } else if (oc && oc->width > 0 && oc->height > 0) {
        hwd_log(
            HWD_DEBUG, "Set %s mode to %dx%d (%f Hz)", wlr_output->name, oc->width, oc->height,
            oc->refresh_rate
        );
        set_mode(wlr_output, oc->width, oc->height, oc->refresh_rate, oc->custom_mode == 1);
    } else if (!wl_list_empty(&wlr_output->modes)) {
        hwd_log(HWD_DEBUG, "Set preferred mode");
        struct wlr_output_mode *preferred_mode = wlr_output_preferred_mode(wlr_output);
        wlr_output_set_mode(wlr_output, preferred_mode);

        if (!wlr_output_test(wlr_output)) {
            hwd_log(
                HWD_DEBUG,
                "Preferred mode rejected, "
                "falling back to another mode"
            );
            struct wlr_output_mode *mode;
            wl_list_for_each(mode, &wlr_output->modes, link) {
                if (mode == preferred_mode) {
                    continue;
                }

                wlr_output_set_mode(wlr_output, mode);
                if (wlr_output_test(wlr_output)) {
                    break;
                }
            }
        }
    }

    if (oc && (oc->subpixel != WL_OUTPUT_SUBPIXEL_UNKNOWN || config->reloading)) {
        hwd_log(
            HWD_DEBUG, "Set %s subpixel to %s", oc->name,
            hwd_wl_output_subpixel_to_string(oc->subpixel)
        );
        wlr_output_set_subpixel(wlr_output, oc->subpixel);
    }

    enum wl_output_transform tr = WL_OUTPUT_TRANSFORM_NORMAL;
    if (oc && oc->transform >= 0) {
        tr = oc->transform;
    } else if (wlr_output_is_drm(wlr_output)) {
        tr = wlr_drm_connector_get_panel_orientation(wlr_output);
        hwd_log(HWD_DEBUG, "Auto-detected output transform: %d", tr);
    }
    if (wlr_output->transform != tr) {
        hwd_log(HWD_DEBUG, "Set %s transform to %d", oc->name, tr);
        wlr_output_set_transform(wlr_output, tr);
    }

    // Apply the scale last before the commit, because the scale auto-detection
    // reads the pending output size
    float scale;
    if (oc && oc->scale > 0) {
        scale = oc->scale;
    } else {
        scale = compute_default_scale(wlr_output);
        hwd_log(HWD_DEBUG, "Auto-detected output scale: %f", scale);
    }
    if (scale != wlr_output->scale) {
        hwd_log(HWD_DEBUG, "Set %s scale to %f", wlr_output->name, scale);
        wlr_output_set_scale(wlr_output, scale);
    }

    if (oc && oc->adaptive_sync != -1) {
        hwd_log(HWD_DEBUG, "Set %s adaptive sync to %d", wlr_output->name, oc->adaptive_sync);
        wlr_output_enable_adaptive_sync(wlr_output, oc->adaptive_sync == 1);
    }

    if (oc && oc->render_bit_depth != RENDER_BIT_DEPTH_DEFAULT) {
        const uint32_t *fmts = bit_depth_preferences[oc->render_bit_depth];
        assert(fmts);

        for (size_t i = 0; fmts[i] != DRM_FORMAT_INVALID; i++) {
            wlr_output_set_render_format(wlr_output, fmts[i]);
            if (wlr_output_test(wlr_output)) {
                break;
            }

            hwd_log(
                HWD_DEBUG,
                "Preferred output format 0x%08x "
                "failed to work, falling back to next in "
                "list, 0x%08x",
                fmts[i], fmts[i + 1]
            );
        }
    }
}

bool
apply_output_config(struct output_config *oc, struct hwd_output *output) {
    if (output == root->fallback_output) {
        return false;
    }

    struct wlr_output *wlr_output = output->wlr_output;

    // Flag to prevent the output mode event handler from calling us
    output->enabling = (!oc || oc->enabled);

    queue_output_config(oc, output);

    if (!oc || oc->dpms_state != DPMS_OFF) {
        output->current_mode = wlr_output->pending.mode;
    }

    hwd_log(HWD_DEBUG, "Committing output %s", wlr_output->name);
    if (!wlr_output_commit(wlr_output)) {
        // Failed to commit output changes, maybe the output is missing a CRTC.
        // Leave the output disabled for now and try again when the output gets
        // the mode we asked for.
        hwd_log(HWD_ERROR, "Failed to commit output %s", wlr_output->name);
        output->enabling = false;
        return false;
    }

    output->enabling = false;

    if (oc && !oc->enabled) {
        hwd_log(HWD_DEBUG, "Disabling output %s", oc->name);
        if (output->enabled) {
            output_disable(output);
            wlr_output_layout_remove(root->output_layout, wlr_output);
        }
        return true;
    }

    if (oc) {
        enum scale_filter_mode scale_filter_old = output->scale_filter;
        switch (oc->scale_filter) {
        case SCALE_FILTER_DEFAULT:
        case SCALE_FILTER_SMART:
            output->scale_filter = ceilf(wlr_output->scale) == wlr_output->scale
                ? SCALE_FILTER_NEAREST
                : SCALE_FILTER_LINEAR;
            break;
        case SCALE_FILTER_LINEAR:
        case SCALE_FILTER_NEAREST:
            output->scale_filter = oc->scale_filter;
            break;
        }
        if (scale_filter_old != output->scale_filter) {
            hwd_log(
                HWD_DEBUG, "Set %s scale_filter to %s", oc->name,
                hwd_output_scale_filter_to_string(output->scale_filter)
            );
        }
    }

    // Find position for it
    if (oc && (oc->x != -1 || oc->y != -1)) {
        hwd_log(HWD_DEBUG, "Set %s position to %d, %d", oc->name, oc->x, oc->y);
        wlr_output_layout_add(root->output_layout, wlr_output, oc->x, oc->y);
    } else {
        wlr_output_layout_add_auto(root->output_layout, wlr_output);
    }

    // Update output->{lx, ly, width, height}
    struct wlr_box output_box;
    wlr_output_layout_get_box(root->output_layout, wlr_output, &output_box);
    output->lx = output_box.x;
    output->ly = output_box.y;
    output->width = output_box.width;
    output->height = output_box.height;

    if (!output->enabled) {
        output_enable(output);
    }

    if (oc && oc->max_render_time >= 0) {
        hwd_log(HWD_DEBUG, "Set %s max render time to %d", oc->name, oc->max_render_time);
        output->max_render_time = oc->max_render_time;
    }

    // Reconfigure all devices, since input config may have been applied before
    // this output came online, and some config items (like map_to_output) are
    // dependent on an output being present.
    input_manager_configure_all_inputs();
    // Reconfigure the cursor images, since the scale may have changed.
    input_manager_configure_xcursor();
    return true;
}

bool
test_output_config(struct output_config *oc, struct hwd_output *output) {
    if (output == root->fallback_output) {
        return false;
    }

    queue_output_config(oc, output);
    bool ok = wlr_output_test(output->wlr_output);
    wlr_output_rollback(output->wlr_output);
    return ok;
}

static void
default_output_config(struct output_config *oc, struct wlr_output *wlr_output) {
    oc->enabled = 1;
    struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
    if (mode != NULL) {
        oc->width = mode->width;
        oc->height = mode->height;
        oc->refresh_rate = mode->refresh / 1000.f;
    }
    oc->x = oc->y = -1;
    oc->scale = 0; // auto
    oc->scale_filter = SCALE_FILTER_DEFAULT;
    struct hwd_output *output = wlr_output->data;
    oc->subpixel = output->detected_subpixel;
    oc->transform = WL_OUTPUT_TRANSFORM_NORMAL;
    oc->dpms_state = DPMS_ON;
    oc->max_render_time = 0;
}

static void
apply_output_config_to_outputs(struct output_config *oc) {
    // Try to find the output container and apply configuration now. If
    // this is during startup then there will be no container and config
    // will be applied during normal "new output" event from wlroots.
    bool wildcard = strcmp(oc->name, "*") == 0;
    char id[128];
    struct hwd_output *hwd_output, *tmp;
    wl_list_for_each_safe(hwd_output, tmp, &root->all_outputs, link) {
        char *name = hwd_output->wlr_output->name;
        output_get_identifier(id, sizeof(id), hwd_output);
        if (wildcard || !strcmp(name, oc->name) || !strcmp(id, oc->name)) {
            struct output_config *config = new_output_config(oc->name);
            default_output_config(config, hwd_output->wlr_output);
            merge_output_config(config, oc);
            apply_output_config(oc, hwd_output);

            if (!wildcard) {
                // Stop looking if the output config isn't applicable to all
                // outputs
                break;
            }
        }
    }

    struct hwd_seat *seat;
    wl_list_for_each(seat, &server.input->seats, link) {
        wlr_seat_pointer_notify_clear_focus(seat->wlr_seat);
        cursor_rebase(seat->cursor);
    }
}

void
reset_outputs(void) {
    apply_output_config_to_outputs(new_output_config("*"));
}

void
free_output_config(struct output_config *oc) {
    if (!oc) {
        return;
    }
    free(oc->name);
    free(oc);
}
