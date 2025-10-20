#include <stdio.h>

#include "app.h"
#include "params.h"
#include "util/log.h"

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    Params params;
    params_init_defaults(&params);

    if (!app_init(&params)) {
        LOG_ERROR("app_init failed; aborting");
        app_shutdown();
        return 1;
    }

    while (!app_should_quit()) {
        app_frame();
    }

    app_shutdown();
    return 0;
}
