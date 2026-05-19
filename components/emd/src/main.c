/*
 * main.c — emd entry point.
 *
 * Parses CLI arguments and hands off to the supervisor.
 */

#include "emd/supervisor.h"
#include "emd/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_CONFIG "/etc/emd/emd.toml"

static void usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s [options]\n"
            "Options:\n"
            "  -c <path>    Config file (default: " DEFAULT_CONFIG ")\n"
            "  -h           Show this help\n"
            "  -v           Show version\n",
            argv0);
}

int main(int argc, char *argv[]) {
    const char *config_path = DEFAULT_CONFIG;

    int opt;
    while ((opt = getopt(argc, argv, "c:hv")) != -1) {
        switch (opt) {
        case 'c':
            config_path = optarg;
            break;
        case 'v':
            printf("emd version %s (commit %s)\n", EMD_VERSION, EMD_GIT_COMMIT);
            return 0;
        case 'h':
        default:
            usage(argv[0]);
            return opt == 'h' ? 0 : 1;
        }
    }

    /* Default logging before config is parsed */
    emd_log_set_level(EMD_LOG_INFO);

    return emd_supervisor_run(config_path);
}
