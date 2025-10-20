#ifndef APP_H
#define APP_H

#include <stdbool.h>

#include "params.h"

bool app_init(const Params *params);
// Initializes platform first, then render. Returns false if initialization fails.

void app_frame(void);
// Executes one iteration of the main loop: pump input, render, swap buffers.

void app_shutdown(void);
// Shuts down render before platform; safe to call once after app_init.

bool app_should_quit(void);
// Returns true after the user has requested exit via input.

#endif  // APP_H
