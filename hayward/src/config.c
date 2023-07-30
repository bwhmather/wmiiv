#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include "hayward/config.h"

#include <config.h>

#include <fcntl.h>
#include <libgen.h>
#include <linux/input-event-codes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/types/wlr_seat.h>
#include <wordexp.h>
#include <xkbcommon/xkbcommon.h>

#include <hayward-common/list.h>
#include <hayward-common/log.h>
#include <hayward-common/pango.h>
#include <hayward-common/stringop.h>
#include <hayward-common/util.h>

#include <hayward/commands.h>
#include <hayward/globals/root.h>
#include <hayward/haywardnag.h>
#include <hayward/input/input_manager.h>
#include <hayward/input/seat.h>
#include <hayward/input/switch.h>
#include <hayward/server.h>
#include <hayward/tree/arrange.h>

struct hwd_config *config = NULL;

static bool
read_config(FILE *file, struct hwd_config *config, struct haywardnag_instance *haywardnag);

static struct xkb_state *
keysym_translation_state_create(struct xkb_rule_names rules) {
    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *xkb_keymap =
        xkb_keymap_new_from_names(context, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);

    xkb_context_unref(context);
    return xkb_state_new(xkb_keymap);
}

static void
keysym_translation_state_destroy(struct xkb_state *state) {
    xkb_keymap_unref(xkb_state_get_keymap(state));
    xkb_state_unref(state);
}

static void
free_mode(struct hwd_mode *mode) {
    if (!mode) {
        return;
    }
    free(mode->name);
    if (mode->keysym_bindings) {
        for (int i = 0; i < mode->keysym_bindings->length; i++) {
            free_hwd_binding(mode->keysym_bindings->items[i]);
        }
        list_free(mode->keysym_bindings);
    }
    if (mode->keycode_bindings) {
        for (int i = 0; i < mode->keycode_bindings->length; i++) {
            free_hwd_binding(mode->keycode_bindings->items[i]);
        }
        list_free(mode->keycode_bindings);
    }
    if (mode->mouse_bindings) {
        for (int i = 0; i < mode->mouse_bindings->length; i++) {
            free_hwd_binding(mode->mouse_bindings->items[i]);
        }
        list_free(mode->mouse_bindings);
    }
    if (mode->switch_bindings) {
        for (int i = 0; i < mode->switch_bindings->length; i++) {
            free_switch_binding(mode->switch_bindings->items[i]);
        }
        list_free(mode->switch_bindings);
    }
    free(mode);
}

static void
destroy_removed_seats(struct hwd_config *old_config, struct hwd_config *new_config) {
    struct seat_config *seat_config;
    struct hwd_seat *seat;
    int i;
    for (i = 0; i < old_config->seat_configs->length; i++) {
        seat_config = old_config->seat_configs->items[i];
        // Skip the wildcard seat config, it won't have a matching real seat.
        if (strcmp(seat_config->name, "*") == 0) {
            continue;
        }

        /* Also destroy seats that aren't present in new config */
        if (new_config &&
            list_seq_find(new_config->seat_configs, seat_name_cmp, seat_config->name) < 0) {
            seat = input_manager_get_seat(seat_config->name, false);
            if (seat) {
                seat_destroy(seat);
            }
        }
    }
}

static void
config_defaults(struct hwd_config *config) {
    if (!(config->haywardnag_command = strdup("haywardnag")))
        goto cleanup;
    config->haywardnag_config_errors = (struct haywardnag_instance){0};
    config->haywardnag_config_errors.args =
        "--type error "
        "--message 'There are errors in your config file' "
        "--detailed-message "
        "--button-no-terminal 'Exit hayward' 'haywardmsg exit' "
        "--button-no-terminal 'Reload hayward' 'haywardmsg reload'";
    config->haywardnag_config_errors.detailed = true;

    if (!(config->symbols = create_list()))
        goto cleanup;
    if (!(config->modes = create_list()))
        goto cleanup;
    if (!(config->bars = create_list()))
        goto cleanup;
    if (!(config->criteria = create_list()))
        goto cleanup;
    if (!(config->no_focus = create_list()))
        goto cleanup;
    if (!(config->seat_configs = create_list()))
        goto cleanup;
    if (!(config->output_configs = create_list()))
        goto cleanup;

    if (!(config->input_type_configs = create_list()))
        goto cleanup;
    if (!(config->input_configs = create_list()))
        goto cleanup;

    if (!(config->cmd_queue = create_list()))
        goto cleanup;

    if (!(config->current_mode = malloc(sizeof(struct hwd_mode))))
        goto cleanup;
    if (!(config->current_mode->name = malloc(sizeof("default"))))
        goto cleanup;
    strcpy(config->current_mode->name, "default");
    if (!(config->current_mode->keysym_bindings = create_list()))
        goto cleanup;
    if (!(config->current_mode->keycode_bindings = create_list()))
        goto cleanup;
    if (!(config->current_mode->mouse_bindings = create_list()))
        goto cleanup;
    if (!(config->current_mode->switch_bindings = create_list()))
        goto cleanup;
    list_add(config->modes, config->current_mode);

    config->floating_mod = 0;
    config->floating_mod_inverse = false;
    config->dragging_key = BTN_LEFT;
    config->resizing_key = BTN_RIGHT;

    if (!(config->floating_scroll_up_cmd = strdup("")))
        goto cleanup;
    if (!(config->floating_scroll_down_cmd = strdup("")))
        goto cleanup;
    if (!(config->floating_scroll_left_cmd = strdup("")))
        goto cleanup;
    if (!(config->floating_scroll_right_cmd = strdup("")))
        goto cleanup;
    if (!(config->font = strdup("monospace 10")))
        goto cleanup;
    config->urgent_timeout = 500;
    config->focus_on_window_activation = FOWA_URGENT;
    config->popup_during_fullscreen = POPUP_SMART;
    config->xwayland = XWAYLAND_MODE_LAZY;

    config->titlebar_border_thickness = 1;
    config->titlebar_h_padding = 5;
    config->titlebar_v_padding = 4;

    // floating view
    config->floating_maximum_width = 0;
    config->floating_maximum_height = 0;
    config->floating_minimum_width = 75;
    config->floating_minimum_height = 50;

    // Flags
    config->focus_follows_mouse = FOLLOWS_YES;
    config->focus_wrapping = WRAP_YES;
    config->validating = false;
    config->reloading = false;
    config->active = false;
    config->failed = false;
    config->reading = false;
    config->show_marks = true;
    config->title_align = ALIGN_LEFT;
    config->tiling_drag = true;
    config->tiling_drag_threshold = 9;

    if (!(config->active_bar_modifiers = create_list()))
        goto cleanup;

    if (!(config->haywardbg_command = strdup("haywardbg")))
        goto cleanup;

    if (!(config->config_chain = create_list()))
        goto cleanup;
    config->current_config_path = NULL;
    config->current_config = NULL;

    // borders
    config->border_thickness = 2;
    config->floating_border_thickness = 2;
    config->hide_edge_borders = E_NONE;

    config->has_focused_tab_title = false;

    // border colors
    color_to_rgba(config->border_colors.focused.border, 0x4C7899FF);
    color_to_rgba(config->border_colors.focused.background, 0x285577FF);
    color_to_rgba(config->border_colors.focused.text, 0xFFFFFFFF);
    color_to_rgba(config->border_colors.focused.indicator, 0x2E9EF4FF);

    color_to_rgba(config->border_colors.focused_inactive.border, 0x333333FF);
    color_to_rgba(config->border_colors.focused_inactive.background, 0x5F676AFF);
    color_to_rgba(config->border_colors.focused_inactive.text, 0xFFFFFFFF);
    color_to_rgba(config->border_colors.focused_inactive.indicator, 0x484E50FF);

    color_to_rgba(config->border_colors.unfocused.border, 0x333333FF);
    color_to_rgba(config->border_colors.unfocused.background, 0x222222FF);
    color_to_rgba(config->border_colors.unfocused.text, 0x888888FF);
    color_to_rgba(config->border_colors.unfocused.indicator, 0x292D2EFF);

    color_to_rgba(config->border_colors.urgent.border, 0x2F343AFF);
    color_to_rgba(config->border_colors.urgent.background, 0x900000FF);
    color_to_rgba(config->border_colors.urgent.text, 0xFFFFFFFF);
    color_to_rgba(config->border_colors.urgent.indicator, 0x900000FF);

    color_to_rgba(config->border_colors.placeholder.border, 0x000000FF);
    color_to_rgba(config->border_colors.placeholder.background, 0x0C0C0CFF);
    color_to_rgba(config->border_colors.placeholder.text, 0xFFFFFFFF);
    color_to_rgba(config->border_colors.placeholder.indicator, 0x000000FF);

    color_to_rgba(config->border_colors.background, 0xFFFFFFFF);

    // The keysym to keycode translation
    struct xkb_rule_names rules = {0};
    config->keysym_translation_state = keysym_translation_state_create(rules);

    return;
cleanup:
    hwd_abort("Unable to allocate config structures");
}

static bool
file_exists(const char *path) {
    return path && access(path, R_OK) != -1;
}

static char *
config_path(const char *prefix, const char *config_folder) {
    if (!prefix || !prefix[0] || !config_folder || !config_folder[0]) {
        return NULL;
    }

    const char *filename = "config";

    size_t size = 3 + strlen(prefix) + strlen(config_folder) + strlen(filename);
    char *path = calloc(size, sizeof(char));
    snprintf(path, size, "%s/%s/%s", prefix, config_folder, filename);
    return path;
}

static char *
get_config_path(void) {
    char *path = NULL;
    const char *home = getenv("HOME");
    char *config_home_fallback = NULL;

    const char *config_home = getenv("XDG_CONFIG_HOME");
    if ((config_home == NULL || config_home[0] == '\0') && home != NULL) {
        size_t size_fallback = 1 + strlen(home) + strlen("/.config");
        config_home_fallback = calloc(size_fallback, sizeof(char));
        if (config_home_fallback != NULL)
            snprintf(config_home_fallback, size_fallback, "%s/.config", home);
        config_home = config_home_fallback;
    }

    struct config_path {
        const char *prefix;
        const char *config_folder;
    };

    struct config_path config_paths[] = {
        {.prefix = home, .config_folder = ".hayward"},
        {.prefix = config_home, .config_folder = "hayward"},
        {.prefix = home, .config_folder = ".i3"},
        {.prefix = config_home, .config_folder = "i3"},
        {.prefix = SYSCONFDIR, .config_folder = "hayward"},
        {.prefix = SYSCONFDIR, .config_folder = "i3"}};

    size_t num_config_paths = sizeof(config_paths) / sizeof(config_paths[0]);
    for (size_t i = 0; i < num_config_paths; i++) {
        path = config_path(config_paths[i].prefix, config_paths[i].config_folder);
        if (!path) {
            continue;
        }
        if (file_exists(path)) {
            break;
        }
        free(path);
        path = NULL;
    }

    free(config_home_fallback);
    return path;
}

static bool
load_config(const char *path, struct hwd_config *config, struct haywardnag_instance *haywardnag) {
    if (path == NULL) {
        hwd_log(HWD_ERROR, "Unable to find a config file!");
        return false;
    }

    hwd_log(HWD_INFO, "Loading config from %s", path);

    struct stat sb;
    if (stat(path, &sb) == 0 && S_ISDIR(sb.st_mode)) {
        hwd_log(HWD_ERROR, "%s is a directory not a config file", path);
        return false;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        hwd_log(HWD_ERROR, "Unable to open %s for reading", path);
        return false;
    }

    bool config_load_success = read_config(f, config, haywardnag);
    fclose(f);

    if (!config_load_success) {
        hwd_log(HWD_ERROR, "Error(s) loading config!");
    }

    return config->active || !config->validating || config_load_success;
}

bool
load_main_config(const char *file, bool is_active, bool validating) {
    char *path;
    if (file != NULL) {
        path = strdup(file);
    } else {
        path = get_config_path();
    }
    if (path == NULL) {
        hwd_log(HWD_ERROR, "Cannot find config file");
        return false;
    }

    char *real_path = realpath(path, NULL);
    if (real_path == NULL) {
        hwd_log(HWD_ERROR, "%s not found", path);
        free(path);
        return false;
    }

    struct hwd_config *old_config = config;
    config = calloc(1, sizeof(struct hwd_config));
    if (!config) {
        hwd_abort("Unable to allocate config");
    }

    config_defaults(config);
    config->validating = validating;
    if (is_active) {
        hwd_log(
            HWD_DEBUG, "Performing configuration file %s", validating ? "validation" : "reload"
        );
        config->reloading = true;
        config->active = true;

        // xwayland can only be enabled/disabled at launch
        hwd_log(
            HWD_DEBUG, "xwayland will remain %s", old_config->xwayland ? "enabled" : "disabled"
        );
        config->xwayland = old_config->xwayland;

        if (!config->validating) {
            if (old_config->haywardbg_client != NULL) {
                wl_client_destroy(old_config->haywardbg_client);
            }

            if (old_config->haywardnag_config_errors.client != NULL) {
                wl_client_destroy(old_config->haywardnag_config_errors.client);
            }

            input_manager_reset_all_inputs();
        }
    }

    config->user_config_path = file ? true : false;
    config->current_config_path = path;
    list_add(config->config_chain, real_path);

    config->reading = true;

    // Read security configs
    // TODO: Security
    bool success = true;
    /*
    DIR *dir = opendir(SYSCONFDIR "/hayward/security.d");
    if (!dir) {
        hwd_log(HWD_ERROR,
            "%s does not exist, hayward will have no security configuration"
            " and will probably be broken", SYSCONFDIR "/hayward/security.d");
    } else {
        list_t *secconfigs = create_list();
        char *base = SYSCONFDIR "/hayward/security.d/";
        struct dirent *ent = readdir(dir);
        struct stat s;
        while (ent != NULL) {
            char *_path = malloc(strlen(ent->d_name) + strlen(base) + 1);
            strcpy(_path, base);
            strcat(_path, ent->d_name);
            lstat(_path, &s);
            if (S_ISREG(s.st_mode) && ent->d_name[0] != '.') {
                list_add(secconfigs, _path);
            }
            else {
                free(_path);
            }
            ent = readdir(dir);
        }
        closedir(dir);

        list_qsort(secconfigs, qstrcmp);
        for (int i = 0; i < secconfigs->length; ++i) {
            char *_path = secconfigs->items[i];
            if (stat(_path, &s) || s.st_uid != 0 || s.st_gid != 0 ||
                    (((s.st_mode & 0777) != 0644) &&
                    (s.st_mode & 0777) != 0444)) {
                hwd_log(HWD_ERROR,
                    "Refusing to load %s - it must be owned by root "
                    "and mode 644 or 444", _path);
                success = false;
            } else {
                success = success && load_config(_path, config);
            }
        }

        list_free_items_and_destroy(secconfigs);
    }
    */

    success = success && load_config(path, config, &config->haywardnag_config_errors);

    if (validating) {
        free_config(config);
        config = old_config;
        return success;
    }

    // Only really necessary if not explicitly `font` is set in the config.
    config_update_font_height();

    if (is_active && !validating) {
        input_manager_verify_fallback_seat();

        for (int i = 0; i < config->input_configs->length; i++) {
            input_manager_apply_input_config(config->input_configs->items[i]);
        }

        for (int i = 0; i < config->input_type_configs->length; i++) {
            input_manager_apply_input_config(config->input_type_configs->items[i]);
        }

        for (int i = 0; i < config->seat_configs->length; i++) {
            input_manager_apply_seat_config(config->seat_configs->items[i]);
        }
        hwd_switch_retrigger_bindings_for_all();

        reset_outputs();
        spawn_haywardbg();

        config->reloading = false;
        if (config->haywardnag_config_errors.client != NULL) {
            haywardnag_show(&config->haywardnag_config_errors);
        }
    }

    if (old_config) {
        destroy_removed_seats(old_config, config);
        free_config(old_config);
    }
    config->reading = false;
    return success;
}

static bool
load_include_config(
    const char *path, const char *parent_dir, struct hwd_config *config,
    struct haywardnag_instance *haywardnag
) {
    // save parent config
    const char *parent_config = config->current_config_path;

    char *full_path;
    int len = strlen(path);
    if (len >= 1 && path[0] != '/') {
        len = len + strlen(parent_dir) + 2;
        full_path = malloc(len * sizeof(char));
        if (!full_path) {
            hwd_log(HWD_ERROR, "Unable to allocate full path to included config");
            return false;
        }
        snprintf(full_path, len, "%s/%s", parent_dir, path);
    } else {
        full_path = strdup(path);
    }

    char *real_path = realpath(full_path, NULL);
    free(full_path);

    if (real_path == NULL) {
        hwd_log(HWD_DEBUG, "%s not found.", path);
        return false;
    }

    // check if config has already been included
    int j;
    for (j = 0; j < config->config_chain->length; ++j) {
        char *old_path = config->config_chain->items[j];
        if (strcmp(real_path, old_path) == 0) {
            hwd_log(HWD_DEBUG, "%s already included once, won't be included again.", real_path);
            free(real_path);
            return false;
        }
    }

    config->current_config_path = real_path;
    list_add(config->config_chain, real_path);
    int index = config->config_chain->length - 1;

    if (!load_config(real_path, config, haywardnag)) {
        free(real_path);
        config->current_config_path = parent_config;
        list_del(config->config_chain, index);
        return false;
    }

    // restore current_config_path
    config->current_config_path = parent_config;
    return true;
}

void
load_include_configs(
    const char *path, struct hwd_config *config, struct haywardnag_instance *haywardnag
) {
    char *wd = getcwd(NULL, 0);
    char *parent_path = strdup(config->current_config_path);
    const char *parent_dir = dirname(parent_path);

    if (chdir(parent_dir) < 0) {
        hwd_log(HWD_ERROR, "failed to change working directory");
        goto cleanup;
    }

    wordexp_t p;
    if (wordexp(path, &p, 0) == 0) {
        char **w = p.we_wordv;
        size_t i;
        for (i = 0; i < p.we_wordc; ++i) {
            load_include_config(w[i], parent_dir, config, haywardnag);
        }
        wordfree(&p);
    }

    // Attempt to restore working directory before returning.
    if (chdir(wd) < 0) {
        hwd_log(HWD_ERROR, "failed to change working directory");
    }
cleanup:
    free(parent_path);
    free(wd);
}

// get line, with backslash continuation
static ssize_t
getline_with_cont(char **lineptr, size_t *line_size, FILE *file, int *nlines) {
    char *next_line = NULL;
    size_t next_line_size = 0;
    ssize_t nread = getline(lineptr, line_size, file);
    *nlines = nread == -1 ? 0 : 1;
    while (nread >= 2 && strcmp(&(*lineptr)[nread - 2], "\\\n") == 0 && (*lineptr)[0] != '#') {
        ssize_t next_nread = getline(&next_line, &next_line_size, file);
        if (next_nread == -1) {
            break;
        }
        (*nlines)++;

        nread += next_nread - 2;
        if ((ssize_t)*line_size < nread + 1) {
            *line_size = nread + 1;
            char *old_ptr = *lineptr;
            *lineptr = realloc(*lineptr, *line_size);
            if (!*lineptr) {
                free(old_ptr);
                nread = -1;
                break;
            }
        }
        strcpy(&(*lineptr)[nread - next_nread], next_line);
    }
    free(next_line);
    return nread;
}

static int
detect_brace(FILE *file) {
    int ret = 0;
    int lines = 0;
    long pos = ftell(file);
    char *line = NULL;
    size_t line_size = 0;
    while ((getline(&line, &line_size, file)) != -1) {
        lines++;
        strip_whitespace(line);
        if (*line) {
            if (strcmp(line, "{") == 0) {
                ret = lines;
            }
            break;
        }
    }
    free(line);
    if (ret == 0) {
        fseek(file, pos, SEEK_SET);
    }
    return ret;
}

static char *
expand_line(const char *block, const char *line, bool add_brace) {
    int size = (block ? strlen(block) + 1 : 0) + strlen(line) + (add_brace ? 2 : 0) + 1;
    char *expanded = calloc(1, size);
    if (!expanded) {
        hwd_log(HWD_ERROR, "Cannot allocate expanded line buffer");
        return NULL;
    }
    snprintf(
        expanded, size, "%s%s%s%s", block ? block : "", block ? " " : "", line,
        add_brace ? " {" : ""
    );
    return expanded;
}

static bool
read_config(FILE *file, struct hwd_config *config, struct haywardnag_instance *haywardnag) {
    bool reading_main_config = false;
    char *this_config = NULL;
    size_t config_size = 0;
    if (config->current_config == NULL) {
        reading_main_config = true;

        int ret_seek = fseek(file, 0, SEEK_END);
        long ret_tell = ftell(file);
        if (ret_seek == -1 || ret_tell == -1) {
            hwd_log(HWD_ERROR, "Unable to get size of config file");
            return false;
        }
        config_size = ret_tell;
        rewind(file);

        config->current_config = this_config = calloc(1, config_size + 1);
        if (this_config == NULL) {
            hwd_log(HWD_ERROR, "Unable to allocate buffer for config contents");
            return false;
        }
    }

    bool success = true;
    int line_number = 0;
    char *line = NULL;
    size_t line_size = 0;
    ssize_t nread;
    list_t *stack = create_list();
    size_t read = 0;
    int nlines = 0;
    while ((nread = getline_with_cont(&line, &line_size, file, &nlines)) != -1) {
        if (reading_main_config) {
            if (read + nread > config_size) {
                hwd_log(HWD_ERROR, "Config file changed during reading");
                success = false;
                break;
            }

            strcpy(&this_config[read], line);
            read += nread;
        }

        if (line[nread - 1] == '\n') {
            line[nread - 1] = '\0';
        }

        line_number += nlines;
        hwd_log(HWD_DEBUG, "Read line %d: %s", line_number, line);

        strip_whitespace(line);
        if (!*line || line[0] == '#') {
            continue;
        }
        int brace_detected = 0;
        if (line[strlen(line) - 1] != '{' && line[strlen(line) - 1] != '}') {
            brace_detected = detect_brace(file);
            if (brace_detected > 0) {
                line_number += brace_detected;
                hwd_log(HWD_DEBUG, "Detected open brace on line %d", line_number);
            }
        }
        char *block = stack->length ? stack->items[0] : NULL;
        char *expanded = expand_line(block, line, brace_detected > 0);
        if (!expanded) {
            success = false;
            break;
        }
        config->current_config_line_number = line_number;
        config->current_config_line = line;
        struct cmd_results *res;
        char *new_block = NULL;
        if (block && strcmp(block, "<commands>") == 0) {
            // Special case
            res = config_commands_command(expanded);
        } else {
            res = config_command(expanded, &new_block);
        }
        switch (res->status) {
        case CMD_FAILURE:
        case CMD_INVALID:
            hwd_log(
                HWD_ERROR, "Error on line %i '%s': %s (%s)", line_number, line, res->error,
                config->current_config_path
            );
            if (!config->validating) {
                haywardnag_log(
                    config->haywardnag_command, haywardnag, "Error on line %i (%s) '%s': %s",
                    line_number, config->current_config_path, line, res->error
                );
            }
            success = false;
            break;

        case CMD_DEFER:
            hwd_log(HWD_DEBUG, "Deferring command `%s'", line);
            list_add(config->cmd_queue, strdup(expanded));
            break;

        case CMD_BLOCK_COMMANDS:
            hwd_log(HWD_DEBUG, "Entering commands block");
            list_insert(stack, 0, "<commands>");
            break;

        case CMD_BLOCK:
            hwd_log(HWD_DEBUG, "Entering block '%s'", new_block);
            list_insert(stack, 0, strdup(new_block));
            if (strcmp(new_block, "bar") == 0) {
                config->current_bar = NULL;
            }
            break;

        case CMD_BLOCK_END:
            if (!block) {
                hwd_log(HWD_DEBUG, "Unmatched '}' on line %i", line_number);
                success = false;
                break;
            }
            if (strcmp(block, "bar") == 0) {
                config->current_bar = NULL;
            }

            hwd_log(HWD_DEBUG, "Exiting block '%s'", block);
            list_del(stack, 0);
            free(block);
            memset(&config->handler_context, 0, sizeof(config->handler_context));
        default:;
        }
        free(new_block);
        free(expanded);
        free_cmd_results(res);
    }
    free(line);
    list_free_items_and_destroy(stack);
    config->current_config_line_number = 0;
    config->current_config_line = NULL;

    return success;
}

void
run_deferred_commands(void) {
    if (!config->cmd_queue->length) {
        return;
    }
    hwd_log(HWD_DEBUG, "Running deferred commands");
    while (config->cmd_queue->length) {
        char *line = config->cmd_queue->items[0];
        list_t *res_list = execute_command(line, NULL, NULL);
        for (int i = 0; i < res_list->length; ++i) {
            struct cmd_results *res = res_list->items[i];
            if (res->status != CMD_SUCCESS) {
                hwd_log(HWD_ERROR, "Error on line '%s': %s", line, res->error);
            }
            free_cmd_results(res);
        }
        list_del(config->cmd_queue, 0);
        list_free(res_list);
        free(line);
    }
}

void
run_deferred_bindings(void) {
    struct hwd_seat *seat;
    wl_list_for_each(seat, &(server.input->seats), link) {
        if (!seat->deferred_bindings->length) {
            continue;
        }
        hwd_log(HWD_DEBUG, "Running deferred bindings for seat %s", seat->wlr_seat->name);
        while (seat->deferred_bindings->length) {
            struct hwd_binding *binding = seat->deferred_bindings->items[0];
            seat_execute_command(seat, binding);
            list_del(seat->deferred_bindings, 0);
            free_hwd_binding(binding);
        }
    }
}

void
config_add_haywardnag_warning(char *fmt, ...) {
    if (config->reading && !config->validating) {
        va_list args;
        va_start(args, fmt);
        size_t length = vsnprintf(NULL, 0, fmt, args) + 1;
        va_end(args);

        char *temp = malloc(length + 1);
        if (!temp) {
            hwd_log(HWD_ERROR, "Failed to allocate buffer for warning.");
            return;
        }

        va_start(args, fmt);
        vsnprintf(temp, length, fmt, args);
        va_end(args);

        haywardnag_log(
            config->haywardnag_command, &config->haywardnag_config_errors,
            "Warning on line %i (%s) '%s': %s", config->current_config_line_number,
            config->current_config_path, config->current_config_line, temp
        );
    }
}

void
free_config(struct hwd_config *config) {
    if (!config) {
        return;
    }

    memset(&config->handler_context, 0, sizeof(config->handler_context));

    // TODO: handle all currently unhandled lists as we add implementations
    if (config->symbols) {
        for (int i = 0; i < config->symbols->length; ++i) {
            free_hwd_variable(config->symbols->items[i]);
        }
        list_free(config->symbols);
    }
    if (config->modes) {
        for (int i = 0; i < config->modes->length; ++i) {
            free_mode(config->modes->items[i]);
        }
        list_free(config->modes);
    }
    if (config->bars) {
        for (int i = 0; i < config->bars->length; ++i) {
            free_bar_config(config->bars->items[i]);
        }
        list_free(config->bars);
    }
    list_free(config->cmd_queue);
    if (config->output_configs) {
        for (int i = 0; i < config->output_configs->length; i++) {
            free_output_config(config->output_configs->items[i]);
        }
        list_free(config->output_configs);
    }
    if (config->haywardbg_client != NULL) {
        wl_client_destroy(config->haywardbg_client);
    }
    if (config->input_configs) {
        for (int i = 0; i < config->input_configs->length; i++) {
            free_input_config(config->input_configs->items[i]);
        }
        list_free(config->input_configs);
    }
    if (config->input_type_configs) {
        for (int i = 0; i < config->input_type_configs->length; i++) {
            free_input_config(config->input_type_configs->items[i]);
        }
        list_free(config->input_type_configs);
    }
    if (config->seat_configs) {
        for (int i = 0; i < config->seat_configs->length; i++) {
            free_seat_config(config->seat_configs->items[i]);
        }
        list_free(config->seat_configs);
    }
    list_free(config->no_focus);
    list_free(config->active_bar_modifiers);
    list_free_items_and_destroy(config->config_chain);
    free(config->floating_scroll_up_cmd);
    free(config->floating_scroll_down_cmd);
    free(config->floating_scroll_left_cmd);
    free(config->floating_scroll_right_cmd);
    free(config->font);
    free(config->haywardbg_command);
    free(config->haywardnag_command);
    free((char *)config->current_config_path);
    free((char *)config->current_config);
    keysym_translation_state_destroy(config->keysym_translation_state);
    free(config);
}

char *
do_var_replacement(char *str) {
    int i;
    char *find = str;
    while ((find = strchr(find, '$'))) {
        // Skip if escaped.
        if (find > str && find[-1] == '\\') {
            if (find == str + 1 || !(find > str + 1 && find[-2] == '\\')) {
                ++find;
                continue;
            }
        }
        // Unescape double $ and move on
        if (find[1] == '$') {
            size_t length = strlen(find + 1);
            memmove(find, find + 1, length);
            find[length] = '\0';
            ++find;
            continue;
        }
        // Find matching variable
        for (i = 0; i < config->symbols->length; ++i) {
            struct hwd_variable *var = config->symbols->items[i];
            int vnlen = strlen(var->name);
            if (strncmp(find, var->name, vnlen) == 0) {
                int vvlen = strlen(var->value);
                char *newstr = malloc(strlen(str) - vnlen + vvlen + 1);
                if (!newstr) {
                    hwd_log(
                        HWD_ERROR,
                        "Unable to allocate replacement "
                        "during variable expansion"
                    );
                    break;
                }
                char *newptr = newstr;
                int offset = find - str;
                strncpy(newptr, str, offset);
                newptr += offset;
                strncpy(newptr, var->value, vvlen);
                newptr += vvlen;
                strcpy(newptr, find + vnlen);
                free(str);
                str = newstr;
                find = str + offset + vvlen;
                break;
            }
        }
        if (i == config->symbols->length) {
            ++find;
        }
    }
    return str;
}

void
config_update_font_height(void) {
    int prev_max_height = config->font_height;

    get_text_metrics(config->font_description, &config->font_height, &config->font_baseline);

    if (config->font_height != prev_max_height) {
        arrange_root(root);
    }
}

static void
translate_binding_list(list_t *bindings, list_t *bindsyms, list_t *bindcodes) {
    for (int i = 0; i < bindings->length; ++i) {
        struct hwd_binding *binding = bindings->items[i];
        translate_binding(binding);

        switch (binding->type) {
        case BINDING_KEYSYM:
            binding_add_translated(binding, bindsyms);
            break;
        case BINDING_KEYCODE:
            binding_add_translated(binding, bindcodes);
            break;
        default:
            hwd_assert(false, "unexpected translated binding type: %d", binding->type);
            break;
        }
    }
}

void
translate_keysyms(struct input_config *input_config) {
    keysym_translation_state_destroy(config->keysym_translation_state);

    struct xkb_rule_names rules = {0};
    input_config_fill_rule_names(input_config, &rules);
    config->keysym_translation_state = keysym_translation_state_create(rules);

    for (int i = 0; i < config->modes->length; ++i) {
        struct hwd_mode *mode = config->modes->items[i];

        list_t *bindsyms = create_list();
        list_t *bindcodes = create_list();

        translate_binding_list(mode->keysym_bindings, bindsyms, bindcodes);
        translate_binding_list(mode->keycode_bindings, bindsyms, bindcodes);

        list_free(mode->keysym_bindings);
        list_free(mode->keycode_bindings);

        mode->keysym_bindings = bindsyms;
        mode->keycode_bindings = bindcodes;
    }

    hwd_log(HWD_DEBUG, "Translated keysyms using config for device '%s'", input_config->identifier);
}
