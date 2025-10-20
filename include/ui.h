#ifndef UI_H
#define UI_H

#include <stdbool.h>

#include "params.h"
#include "platform.h"

typedef struct UiActions {
    bool toggle_pause;
    bool step_once;
    bool apply;
    bool reset;
    bool reinit_required;
} UiActions;

void ui_init(void);
void ui_shutdown(void);
void ui_sync_to_params(const Params *baseline, Params *runtime);
UiActions ui_update(const Input *input, bool sim_paused, float dt_sec);
void ui_render(int framebuffer_width, int framebuffer_height);
bool ui_wants_mouse(void);
bool ui_wants_keyboard(void);

#endif  // UI_H
