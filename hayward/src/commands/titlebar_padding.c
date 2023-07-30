#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include <config.h>

#include <stdlib.h>

#include <hayward/commands.h>
#include <hayward/config.h>
#include <hayward/globals/root.h>
#include <hayward/tree/arrange.h>
#include <hayward/tree/root.h>
#include <hayward/tree/workspace.h>

struct cmd_results *
cmd_titlebar_padding(int argc, char **argv) {
    struct cmd_results *error = NULL;
    if ((error = checkarg(argc, "titlebar_padding", EXPECTED_AT_LEAST, 1))) {
        return error;
    }

    char *inv;
    int h_value = strtol(argv[0], &inv, 10);
    if (*inv != '\0' || h_value < 0 || h_value < config->titlebar_border_thickness) {
        return cmd_results_new(CMD_FAILURE, "Invalid size specified");
    }

    int v_value;
    if (argc == 1) {
        v_value = h_value;
    } else {
        v_value = strtol(argv[1], &inv, 10);
        if (*inv != '\0' || v_value < 0 || v_value < config->titlebar_border_thickness) {
            return cmd_results_new(CMD_FAILURE, "Invalid size specified");
        }
    }

    config->titlebar_v_padding = v_value;
    config->titlebar_h_padding = h_value;

    struct hwd_workspace *workspace = root_get_active_workspace(root);
    arrange_workspace(workspace);

    return cmd_results_new(CMD_SUCCESS, NULL);
}
