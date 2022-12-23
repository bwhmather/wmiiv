#include "hayward/commands.h"

#include <string.h>
#include <strings.h>

#include <hayward-common/log.h>
#include <hayward-common/util.h>

#include <hayward/config.h>
#include <hayward/input/input-manager.h>

struct cmd_results *
input_cmd_tap(int argc, char **argv) {
    struct cmd_results *error = NULL;
    if ((error = checkarg(argc, "tap", EXPECTED_AT_LEAST, 1))) {
        return error;
    }
    struct input_config *ic = config->handler_context.input_config;
    if (!ic) {
        return cmd_results_new(CMD_FAILURE, "No input device defined.");
    }

    if (parse_boolean(argv[0], true)) {
        ic->tap = LIBINPUT_CONFIG_TAP_ENABLED;
    } else {
        ic->tap = LIBINPUT_CONFIG_TAP_DISABLED;
    }

    return cmd_results_new(CMD_SUCCESS, NULL);
}
