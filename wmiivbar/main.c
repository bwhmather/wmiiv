#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <getopt.h>
#include "wmiivbar/bar.h"
#include "ipc-client.h"
#include "log.h"

static struct wmiivbar wmiivbar;

void sig_handler(int signal) {
	wmiivbar.running = false;
}

int main(int argc, char **argv) {
	char *socket_path = NULL;
	bool debug = false;

	static const struct option long_options[] = {
		{"help", no_argument, NULL, 'h'},
		{"version", no_argument, NULL, 'v'},
		{"socket", required_argument, NULL, 's'},
		{"bar_id", required_argument, NULL, 'b'},
		{"debug", no_argument, NULL, 'd'},
		{0, 0, 0, 0}
	};

	const char *usage =
		"Usage: wmiivbar [options...]\n"
		"\n"
		"  -h, --help             Show help message and quit.\n"
		"  -v, --version          Show the version number and quit.\n"
		"  -s, --socket <socket>  Connect to wmiiv via socket.\n"
		"  -b, --bar_id <id>      Bar ID for which to get the configuration.\n"
		"  -d, --debug            Enable debugging.\n"
		"\n"
		" PLEASE NOTE that wmiivbar will be automatically started by wmiiv as\n"
		" soon as there is a 'bar' configuration block in your config file.\n"
		" You should never need to start it manually.\n";

	int c;
	while (1) {
		int option_index = 0;
		c = getopt_long(argc, argv, "hvs:b:d", long_options, &option_index);
		if (c == -1) {
			break;
		}
		switch (c) {
		case 's': // Socket
			socket_path = strdup(optarg);
			break;
		case 'b': // Type
			wmiivbar.id = strdup(optarg);
			break;
		case 'v':
			printf("wmiivbar version " WMIIV_VERSION "\n");
			exit(EXIT_SUCCESS);
			break;
		case 'd': // Debug
			debug = true;
			break;
		default:
			fprintf(stderr, "%s", usage);
			exit(EXIT_FAILURE);
		}
	}

	if (debug) {
		wmiiv_log_init(WMIIV_DEBUG, NULL);
	} else {
		wmiiv_log_init(WMIIV_INFO, NULL);
	}

	if (!wmiivbar.id) {
		wmiiv_log(WMIIV_ERROR, "No bar_id passed. "
				"Provide --bar_id or let wmiiv start wmiivbar");
		return 1;
	}

	if (!socket_path) {
		socket_path = get_socketpath();
		if (!socket_path) {
			wmiiv_log(WMIIV_ERROR, "Unable to retrieve socket path");
			return 1;
		}
	}

	if (!bar_setup(&wmiivbar, socket_path)) {
		free(socket_path);
		return 1;
	}

	free(socket_path);

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	wmiivbar.running = true;
	bar_run(&wmiivbar);
	bar_teardown(&wmiivbar);
	return 0;
}
