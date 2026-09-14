/*
 * The one button on the T-Dongle-S3 (GPIO0, labelled BOOT).
 *
 * Polled from the hr_ui task; produces SHORT (released before 1.5 s) and
 * LONG (held for 1.5 s) events. Both go to hr_ui_button() and nowhere else:
 * the button switches screens and toggles the backlight, and has no path -
 * direct or indirect - to hr_session or the USB transport. A stray press on
 * a stick sticking out of the machine must not be able to touch a batch.
 */
#ifndef HR_BUTTON_H
#define HR_BUTTON_H

#include <stdbool.h>

#include "hr_ui_model.h"

void hr_button_init(void);

/*
 * Sample the pin. Call every ~20 ms with the current uptime. Returns true
 * and fills *ev when a gesture completed. Ignores the pin for the first
 * 100 ms after boot (GPIO0 is the boot strapping pin and may still be held
 * from entering download mode).
 */
bool hr_button_poll(unsigned long now_ms, hr_ui_button_event_t *ev);

#endif /* HR_BUTTON_H */
