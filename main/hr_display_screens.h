/*
 * Screen layouts for the 160x80 display (docs/04 section 4), drawn into the
 * hr_gfx frame buffer. Plain C over hr_gfx and hr_ui - no FreeRTOS, no
 * hardware - so the layouts can be rendered on a host for a look.
 */
#ifndef HR_DISPLAY_SCREENS_H
#define HR_DISPLAY_SCREENS_H

#include "hr_ui_model.h"

/* Fixed redraw zones (top y, height), in frame-buffer rows. */
#define HR_ZONE_STATUS_Y   0
#define HR_ZONE_STATUS_H   10
#define HR_ZONE_TITLE_Y    10
#define HR_ZONE_TITLE_H    16
#define HR_ZONE_BIG_Y      26
#define HR_ZONE_BIG_H      24
#define HR_ZONE_PROGRESS_Y 50
#define HR_ZONE_PROGRESS_H 10
#define HR_ZONE_FOOTER_Y   60
#define HR_ZONE_FOOTER_H   20
#define HR_ZONE_COUNT      5

/* Zone `i` as (y, h). */
void hr_screens_zone(int i, int *y, int *h);

/*
 * Draw `screen` for this model and state into the frame buffer. Does not
 * flush. `adapter_fw` is our own version string for the BOOT / INFO
 * screens.
 */
void hr_screens_render(hr_ui_screen_t screen, const hr_ui_state_t *st,
                       const hr_ui_model_t *m, unsigned long now_ms,
                       const char *adapter_fw);

#endif /* HR_DISPLAY_SCREENS_H */
