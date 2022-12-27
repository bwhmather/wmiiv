#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include "hayward/commands.h"

#include <stdlib.h>
#include <string.h>

#include <hayward-common/log.h>
#include <hayward-common/stringop.h>

#include <hayward/config.h>

#include <config.h>

struct cmd_results *
bar_cmd_status_command(int argc, char **argv) {
    struct cmd_results *error = NULL;
    if ((error = checkarg(argc, "status_command", EXPECTED_AT_LEAST, 1))) {
        return error;
    }
    free(config->current_bar->status_command);
    config->current_bar->status_command = NULL;

    char *new_command = join_args(argv, argc);
    if (strcmp(new_command, "-") != 0) {
        config->current_bar->status_command = new_command;
        hayward_log(
            HAYWARD_DEBUG, "Feeding bar with status command: %s",
            config->current_bar->status_command
        );
    } else {
        free(new_command);
    }
    return cmd_results_new(CMD_SUCCESS, NULL);
}
